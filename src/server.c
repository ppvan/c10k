#include <arpa/inet.h>
#include <asm-generic/errno-base.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define BUF_SIZE 1024

static volatile sig_atomic_t running = 1;
static void handle_sigint(int sig) {
    (void)sig;
    running = 0;
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
        } else if (bytes_read > 0) {
            send_all(peer_fd, echo_buf, (size_t)bytes_read);
        }
    }
    close(peer_fd);

    return NULL;
}

static void usage_and_exit(const char *prog) {
    fprintf(stderr, "usage: %s <address> <port>\n", prog);
    exit(EXIT_FAILURE);
}

static uint16_t parse_port(const char *raw_port) {
    char *end = NULL;
    long value = strtol(raw_port, &end, 10);
    if (errno != 0 || end == raw_port || *end != '\0' || value < 1 ||
        value > 65535) {
        errx(EXIT_FAILURE, "invalid port: %s", raw_port);
    }

    return (uint16_t)value;
}

static int create_listener(const char *address, uint16_t port) {
    int opt = 1;

    struct sockaddr_in serv_addr;
    const int backlog = SOMAXCONN;
    

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        err(EXIT_FAILURE, "Open socket failed");
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        err(EXIT_FAILURE, "setsockopt failed");
    }

    memset(&serv_addr, 0, sizeof(struct sockaddr_in));
    serv_addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, address, &serv_addr.sin_addr) != 1) {
        errx(EXIT_FAILURE, "invalid address");
    }
    serv_addr.sin_port = htons(port);

    int ret = bind(fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
    if (ret < 0) {
        close(fd);
        err(EXIT_FAILURE, "Bind failed");
    }

    ret = listen(fd, backlog);
    if (ret < 0) {
        close(fd);
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
    struct sockaddr peer_addr;
    socklen_t peer_addr_len;

    int peer_fd;
    while (running) {
        pthread_t t;
        peer_addr_len = sizeof(peer_addr);
        memset(&peer_addr, 0, sizeof(struct sockaddr));
        memset(&t, 0, sizeof(pthread_t));

        peer_fd = accept(listen_fd, &peer_addr, &peer_addr_len);
        if (peer_fd < 0) {
            if (errno == ECONNABORTED || errno == EINTR || errno == EAGAIN) {
                continue;
            } else {
                err(EXIT_FAILURE, "Accept failed");
            }
        }

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
            continue;
        }
        pthread_detach(t);
    }
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

    printf("Listenning to: %s:%u.\n", address, port);
    accept_loop(listen_fd);

    close(listen_fd);
    return 0;
}