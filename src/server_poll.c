#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define BUF_SIZE 1024

struct config {
    const char *address;
    uint16_t port;
    int backlog;
    bool verbose;
};

struct connection_t {
    int fd;
    char buf[BUF_SIZE];
    size_t len, off;
};

struct loop;
typedef void (*event_cb)(struct loop *l, void *ctx, short revents);

struct handler {
    event_cb cb;
    void *ctx;
};

struct loop {
    struct pollfd *fds;
    struct handler *handlers;
    size_t *slot_of; // slot_of[fd] = index into fds/handlers

    nfds_t count;
    nfds_t cap;
};

static struct loop *loop_init(struct config cfg) {
    struct loop *l = malloc(sizeof(struct loop));
    l->cap = 256;
    l->count = 0;
    l->fds = malloc(sizeof(struct pollfd) * l->cap);
    l->handlers = malloc(sizeof(struct handler) * l->cap);

    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) == -1) {
        err(EXIT_FAILURE, "can't get rlimit");
    }
    if (cfg.verbose) {
        printf("file limit: %lu\n", limit.rlim_cur);
    }

    l->slot_of = malloc(limit.rlim_cur * sizeof(size_t));
    memset(l->slot_of, 0, limit.rlim_cur * sizeof(size_t));

    return l;
}

static size_t loop_add(struct loop *l, int fd, event_cb cb, short events,
                       void *ctx) {

    if (l->count >= l->cap) {
        l->cap = l->cap * 2;
        l->fds = realloc(l->fds, sizeof(struct pollfd) * l->cap);
        l->handlers = realloc(l->handlers, sizeof(struct handler) * l->cap);
    }

    size_t idx = l->count++;
    l->fds[idx].events = events;
    l->fds[idx].fd = fd;
    l->slot_of[fd] = idx;

    l->handlers[idx].cb = cb;
    l->handlers[idx].ctx = ctx;

    return idx;
}

static void loop_set_events(struct loop *l, int fd, short events) {
    l->fds[l->slot_of[fd]].events = events;
}

static void loop_remove(struct loop *l, int fd) {

    size_t idx = l->slot_of[fd];
    size_t last = l->count - 1;
    if (idx != last) {
        l->fds[idx] = l->fds[last];
        l->handlers[idx] = l->handlers[last];
        l->slot_of[l->fds[idx].fd] = idx;
    }
    l->count--;

    if (l->count <= l->cap / 4 && l->cap > 256) {
        l->cap = l->cap / 2;
        l->fds = realloc(l->fds, sizeof(struct pollfd) * l->cap);
        l->handlers = realloc(l->handlers, sizeof(struct handler) * l->cap);
    }
}

void conn_close(struct loop *l, struct connection_t *c) {
    loop_remove(l, c->fd);
    close(c->fd);
    free(c);
}

static void loop_run(struct loop *l) {
    struct timespec tv = {.tv_sec = 1};
    for (;;) {
        int active_fds = ppoll(l->fds, l->count, &tv, NULL);
        if (active_fds < 0) {
            err(EXIT_FAILURE, "ppoll");
        }

        // Dispatch backwards, so remove not skip dispatch
        ssize_t n = (ssize_t)l->count;
        for (ssize_t i = n - 1; i >= 0; i--) {
            if (l->fds[i].revents) {
                l->handlers[i].cb(l, l->handlers[i].ctx, l->fds[i].revents);
            }
        }
    }
}

static void on_client(struct loop *l, void *ctx, short revents) {
    struct connection_t *c = (struct connection_t *)ctx;

    if (revents & (POLLHUP | POLLERR)) {
        conn_close(l, c);
    } else if (revents & POLLIN) {
        ssize_t ret = recv(c->fd, c->buf, sizeof(c->buf), 0);
        if (ret <= 0) {
            conn_close(l, c);
            return;
        }

        c->off = 0;
        c->len = (size_t)ret;
        loop_set_events(l, c->fd, POLLOUT);
    } else if (revents & POLLOUT) {
        ssize_t ret = send(c->fd, c->buf + c->off, (size_t)(c->len - c->off),
                           MSG_NOSIGNAL);
        if (ret <= 0) {
            conn_close(l, c);
            return;
        }

        c->off += (size_t)ret;
        if (c->off == c->len) {
            loop_set_events(l, c->fd, POLLIN);
        }
    }
}

