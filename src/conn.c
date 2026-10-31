#include "conn.h"
#include "loop.h"
#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUF_SIZE 1024

struct conn {
    struct loop *loop;
    int fd;

    char *write_buf;
    size_t w_queued;

    char *read_buf;
    size_t r_queued;

    struct conn_ops ops;
    void *conn_ctx;
};

struct listener {
    int fd;
    struct loop *loop;

    listener_accept_cb handler;
    void *listener_ctx;
};

static void io_signal_callback(struct loop *, int, uint32_t events, void *ud) {
    struct conn *c = (struct conn *)ud;
    if (events & (EPOLLERR | EPOLLHUP)) {
        conn_abort(c, ECONNRESET);
        return;
    }

    if (events & EPOLLIN) {
        ssize_t n = recv(c->fd, c->read_buf + c->r_queued, BUF_SIZE - c->r_queued, 0);
        if (n == 0) {
            conn_abort(c, 0);
            return;
        }
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                int err = errno;
                conn_abort(c, err);
            }
            return;
        }

        // socket: 1 2 3 4 5
        // app consumed: 123
        // queued: 4 5
        size_t total = c->r_queued + (size_t)n;
        size_t consumed = c->ops.on_read(c, c->read_buf, total, c->conn_ctx);
        memmove(c->read_buf, c->read_buf + consumed, total - consumed);
        c->r_queued = total - consumed;
    }

    if (events & EPOLLOUT) {

        ssize_t n = send(c->fd, c->write_buf, c->w_queued, 0);
        if (n == 0) {
            conn_abort(c, 0);
            return;
        }
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                int err = errno;
                conn_abort(c, err);
            }
            return;
        }

        // 1 2 3 4 5
        // write 1 2 3 -> pending = 2
        // 4 5
        size_t w = (size_t)n;
        size_t pending = c->w_queued - w;
        memmove(c->write_buf, c->write_buf + w, pending);
        c->w_queued = pending;

        if (c->w_queued == 0) {
            loop_io_mod(c->loop, c->fd, EPOLLIN);
        }
    }
}

static void new_conn_imcoming(struct loop *l, int, uint32_t events, void *ud) {

    if (events != EPOLLIN) {
        err(EXIT_FAILURE, "not implemented");
    }

    struct listener *lis = (struct listener *)(ud);
    int peer_fd = accept4(lis->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (peer_fd < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            // No connections are ready right now.
            // Simply return to event loop or do other work.
            return;
        } else {
            err(EXIT_FAILURE, "can't accept any more");
            return;
        }
    }

    struct conn *c = MALLOC(sizeof(struct conn));
    c->fd = peer_fd;
    c->loop = l;
    c->read_buf = MALLOC(BUF_SIZE * sizeof(char));
    c->write_buf = MALLOC(BUF_SIZE * sizeof(char));
    c->w_queued = 0;
    c->r_queued = 0;

    lis->handler(lis, c, lis->listener_ctx);
}

struct listener *listener_new(struct loop *l,
                              const char *address,
                              uint16_t port,
                              int backlog,
                              listener_accept_cb on_accept,
                              void *ud) {

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

    int fd = socket(addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
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
        .listener_ctx = ud,
    };

    bool ok = loop_io_add(lis->loop, new_conn_imcoming, lis->fd, EPOLLIN, lis);
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

void conn_start(struct conn *c, const struct conn_ops ops, void *ud) {
    c->ops = ops;
    c->conn_ctx = ud;

    bool ok = loop_io_add(c->loop, io_signal_callback, c->fd, EPOLLIN, c);
    if (!ok) {
        int err = errno;
        conn_abort(c, err);
    }
}

ssize_t conn_write(struct conn *c, const char *buf, size_t len) {

    if (c->w_queued + len > BUF_SIZE) {
        err(EXIT_FAILURE, "not implemented");
    }

    size_t queued = c->w_queued;
    c->w_queued += len;
    memcpy(c->write_buf + queued, buf, len);

    bool ok = loop_io_mod(c->loop, c->fd, EPOLLIN | EPOLLOUT);
    if (!ok) {
        err(EXIT_FAILURE, "not implemented");
    }

    return (ssize_t)len;
}

void conn_abort(struct conn *c, int err) {
    loop_io_remove(c->loop, c->fd);
    close(c->fd);

    void (*handler)(struct conn *, int err, void *ud) = c->ops.on_close;
    void *ctx = c->conn_ctx;
    handler(c, err, ctx);

    free(c->write_buf);
    free(c->read_buf);
    free(c);
}