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
    char *buf;
    size_t len;
    size_t offset;
};

static void usage(const char *prog, int status);
static long parse_long(const char *raw, const char *what, long min, long max);
static int make_listener(const struct config *cfg);
static void poll_loop(int listen_fd, const struct config *cfg);

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

    // install_sigint_handler();
    poll_loop(listen_fd, &cfg);

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

static void poll_loop(int listen_fd, const struct config *cfg) {
    // 1. listen to listen fd for readable signal
    // 2. If new signal in, accept()
    //    alloc new buf, fd to new client
    // ....keep  the client somewhere
    // 3. polling
    // 4. If client is readable, read it as much as you can to its buf, subcribe
    // to writes on this socket
    // 5. If client is writeable, write as much as you can, if done -> unsub
    // write event
    // 6. What if we did not complete write yet, but client keep send new data?
    //   Trust TCP we retransmit, since we don't  read TCP would resend it or in
    //   the buffer somewhere, don't care about this case yet.

    // Need an array of pollfd. Hard cap to 100 cons array for simplicity,
    // dynamic array later.
    nfds_t nfds = 100;
    nfds_t count = 1;
    struct pollfd *fds = malloc(nfds * sizeof(struct pollfd));
    struct connection_t *connections =
        malloc((nfds - 1) * sizeof(struct connection_t));
    struct timespec tv;
    tv.tv_sec = 1;

    fds[0].events = POLLIN;
    fds[0].fd = listen_fd;

    while (true) {
        int active_fds = ppoll(fds, count, &tv, NULL);
        switch (active_fds) {
        case 0:
            if (cfg->verbose) {
                printf("ppoll timeout\n");
            }
            break;
        case -1:
            err(EXIT_FAILURE, "ppoll");
            break;
        default:
            if (fds[0].revents & POLLERR) {
                err(EXIT_FAILURE, "listen_fd err");
            }
            if (fds[0].revents & POLLIN) {
                int peer_fd = accept(listen_fd, NULL, NULL);
                if (count >= nfds) {
                    err(EXIT_FAILURE, "Too many connection");
                }

                connections[count - 1].buf = malloc(BUF_SIZE);
                connections[count - 1].len = BUF_SIZE;
                connections[count - 1].offset = 0;

                fds[count].events = POLLIN;
                fds[count].fd = peer_fd;

                count++;
                continue;
            }

            for (size_t i = 1; i < count; i++) {

                if (fds[0].revents & POLLERR) {
                    err(EXIT_FAILURE, "peer fd err");
                }

                if (fds[i].revents & POLLIN) {
                    ssize_t rlen = recv(fds[i].fd, connections[i - 1].buf,
                                        connections[i - 1].len, 0);
                    if (rlen <= 0) {
                        fprintf(stderr, "client read error\n");
                        close(fds[i].fd);
                        exit(1);
                    }

                    connections[i - 1].len = (size_t)rlen;
                    connections[i - 1].offset = 0;
                    fds[i].events = POLLOUT;
                }

                if (fds[i].revents & POLLOUT) {

                    if (connections[i - 1].offset < connections[i - 1].len) {
                        ssize_t wlen = send(fds[i].fd,
                                            connections[i - 1].buf +
                                                connections[i - 1].offset,
                                            connections[i - 1].len, 0);
                        if (wlen <= 0) {
                            fprintf(stderr, "client read error\n");
                            close(fds[i].fd);
                            exit(1);
                        }
                        connections[i - 1].offset += (size_t)wlen;
                    } else {
                        connections[i - 1].offset = 0;
                        fds[i].events = POLLIN;
                    }
                }
            }
            break;
        }
    }
}