void on_accept(struct loop *l, void *ctx, short revents) {
    (void)revents;
    int listen_fd = (int)(intptr_t)ctx;

    while (true) {
        int peer_fd =
            accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (peer_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No connections are ready right now.
                // Simply return to your event loop or do other work.
                return;
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EMFILE) {
                fprintf(stderr, "too many open files");
                return;
            } else {
                err(EXIT_FAILURE, "can't accept any more");
                return;
            }
        }
        struct connection_t *conn = calloc(sizeof(struct connection_t), 1);

        conn->fd = peer_fd;
        loop_add(l, peer_fd, on_client, POLLIN, conn);
    }
}

static void usage(const char *prog, int status);
static long parse_long(const char *raw, const char *what, long min, long max);
static int make_listener(const struct config *cfg);

int main(int argc, char **argv) {
    struct config cfg = {
        .backlog = SOMAXCONN,
        .verbose = false,
    };

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
        default: /* '?' — getopt already printed the error */
            usage(argv[0], EXIT_FAILURE);
        }
    }

    /* Exactly two positional args must remain: <address> <port> */
    if (argc - optind != 2)
        usage(argv[0], EXIT_FAILURE);

    cfg.address = argv[optind];
    cfg.port = (uint16_t)parse_long(argv[optind + 1], "port", 1, 65535);

    int listen_fd = make_listener(&cfg);

    printf("Listening on %s:%u (backlog %d)%s\n", cfg.address, cfg.port,
           cfg.backlog, cfg.verbose ? " [verbose]" : "");

    struct loop *l = loop_init(cfg);
    loop_add(l, listen_fd, on_accept, POLLIN, (void *)(intptr_t)listen_fd);
    loop_run(l);
    close(listen_fd);

    return EXIT_SUCCESS;
}

static void usage(const char *prog, int status) {
    fprintf(
        status == EXIT_SUCCESS ? stdout : stderr,
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
    if (errno != 0 || end == raw || *end != '\0' || value < min ||
        value > max) {
        errx(EXIT_FAILURE, "invalid %s: '%s' (expected %ld-%ld)", what, raw,
             min, max);
    }
    return value;
}

static int make_listener(const struct config *cfg) {
    struct sockaddr_storage serv_addr;
    socklen_t addrlen;
    int one = 1;

    memset(&serv_addr, 0, sizeof(serv_addr));

    struct sockaddr_in *sin = (struct sockaddr_in *)&serv_addr;
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&serv_addr;

    if (inet_pton(AF_INET, cfg->address, &sin->sin_addr) == 1) {
        sin->sin_family = AF_INET;
        sin->sin_port = htons(cfg->port);
        addrlen = sizeof(*sin);
    } else if (inet_pton(AF_INET6, cfg->address, &sin6->sin6_addr) == 1) {
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(cfg->port);
        addrlen = sizeof(*sin6);
    } else {
        errx(EXIT_FAILURE, "invalid address: '%s'", cfg->address);
    }

    int fd = socket(serv_addr.ss_family,
                    SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        err(EXIT_FAILURE, "socket");

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
        err(EXIT_FAILURE, "setsockopt(SO_REUSEADDR)");

    if (bind(fd, (struct sockaddr *)&serv_addr, addrlen) < 0)
        err(EXIT_FAILURE, "bind %s:%u", cfg->address, cfg->port);

    if (listen(fd, cfg->backlog) < 0)
        err(EXIT_FAILURE, "listen");

    if (cfg->verbose)
        fprintf(stderr, "listener fd=%d bound to %s:%u\n", fd, cfg->address,
                cfg->port);

    return fd;
}
