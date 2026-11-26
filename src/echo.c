
#include "cli.h"
#include "conn.h"
#include "loop.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t count = 0;

struct http_server {
    bool verbose;
};


static size_t client_on_read(struct conn *c, const void *buf, size_t len, void *ud) {
    (void)ud;

    // Pause before writing in case on_drain fires synchronously.
    conn_read_pause(c);

    if (conn_write(c, buf, len) < 0) {
        conn_abort(c, errno ? errno : EIO);
    }

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
    count++;
    if (echo->verbose) {
        fprintf(stderr, "%d accepted\n", count);
    }
}

int main(int argc, char **argv) {
    struct cli_config cfg;
    cli_parse(&cfg, argc, argv, NULL);

    struct http_server echo = {
        .verbose = cfg.verbose,
    };

    struct loop *l = loop_new();
    struct listener *ln = listener_new(l, cfg.address, cfg.port, cfg.backlog, on_accept, &echo);

    if (ln == NULL) {
        err(EXIT_FAILURE, "listen %s:%u", cfg.address, cfg.port);
    }

    printf("Listening on %s:%u (backlog %d)\n", cfg.address, cfg.port, cfg.backlog);

    loop_run(l);

    listener_close(ln);
    loop_destroy(l);

    return EXIT_SUCCESS;
}
