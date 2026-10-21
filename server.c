#include <arpa/inet.h>
#include <assert.h>
#include <err.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#define BUF_SIZE 1024

char *shift_args(int *argc, char ***argv) {
    assert((*argc) > 0);
    (*argc)--;

    return *(*argv)++;
}

int main(int argc, char **argv) {
    const char *program_name;
    char *address;
    char *port_str;
    uint16_t port = 0;
    int fd = -1;
    struct sockaddr_in serv_addr;
    const int backlog = 1;
    int ret;
    int peer_fd;
    struct sockaddr peer_addr;
    socklen_t peer_addr_len;
    char buf[BUF_SIZE];
    ssize_t bytes_read = 0;
    ssize_t bytes_write = 0;
    int opt = 1;

    program_name = shift_args(&argc, &argv);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <address> <port>\n", program_name);
        exit(EXIT_FAILURE);
    }

    address = shift_args(&argc, &argv);
    port_str = shift_args(&argc, &argv);
    port = (uint16_t)atoi(port_str);

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
        err(EXIT_FAILURE, "Bind failed");
    }

    ret = listen(fd, backlog);
    if (ret < 0) {
        err(EXIT_FAILURE, "Listen failed");
    }
    printf("Listenning to: %s:%d.\n", address, port);

    peer_addr_len = sizeof(peer_addr);
    memset(&peer_addr, 0, sizeof(struct sockaddr));

    peer_fd = accept(fd, &peer_addr, &peer_addr_len);
    if (peer_fd < 0) {
        err(EXIT_FAILURE, "Accept failed");
    }

    bytes_read = recv(peer_fd, &buf, BUF_SIZE, 0);
    if (bytes_read < 0) {
        err(EXIT_FAILURE, "recv() failed");
    }

    bytes_write = send(peer_fd, &buf, (size_t)bytes_read, 0);
    if (bytes_write < 0) {
        err(EXIT_FAILURE, "send() failed");
    }

    return 0;
}