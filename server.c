#include <stddef.h>
#include <netinet/in.h>
#include <stdio.h>
   #include <err.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

int main(void) {

    int fd = -1;
    struct sockaddr_in serv_addr;
    uint16_t port = 8080;
    const int backlog = 1;
    int ret;
    int peer_fd;
    struct sockaddr peer_addr;
    socklen_t peer_addr_len;
    size_t buf_size = 1024;
    char buf[buf_size];
    ssize_t bytes_read = 0;
    ssize_t bytes_write = 0;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        err(EXIT_FAILURE, "Open socket failed");
    }

    memset(&serv_addr, 0, sizeof(struct sockaddr_in));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    ret = bind(fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
    if (ret < 0) {
        err(EXIT_FAILURE, "Bind failed");
    }

    ret = listen(fd, backlog);
     if (ret < 0) {
        err(EXIT_FAILURE, "Listen failed");
    }
    printf("Listenning to..%s:%d.\n", "0.0.0.0", port);

    peer_addr_len = sizeof(peer_addr);
    memset(&peer_addr, 0, sizeof(struct sockaddr));

    peer_fd = accept(fd, &peer_addr, &peer_addr_len);
    if (peer_fd < 0) {
        err(EXIT_FAILURE, "Accept failed");
    }

    bytes_read = recv(peer_fd, &buf, buf_size, 0);
    if (bytes_read < 0) {
        err(EXIT_FAILURE, "recv() failed");
    }

    bytes_write = send(peer_fd, &buf, (size_t)bytes_read, 0);
    if (bytes_write < 0) {
        err(EXIT_FAILURE, "send() failed");
    }

    return 0;
}