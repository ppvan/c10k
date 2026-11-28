/*
 * Static-file HTTP/1.1 server on top of conn.h / loop.h.
 * Protocol details (parsing, header helpers, MIME, response heads) live in http.c.
 *
 * Usage: http [-v] [-n backlog] [-r docroot] <address> <port>
 */
#include "cli.h"
#include "conn.h"
#include "http.h"
#include "loop.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PATH_MAX_LEN 4096
#define CHUNK_SIZE (64 * 1024)

struct http_server {
    const char *address;
    uint16_t port;
    int backlog;
    bool verbose;
    const char *root;
};

struct http_client {
    struct http_server *server;
    int file_fd;
    size_t remaining;
    bool keep;
};

static bool http_cli_handle(int opt, const char *arg, void *ud) {
    struct http_server *s = ud;

    if (opt != 'r')
        return false;

    char *resolved = realpath(arg, nullptr);
    if (resolved == nullptr)
        err(EXIT_FAILURE, "invalid document root '%s'", arg);
    s->root = resolved;
    return true;
}

static void send_simple(struct conn *c, enum http_code code, const char *type, const char *body, bool keep) {
    struct http_response *res = http_response_new();
    if (res == nullptr) {
        conn_abort(c, ENOMEM);
        return;
    }

    http_response_status(res, code);
    http_response_header(res, "Content-Type", type);
    http_response_header(res, "Connection", keep ? "keep-alive" : "close");
    http_response_body(res, body, strlen(body));

    const char *out;
    size_t out_len;
    if (http_response_build(res, &out, &out_len) == 0 && conn_write(c, out, out_len) == 0) {
        if (keep)
            conn_read_pause(c);
        else
            conn_close(c);
    } else {
        conn_abort(c, EIO);
    }
    http_response_free(res);
}

static void send_error(struct conn *c, enum http_code code, bool keep) {
    send_simple(c, code, "text/plain", http_code_reason(code), keep);
}

static int open_target(const struct http_server *s, struct slice target, char *path, size_t path_len, struct stat *st) {
    struct slice t = http_target_normalize(target);
    if (!http_target_is_safe(t))
        return -1;

    int n = snprintf(path, path_len, "%s%.*s", s->root, (int)t.n, t.p);
    if (n < 0 || (size_t)n >= path_len)
        return -1;

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    if (fstat(fd, st) < 0 || !S_ISREG(st->st_mode)) {
        close(fd);
        return -1;
    }
    return fd;
}

static void handle_request(struct conn *c, struct http_client *cl, const struct http_request *r) {
    cl->keep = http_wants_keep_alive(r);

    bool is_get = http_name_is(r->method, "GET");
    bool is_head = http_name_is(r->method, "HEAD");
    if (!is_get && !is_head) {
        send_error(c, HTTP_405_METHOD_NOT_ALLOWED, cl->keep);
        return;
    }

    char path[PATH_MAX_LEN];
    struct stat st;
    int fd = open_target(cl->server, r->target, path, sizeof path, &st);
    if (fd < 0) {
        send_error(c, HTTP_404_NOT_FOUND, cl->keep);
        return;
    }

    /* Pause so a pipelined request or a client FIN can't interfere (or close
     * the connection) while the response is streaming. */
    conn_read_pause(c);

    char head[512];
    int hn = http_file_head(head, sizeof head, http_mime_for(path), st.st_size, cl->keep);
    if (hn < 0 || conn_write(c, head, (size_t)hn) < 0) {
        close(fd);
        conn_abort(c, EIO);
        return;
    }

    if (is_head || st.st_size == 0) {
        close(fd);
        if (!cl->keep)
            conn_close(c);
        return;
    }

    cl->file_fd = fd;
    cl->remaining = (size_t)st.st_size;
}

static size_t client_on_read(struct conn *c, const void *buf, size_t len, void *ud) {
    struct http_client *cl = ud;
    struct http_request r;

    switch (http_parse(buf, len, &r)) {
    case HTTP_INCOMPLETE:
        if (len >= MAX_HEADER_SIZE) {
            send_error(c, HTTP_431_HEADER_FIELDS_TOO_LARGE, false);
            return len;
        }
        return 0;

    case HTTP_BAD:
        send_error(c, HTTP_400_BAD_REQUEST, false);
        return len;

    case HTTP_OK:
        handle_request(c, cl, &r);
        return r.head_len;
    }
    return len;
}

static void client_on_drain(struct conn *c, void *ud) {
    struct http_client *cl = ud;

    if (cl->file_fd < 0) {
        if (cl->keep)
            conn_read_resume(c);
        return;
    }

    char chunk[CHUNK_SIZE];
    size_t want = cl->remaining < sizeof chunk ? cl->remaining : sizeof chunk;
    ssize_t n = read(cl->file_fd, chunk, want);
    if (n <= 0) {
        conn_abort(c, n < 0 ? errno : EIO);
        return;
    }
    if (conn_write(c, chunk, (size_t)n) < 0) {
        conn_abort(c, EIO);
        return;
    }

    cl->remaining -= (size_t)n;
    if (cl->remaining == 0) {
        close(cl->file_fd);
        cl->file_fd = -1;
        if (!cl->keep)
            conn_close(c);
    }
}

static void client_on_close(struct conn *c, int err, void *ud) {
    (void)c;
    struct http_client *cl = ud;

    if (cl->server->verbose)
        fprintf(stderr, "closed: %s\n", err ? strerror(err) : "peer closed");

    if (cl->file_fd >= 0)
        close(cl->file_fd);
    free(cl);
}

static void on_accept(struct listener *ln, struct conn *c, void *ud) {
    (void)ln;

    static const struct conn_ops ops = {
        .on_read = client_on_read,
        .on_close = client_on_close,
        .on_drain = client_on_drain,
    };

    struct http_client *cl = MALLOC(sizeof *cl);
    *cl = (struct http_client){
        .server = ud,
        .file_fd = -1,
    };

    conn_start(c, ops, cl);
}

int main(int argc, char **argv) {
    struct http_server s = {.root = "."};

    const struct cli_ext ext = {
        .optstring = "r:",
        .help = "  -r <dir>     document root (default .)\n",
        .handle = http_cli_handle,
        .ud = &s,
    };

    struct cli_config cfg;
    cli_parse(&cfg, argc, argv, &ext);
    s.address = cfg.address;
    s.port = cfg.port;
    s.backlog = cfg.backlog;
    s.verbose = cfg.verbose;

    if (strcmp(s.root, ".") == 0) {
        char *resolved = realpath(".", nullptr);
        if (resolved == nullptr)
            err(EXIT_FAILURE, "realpath .");
        s.root = resolved;
    }

    struct loop *l = loop_new();
    struct listener *ln = listener_new(l, s.address, s.port, s.backlog, on_accept, &s);
    if (ln == nullptr)
        err(EXIT_FAILURE, "listen %s:%u", s.address, s.port);

    printf("Listening on %s:%u (backlog %d)\n", s.address, s.port, s.backlog);
    printf("Root: %s\n", s.root);

    loop_run(l);

    listener_close(ln);
    loop_destroy(l);
    return EXIT_SUCCESS;
}