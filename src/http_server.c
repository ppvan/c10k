
#include "cli.h"
#include "conn.h"
#include "loop.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "http.h"

struct http_server {
    const char *address;
    uint16_t port;
    int backlog;
    bool verbose;
    const char *root;
};

static bool http_cli_handle(int opt, const char *arg, void *ud) {
    struct http_server *s = ud;

    switch (opt) {
    case 'r':
        char *resolved = realpath(arg, nullptr);
        if (resolved == nullptr)
            err(EXIT_FAILURE, "invalid document root '%s'", arg);
        s->root = resolved;
        return true;
    default:
        return false;
    }
}


static char *read_file(const char *path, size_t *out_len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return nullptr;

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return nullptr;
    }

    char *data = malloc((size_t)st.st_size + 1);
    if (data == nullptr) {
        close(fd);
        return nullptr;
    }

    size_t off = 0;
    while (off < (size_t)st.st_size) {
        ssize_t n = read(fd, data + off, (size_t)st.st_size - off);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    close(fd);

    *out_len = off;
    return data;
}


static const struct { const char *ext, *type; } mime_types[] = {
    {".html", "text/html; charset=utf-8"},
    {".css",  "text/css; charset=utf-8"},
    {".js",   "text/javascript; charset=utf-8"},
    {".json", "application/json"},
    {".txt",  "text/plain; charset=utf-8"},
    {".svg",  "image/svg+xml"},
    {".png",  "image/png"},
    {".jpg",  "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif",  "image/gif"},
    {".ico",  "image/x-icon"},
    {".wasm", "application/wasm"},
    {".pdf",  "application/pdf"},
};

static const char *mime_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (dot != NULL) {
        for (size_t i = 0; i < ARRAY_LEN(mime_types); i++)
            if (strcasecmp(dot, mime_types[i].ext) == 0)
                return mime_types[i].type;
    }
    return "application/octet-stream";
}

static size_t client_on_read(struct conn *c, const void *buf, size_t len, void *ud) {
    struct http_server *s = ud; 
    struct http_request r;
    switch (http_parse(buf, len, &r)) {
    case HTTP_INCOMPLETE: {
        if (len >= MAX_HEADER_SIZE) {
            struct http_response *res = http_response_new();
            if (res == nullptr) {
                conn_abort(c, errno);
                return len;
            }

            http_response_status(res, HTTP_431_HEADER_FIELDS_TOO_LARGE);
            http_response_header(res, "Content-Type", "text/plain");
            http_response_header(res, "Connection", "close");
            http_response_body(res, "Request Header Fields Too Large", 31);

            const char *out;
            size_t out_len;
            if (http_response_build(res, &out, &out_len) == 0)
                conn_write(c, out, out_len);
            http_response_free(res);
            conn_close(c);

            return len;
        }
        return 0;
    }
    case HTTP_BAD: {
        struct http_response *res = http_response_new();
        http_response_status(res, HTTP_400_BAD_REQUEST);
        http_response_header(res, "Content-Type", "text/plain");
        http_response_header(res, "Connection", "close");
        http_response_body(res, "Bad Request", 5);

        const char *out;
        size_t out_len;
        if (http_response_build(res, &out, &out_len) == 0)
            conn_write(c, out, out_len);
        http_response_free(res);
        conn_close(c);

        return len;
    }
    case HTTP_OK: {
        struct slice t = r.target;
        // Drop query string
        for (size_t i = 0; i < t.n; i++) {
            if (t.p[i] == '?') {
                t.n = i;
                break;
            }
        }

        // "/" -> "/index.html"
        if (t.n == 1 && t.p[0] == '/')
            t = (struct slice){"/index.html", 11};

        char path[4096];
        int pn = snprintf(path, sizeof path, "%s%.*s", s->root, (int)t.n, t.p);

        size_t body_len = 0;
        char *body = (pn > 0 && (size_t)pn < sizeof path) ? read_file(path, &body_len) : nullptr;

        struct http_response *res = http_response_new();
        if (body != nullptr) {
            http_response_status(res, HTTP_200_OK);
            http_response_header(res, "Content-Type", mime_for(path));
            http_response_body(res, body, body_len);
        } else {
            http_response_status(res, HTTP_404_NOT_FOUND);
            http_response_header(res, "Content-Type", "text/plain");
            http_response_body(res, "Not Found", 9);
        }
        http_response_header(res, "Connection", "close");

        const char *out;
        size_t out_len;
        if (http_response_build(res, &out, &out_len) == 0)
            conn_write(c, out, out_len);
        http_response_free(res);
        free(body);
        conn_close(c);

        return len;
    }
    }
}

static void client_on_drain(struct conn *c, void *ud) {
    (void)ud;
    conn_read_resume(c);
}

static void client_on_close(struct conn *c, int err, void *ud) {
    (void)c;

    struct http_server *echo = ud;

    if (echo->verbose) {
        fprintf(stderr, "closed: %s\n", err ? strerror(err) : "peer closed");
    }
}

static void on_accept(struct listener *ln, struct conn *c, void *ud) {
    (void)ln;

    struct http_server *echo = ud;

    static const struct conn_ops ops = {
        .on_read = client_on_read,
        .on_close = client_on_close,
        .on_drain = client_on_drain,
    };

    conn_start(c, ops, echo);
}

int main(int argc, char **argv) {
    struct cli_config cfg;
    struct http_server s = {
        .root = ".",
    };

    const struct cli_ext ext = {
        .optstring = "r:",
        .help = "  -r <dir>     document root (default .)\n",
        .handle = http_cli_handle,
        .ud = &s,
    };

    cli_parse(&cfg, argc, argv, &ext);
    s.verbose = cfg.verbose;
    s.address = cfg.address;
    s.port = cfg.port;
    s.backlog = cfg.backlog;

    struct loop *l = loop_new();
    struct listener *ln = listener_new(l, s.address, s.port, s.backlog, on_accept, &s);

    if (ln == nullptr) {
        err(EXIT_FAILURE, "listen %s:%u", s.address, s.port);
    }

    printf("Listening on %s:%u (backlog %d)\n", s.address, s.port, s.backlog);
    printf("Root: %s\n", s.root);

    loop_run(l);

    listener_close(ln);
    loop_destroy(l);

    return EXIT_SUCCESS;
}
