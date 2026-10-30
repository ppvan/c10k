#include "conn.h"
#include "loop.h"
#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

struct conn {
    struct loop *loop;

    int fd;
};

struct listener {
    int fd;
    struct loop *loop;

    listener_accept_cb handler;
    void *user_data;
};

struct conn_ops {
    size_t (*on_read)(struct conn *, const void *buf, size_t len, void *ud);
    void (*on_close)(struct conn *, int err, void *ud);
};

typedef void (*listener_accept_cb)(struct listener *, struct conn *, void *ud);

static void on_accept_adapter(struct loop *l, int fd, uint32_t events,
                              void *ud) {

    (void)events;
    (void)fd;
    (void)l;

    struct listener *lis = (struct listener *)(ud);
    int peer_fd = accept4(lis->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (peer_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // No connections are ready right now.
            // Simply return to your event loop or do other work.
            return;
        } else {
            err(EXIT_FAILURE, "can't accept any more");
            return;
        }
    }

    struct conn *c = MALLOC(sizeof(struct conn));
    c->fd = peer_fd;
    c->loop = lis->loop;

    lis->handler(lis, c, lis->user_data);
}

struct listener *listener_new(struct loop* l, const char *address, uint16_t port, int backlog,
                              listener_accept_cb on_accept, void *ud) {

    struct sockaddr_storage addr;
    socklen_t addrlen;
    int one = 1;

    memset(&addr, 0, sizeof addr);
    struct sockaddr_in *sin = (struct sockaddr_in *)&addr;
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&addr;

    if (inet_pton(AF_INET, address, &sin->sin_addr) == 1) {
        sin->sin_family = AF_INET;
        sin->sin_port = htons(port);
        addrlen = sizeof *sin;
    } else if (inet_pton(AF_INET6, address, &sin6->sin6_addr) == 1) {
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(port);
        addrlen = sizeof *sin6;
    } else {
        return NULL;
    }

    int fd =
        socket(addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return NULL;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0) {
        close(fd);
        return NULL;
    }

    if (bind(fd, (struct sockaddr *)&addr, addrlen) < 0) {
        close(fd);
        return NULL;
    }

    if (listen(fd, backlog) < 0) {
        close(fd);
        return NULL;
    }

    struct listener *lis = MALLOC(sizeof(struct listener));
    *lis = (struct listener){
        .fd = fd,
        .handler = on_accept,
        .loop = l,
        .user_data = ud,
    };

    bool ok = loop_io_add(lis->loop, on_accept_adapter, lis->fd, EPOLLIN, lis);
    if (!ok) {
        close(lis->fd);
        free(lis);
        return NULL;
    }

    return lis;
}

void listener_close(struct listener *lis) {
    loop_io_remove(lis->loop, lis->fd);
    close(lis->fd);
    free(lis);
}

void conn_start(struct conn *, const struct conn_ops *, void *ud);
void conn_abort(struct conn *);