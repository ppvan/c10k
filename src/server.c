#include <arpa/inet.h>
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

char *shift_args(int *argc, char ***argv) {
    assert((*argc) > 0);
    (*argc)--;

    return *(*argv)++;
}

void *echo(void *arg) {
    int peer_fd = -1;
    ssize_t bytes_read = 0;
    ssize_t bytes_write = 0;
    size_t buf_size = 1024;
    char *echo_buf;

    peer_fd = (int)(intptr_t)arg;
    echo_buf = malloc(buf_size);

    if (echo_buf == NULL) {
        close(peer_fd);
        return NULL;
    }

    do {
        bytes_read = recv(peer_fd, echo_buf, buf_size, 0);

        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue; // Interrupted by a signal, try again safely
            }

            goto out;
        }

        if (bytes_read == 0) {
            printf("client fd=%d EOF\n", peer_fd);
            goto out;
        }

        bytes_write = 0;
        do {
            ssize_t written =
                send(peer_fd, echo_buf + bytes_write,
                     (size_t)(bytes_read - bytes_write), MSG_NOSIGNAL);
            if (written < 0) {
                if (errno == EINTR) {
                    continue; // Interrupted by a signal, try again safely
                }

                goto out;
            }

            if (written == 0) {
                goto out;
            }
            bytes_write += written;
        } while (bytes_write < bytes_read);

    } while (bytes_read != 0);

out:
    free(echo_buf);
    close(peer_fd);

    return NULL;
}

int main(int argc, char **argv) {
    const char *program_name;
    char *address;
    char *port_str;
    uint16_t port = 0;
    int fd = -1;
    struct sockaddr_in serv_addr;
    const int backlog = SOMAXCONN;
    int ret;
    struct sockaddr peer_addr;
    socklen_t peer_addr_len;

    int opt = 1;

    program_name = shift_args(&argc, &argv);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <address> <port>\n", program_name);
        exit(EXIT_FAILURE);
    }

    address = shift_args(&argc, &argv);
    port_str = shift_args(&argc, &argv);
    char *end;
    errno = 0;

    long value = strtol(port_str, &end, 10);

    if (errno != 0 || end == port_str || *end != '\0' || value < 1 ||
        value > 65535) {
        errx(EXIT_FAILURE, "invalid port: %s", port_str);
    }

    port = (uint16_t)value;
    fd = socket(AF_INET, SOCK_STREAM, 0);
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

    ret = bind(fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
    if (ret < 0) {
        close(fd);
        err(EXIT_FAILURE, "Bind failed");
    }

    ret = listen(fd, backlog);
    if (ret < 0) {
        close(fd);
        err(EXIT_FAILURE, "Listen failed");
    }
    printf("Listenning to: %s:%d.\n", address, port);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // no SA_RESTART, so accept() gets interrupted
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        close(fd);
        err(EXIT_FAILURE, "sigaction failed");
    }

    int peer_fd;
    while (running) {
        pthread_t t;
        peer_addr_len = sizeof(peer_addr);
        memset(&peer_addr, 0, sizeof(struct sockaddr));
        memset(&t, 0, sizeof(pthread_t));

        peer_fd = accept(fd, &peer_addr, &peer_addr_len);
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

        int rc = pthread_create(&t, NULL, echo, (void *)(intptr_t)peer_fd);
        pthread_sigmask(SIG_SETMASK, &old, NULL);

        if (rc != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(rc));
            close(peer_fd);
            continue;
        }
        pthread_detach(t);
    }

    close(fd);
    return 0;
}