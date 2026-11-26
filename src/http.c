
#include "cli.h"
#include "conn.h"
#include "loop.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define DELIM_LEN 4
#define MAX_HEADER_SIZE 8192
const char *HTTP_HEADER_END = "\r\n\r\n";

struct http_server {
    bool verbose;
};

ssize_t header_end_index(const char *buf, size_t n) {
    // If buf is smaller than "\r\n\r\n", abort search.
    if (n < 4) {
        return -1;
    }

    for (size_t i = 0; i <= n - 4; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return (ssize_t)i;
        }
    }

    return -1;
}

static size_t client_on_read(struct conn *c, const void *buf, size_t len, void *ud) {
    (void)ud;

    ssize_t header_end = header_end_index(buf, len);
    if (header_end == -1) {
        return 0;
    }

    // Pause before writing in case on_drain fires synchronously.
    if (header_end >= MAX_HEADER_SIZE) {
        const char *resp = "HTTP/1.1 431 Request Header Fields Too Large\r\nContent-Length: 31\r\nContent-Type: "
                           "text/plain\r\nConnection: close\r\n\r\nRequest Header Fields Too Large";
        const size_t resp_len = strlen(resp);

        conn_read_pause(c);
        if (conn_write(c, resp, resp_len) < 0) {
            conn_abort(c, errno ? errno : EIO);
        }
        conn_close(c);

        return len;
    }

    const char *resp =
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nhello";
    const size_t resp_len = strlen(resp);

    conn_read_pause(c);
    if (conn_write(c, resp, resp_len) < 0) {
        conn_abort(c, errno ? errno : EIO);
    }
    conn_close(c);

    return len;
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
    cli_parse(&cfg, argc, argv, NULL);

    struct http_server s = {
        .verbose = cfg.verbose,
    };

    struct loop *l = loop_new();
    struct listener *ln = listener_new(l, cfg.address, cfg.port, cfg.backlog, on_accept, &s);

    if (ln == NULL) {
        err(EXIT_FAILURE, "listen %s:%u", cfg.address, cfg.port);
    }

    printf("Listening on %s:%u (backlog %d)\n", cfg.address, cfg.port, cfg.backlog);

    loop_run(l);

    listener_close(ln);
    loop_destroy(l);

    return EXIT_SUCCESS;
}
