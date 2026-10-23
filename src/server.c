#include <arpa/inet.h>
#include <assert.h>
#include <bits/pthreadtypes.h>
#include <err.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define BUF_SIZE 1024

static _Atomic int32_t running = 1;
static void handle_sigint(int sig) {
    (void)sig;
    atomic_store_explicit(&running, 0, memory_order_relaxed);
}

static _Atomic uint64_t total_accepted;
static _Atomic uint64_t total_closed;
static atomic_long active;
static atomic_long peak;

static void stats_on_accept(void) {
    atomic_fetch_add_explicit(&total_accepted, 1, memory_order_relaxed);
    long now = atomic_fetch_add_explicit(&active, 1, memory_order_relaxed) + 1;

    long old = atomic_load_explicit(&peak, memory_order_relaxed);

    for (;;) {
        if (now <= old)
            break;

        if (atomic_compare_exchange_weak_explicit(
                &peak, &old, now, memory_order_relaxed, memory_order_relaxed))
            break;

        // On failure, old has been refreshed.
    }
}

static void stats_on_close(void) {
    atomic_fetch_add_explicit(&total_closed, 1, memory_order_relaxed);
    atomic_fetch_sub_explicit(&active, 1, memory_order_relaxed);
}

static void *stats_thread(void *arg) {
    (void)arg;
    while (atomic_load_explicit(&running, memory_order_relaxed)) {
        sleep(1);
        printf("active=%ld peak=%ld accepted=%llu closed=%llu\n",
               atomic_load_explicit(&active, memory_order_relaxed),
               atomic_load_explicit(&peak, memory_order_relaxed),
               (unsigned long long)atomic_load_explicit(&total_accepted,
                                                        memory_order_relaxed),
               (unsigned long long)atomic_load_explicit(&total_closed,
                                                        memory_order_relaxed));
    }

    return NULL;
}

static char *shift_args(int *argc, char ***argv) {
    assert((*argc) > 0);
    (*argc)--;

    return *(*argv)++;
}

static int send_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;

            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }

    return 0;
}

static void *handle_client(void *arg) {
    char echo_buf[BUF_SIZE];
    int peer_fd = (int)(intptr_t)arg;

    for (;;) {
        ssize_t bytes_read = recv(peer_fd, echo_buf, sizeof(echo_buf), 0);
        if (bytes_read < 0 && errno == EINTR) {
            continue;
        }

        if (bytes_read <= 0) {
            break;
        }

        if (send_all(peer_fd, echo_buf, (size_t)bytes_read) < 0) {
            break;
        }
    }

    close(peer_fd);
    stats_on_close();

    return NULL;
}

static void usage_and_exit(const char *prog) {
    fprintf(stderr, "usage: %s <address> <port>\n", prog);
    exit(EXIT_FAILURE);
}

static uint16_t parse_port(const char *raw_port) {
    char *end = NULL;

    errno = 0;
    long value = strtol(raw_port, &end, 10);
    if (errno != 0 || end == raw_port || *end != '\0' || value < 1 ||
        value > 65535) {
        errx(EXIT_FAILURE, "invalid port: %s", raw_port);
    }

    return (uint16_t)value;
}

static int create_listener(const char *address, uint16_t port) {
    int opt = 1;
    const int backlog = SOMAXCONN;
    struct sockaddr_storage serv_addr;
    socklen_t addrlen;

    memset(&serv_addr, 0, sizeof(serv_addr));

    struct sockaddr_in *sin = (struct sockaddr_in *)&serv_addr;
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&serv_addr;

    if (inet_pton(AF_INET, address, &sin->sin_addr) == 1) {
        sin->sin_family = AF_INET;
        sin->sin_port = htons(port);
        addrlen = sizeof(*sin);
    } else if (inet_pton(AF_INET6, address, &sin6->sin6_addr) == 1) {
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(port);
        addrlen = sizeof(*sin6);
    } else {
        errx(EXIT_FAILURE, "invalid address");
    }

    int fd = socket(serv_addr.ss_family, SOCK_STREAM, 0);
    if (fd < 0) {
        err(EXIT_FAILURE, "Open socket failed");
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        err(EXIT_FAILURE, "setsockopt failed");
    }

    int ret = bind(fd, (void *)&serv_addr, addrlen);
    if (ret < 0) {
        err(EXIT_FAILURE, "Bind failed");
    }

    ret = listen(fd, backlog);
    if (ret < 0) {
        err(EXIT_FAILURE, "Listen failed");
    }

    return fd;
}

static void install_sigint_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // no SA_RESTART, so accept() gets interrupted
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        err(EXIT_FAILURE, "sigaction failed");
    }
}

static void accept_loop(int listen_fd) {
    int peer_fd;
    while (atomic_load_explicit(&running, memory_order_relaxed)) {
        pthread_t t;
        peer_fd = accept(listen_fd, NULL, NULL);
        if (peer_fd < 0) {
            if (errno == ECONNABORTED || errno == EINTR || errno == EAGAIN) {
                continue;
            }
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS ||
                errno == ENOMEM) {
                warn("accept");
                sleep(1);
                continue;
            }

            else {
                err(EXIT_FAILURE, "Accept failed");
            }
        }

        stats_on_accept();

        sigset_t set, old;
        sigemptyset(&set);
        sigaddset(&set, SIGINT);
        pthread_sigmask(SIG_BLOCK, &set, &old);

        int rc =
            pthread_create(&t, NULL, handle_client, (void *)(intptr_t)peer_fd);
        pthread_sigmask(SIG_SETMASK, &old, NULL);

        if (rc != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(rc));
            close(peer_fd);
            stats_on_close();
            continue;
        }
        pthread_detach(t);
    }
}

static pthread_t start_telemetry(void) {
    pthread_t t;
    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, &old);

    int rc = pthread_create(&t, NULL, stats_thread, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);

    if (rc != 0) {
        fprintf(stderr, "stats pthread_create: %s\n", strerror(rc));
        exit(EXIT_FAILURE);
    }

    return t;
}

int main(int argc, char **argv) {

    const char *program_name = shift_args(&argc, &argv);
    if (argc < 2) {
        usage_and_exit(program_name);
    }
    const char *address = shift_args(&argc, &argv);
    uint16_t port = parse_port(shift_args(&argc, &argv));

    int listen_fd = create_listener(address, port);
    install_sigint_handler();
    pthread_t t = start_telemetry();

    printf("Listening to: %s:%u.\n", address, port);
    accept_loop(listen_fd);

    //
    pthread_join(t, NULL);

    close(listen_fd);
    return 0;
}