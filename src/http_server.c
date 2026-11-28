
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

#include "http.h"

struct http_server {
    bool verbose;
};

static size_t client_on_read(struct conn *c, const void *buf, size_t len, void *ud) {
    (void)ud;
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
        struct http_response *res = http_response_new();
        http_response_status(res, HTTP_200_OK);
        http_response_header(res, "Content-Type", "text/plain");
        http_response_header(res, "Connection", "close");
        http_response_body(res, "OK", 2);

        const char *out;
        size_t out_len;
        if (http_response_build(res, &out, &out_len) == 0)
            conn_write(c, out, out_len);
        http_response_free(res);
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
    cli_parse(&cfg, argc, argv, nullptr);

    struct http_server s = {
        .verbose = cfg.verbose,
    };

    struct loop *l = loop_new();
    struct listener *ln = listener_new(l, cfg.address, cfg.port, cfg.backlog, on_accept, &s);

    if (ln == nullptr) {
        err(EXIT_FAILURE, "listen %s:%u", cfg.address, cfg.port);
    }

    printf("Listening on %s:%u (backlog %d)\n", cfg.address, cfg.port, cfg.backlog);

    loop_run(l);

    listener_close(ln);
    loop_destroy(l);

    return EXIT_SUCCESS;
}
