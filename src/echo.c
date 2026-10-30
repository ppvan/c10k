// Echo server built on the epoll loop API (loop.h).

#include "loop.h"

#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUF_SIZE 4096
#define IDLE_TIMEOUT_MS 30000

struct config {
    const char *address;
    uint16_t port;
    int backlog;
    bool verbose;
};

struct conn {
    int fd;
    uint32_t events;  // what we currently have registered (EPOLLIN or EPOLLOUT)
    uint64_t timer;   // idle timer id, 0 = none
    size_t len, off;  // pending echo data: buf[off..len)
    char buf[BUF_SIZE];
};

static bool g_verbose;
static struct loop *g_loop;

/* ---------- connection lifecycle ---------- */

static void conn_close(struct loop *l, struct conn *c) {
    if (c->timer != 0)
        loop_timer_cancel(l, c->timer);
    loop_io_remove(l, c->fd); // must come before close(), or the fd number can be reused
    close(c->fd);
    if (g_verbose)
        fprintf(stderr, "closed fd=%d\n", c->fd);
    free(c);
}

static void on_idle(struct loop *l, void *ud) {
    struct conn *c = ud;
    c->timer = 0; // this timer already fired and its id is dead; don't cancel it
    if (g_verbose)
        fprintf(stderr, "idle timeout fd=%d\n", c->fd);
    conn_close(l, c);
}

// Called on every sign of life: restart the idle countdown.
static void conn_touch(struct loop *l, struct conn *c) {
    if (c->timer != 0)
        loop_timer_cancel(l, c->timer);
    c->timer = loop_timer_add(l, IDLE_TIMEOUT_MS, on_idle, c);
}

static void conn_set_events(struct loop *l, struct conn *c, uint32_t events) {
    if (c->events == events)
        return;
    if (!loop_io_mod(l, c->fd, events)) {
        conn_close(l, c);
        return;
    }
    c->events = events;
}

/* ---------- echo logic ---------- */

// Try to send pending data. Returns false if the connection was closed.
static bool conn_flush(struct loop *l, struct conn *c) {
    while (c->off < c->len) {
        ssize_t n = send(c->fd, c->buf + c->off, c->len - c->off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                conn_set_events(l, c, EPOLLOUT); // wait until writable
                return false; // either closed by set_events or just waiting
            }
            conn_close(l, c);
            return false;
        }
        c->off += (size_t)n;
    }

    // Everything echoed: go back to reading.
    c->off = c->len = 0;
    conn_set_events(l, c, EPOLLIN);
    return true;
}

static void on_client(struct loop *l, int fd, uint32_t events, void *ud) {
    (void)fd;
    struct conn *c = ud;

    if (events & (EPOLLERR | EPOLLHUP)) {
        conn_close(l, c);
        return;
    }

    if (events & EPOLLIN) {
        ssize_t n = recv(c->fd, c->buf, sizeof c->buf, 0);
        if (n == 0) { // peer closed its write side
            conn_close(l, c);
            return;
        }
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                conn_close(l, c);
            return;
        }
        c->off = 0;
        c->len = (size_t)n;
        conn_touch(l, c);
        conn_flush(l, c); // fast path: usually writes everything immediately
        return;
    }

    if (events & EPOLLOUT) {
        conn_touch(l, c);
        conn_flush(l, c);
    }
}

/* ---------- accepting ---------- */

static void on_accept(struct loop *l, int listen_fd, uint32_t events, void *ud) {
    (void)events;
    (void)ud;

    for (;;) {
        int fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            // EMFILE/ENFILE etc: level-triggered epoll will call us again
            // immediately, so this can spin until an fd frees up.
            fprintf(stderr, "accept: %s\n", strerror(errno));
            return;
        }

        struct conn *c = calloc(1, sizeof *c);
        if (c == NULL) {
            close(fd);
            return;
        }
        c->fd = fd;
        c->events = EPOLLIN;

        if (!loop_io_add(l, on_client, fd, EPOLLIN, c)) {
            fprintf(stderr, "loop_io_add: %s\n", strerror(errno));
            close(fd);
            free(c);
            continue;
        }
        conn_touch(l, c);
        if (g_verbose)
            fprintf(stderr, "accepted fd=%d\n", fd);
    }
}

