
#include "cli.h"
#include "conn.h"
#include "loop.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct echo {
    bool verbose;
};

static struct loop *g_loop;


static size_t client_on_read(
    struct conn *c,
    const void *buf,
    size_t len,
    void *ud
) {
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

    struct echo *echo = ud;

    if (echo->verbose) {
        fprintf(
            stderr,
            "closed: %s\n",
            err ? strerror(err) : "peer closed"
        );
    }
}


static void on_accept(struct listener *ln, struct conn *c, void *ud) {
    (void)ln;

    struct echo *echo = ud;

    static const struct conn_ops ops = {
        .on_read = client_on_read,
        .on_close = client_on_close,
        .on_drain = client_on_drain,
    };

    conn_start(c, ops, echo);

    if (echo->verbose) {
        fprintf(stderr, "accepted\n");
    }
}


static void on_signal(int sig) {
    (void)sig;

    if (g_loop) {
        loop_stop(g_loop);
    }
}

int main(int argc, char **argv) {
    struct cli_config cfg;
    cli_parse(&cfg, argc, argv, NULL);

    struct loop *l = loop_new();
    g_loop = l;

    struct echo echo = {
        .verbose = cfg.verbose,
    };

    struct listener *ln = listener_new(
        l,
        cfg.address,
        cfg.port,
        cfg.backlog,
        on_accept,
        &echo
    );

    if (ln == NULL) {
        err(EXIT_FAILURE, "listen %s:%u", cfg.address, cfg.port);
    }

    printf(
        "Listening on %s:%u (backlog %d)\n",
        cfg.address,
        cfg.port,
        cfg.backlog
    );

    struct sigaction sa = {
        .sa_handler = on_signal,
    };

    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    loop_run(l);

    g_loop = NULL;
    listener_close(ln);
    loop_destroy(l);

    return EXIT_SUCCESS;
}