/* ---------- setup ---------- */

static void on_signal(int sig) {
    (void)sig;
    if (g_loop)
        loop_stop(g_loop); // epoll_wait returns EINTR, loop sees the flag
}

static void usage(const char *prog, int status) {
    fprintf(status == EXIT_SUCCESS ? stdout : stderr,
            "usage: %s [-v] [-n backlog] <address> <port>\n"
            "  <address>    IPv4 or IPv6 address to bind (e.g. 127.0.0.1, ::1)\n"
            "  <port>       port number (1-65535)\n"
            "  -v           verbose output\n"
            "  -n <int>     listen() backlog (default SOMAXCONN = %d)\n"
            "  -h           show this help\n",
            prog, SOMAXCONN);
    exit(status);
}

static long parse_long(const char *raw, const char *what, long min, long max) {
    char *end = NULL;

    errno = 0;
    long value = strtol(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' || value < min || value > max)
        errx(EXIT_FAILURE, "invalid %s: '%s' (expected %ld-%ld)", what, raw, min,
             max);
    return value;
}

static int make_listener(const struct config *cfg) {
    struct sockaddr_storage addr;
    socklen_t addrlen;
    int one = 1;

    memset(&addr, 0, sizeof addr);
    struct sockaddr_in *sin = (struct sockaddr_in *)&addr;
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&addr;

    if (inet_pton(AF_INET, cfg->address, &sin->sin_addr) == 1) {
        sin->sin_family = AF_INET;
        sin->sin_port = htons(cfg->port);
        addrlen = sizeof *sin;
    } else if (inet_pton(AF_INET6, cfg->address, &sin6->sin6_addr) == 1) {
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(cfg->port);
        addrlen = sizeof *sin6;
    } else {
        errx(EXIT_FAILURE, "invalid address: '%s'", cfg->address);
    }

    int fd = socket(addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        err(EXIT_FAILURE, "socket");
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0)
        err(EXIT_FAILURE, "setsockopt(SO_REUSEADDR)");
    if (bind(fd, (struct sockaddr *)&addr, addrlen) < 0)
        err(EXIT_FAILURE, "bind %s:%u", cfg->address, cfg->port);
    if (listen(fd, cfg->backlog) < 0)
        err(EXIT_FAILURE, "listen");
    return fd;
}

int main(int argc, char **argv) {
    struct config cfg = {.backlog = SOMAXCONN};

    int opt;
    while ((opt = getopt(argc, argv, "hvn:")) != -1) {
        switch (opt) {
        case 'v':
            cfg.verbose = true;
            break;
        case 'n':
            cfg.backlog = (int)parse_long(optarg, "backlog", 1, INT_MAX);
            break;
        case 'h':
            usage(argv[0], EXIT_SUCCESS);
            break;
        default:
            usage(argv[0], EXIT_FAILURE);
        }
    }
    if (argc - optind != 2)
        usage(argv[0], EXIT_FAILURE);

    cfg.address = argv[optind];
    cfg.port = (uint16_t)parse_long(argv[optind + 1], "port", 1, 65535);
    g_verbose = cfg.verbose;

    int listen_fd = make_listener(&cfg);
    printf("Listening on %s:%u (backlog %d)\n", cfg.address, cfg.port, cfg.backlog);

    struct loop *l = loop_new();
    g_loop = l;

    struct sigaction sa = {.sa_handler = on_signal}; // no SA_RESTART on purpose
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (!loop_io_add(l, on_accept, listen_fd, EPOLLIN, NULL))
        err(EXIT_FAILURE, "loop_io_add(listener)");

    loop_run(l);

    // Open client connections are not tracked, so they are released by process
    // exit. loop_destroy frees the loop's own watchers and timers.
    g_loop = NULL;
    loop_destroy(l);
    close(listen_fd);
    return EXIT_SUCCESS;
}