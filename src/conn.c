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
    uint32_t events;
    bool started;   /* registered with the loop */

    char *write_buf;
    size_t w_queued;

    char *read_buf;
    size_t r_queued;

    bool closing;
    int close_err;
    uint64_t reap_id;

    uint64_t deliver_id;
    struct conn_ops ops;
    void *conn_ctx;
};

struct listener {
    int fd;
    struct loop *loop;

    listener_accept_cb handler;
    void *listener_ctx;
};

static bool sync_events(struct conn *c) {
    if (!c->started)
        return true;
    return loop_io_mod(c->loop, c->fd, c->events);
}

static void deliver(struct conn *c) {
    if (c->closing || c->r_queued == 0 || !(c->events & EPOLLIN))
        return;

    size_t n = c->r_queued;
    size_t consumed = c->ops.on_read(c, c->read_buf, n, c->conn_ctx);
    if (c->closing)
        return;

    if (consumed > n) {
        conn_abort(c, EPROTO);
        return;
    }

    memmove(c->read_buf, c->read_buf + consumed, n - consumed);
    c->r_queued = n - consumed;
}

static void deliver_cb(struct loop *, void *ud) {
    struct conn *c = ud;
    c->deliver_id = 0;
    deliver(c);
}

static void schedule_deliver(struct conn *c) {
    if (c->closing || c->deliver_id || c->r_queued == 0)
        return;
    c->deliver_id = loop_timer_add(c->loop, 0, deliver_cb, c);
}

static void handle_readable(struct conn *c) {
    if (c->r_queued == BUF_SIZE) {
        deliver(c);
        if (c->closing)
            return;
        if (c->r_queued == BUF_SIZE) {
            // Buffer still full. If the app paused inside on_read this is
            // legitimate backpressure; otherwise it broke the contract.
            // Either way never recv() with length 0 (it looks like EOF).
            if (c->events & EPOLLIN)
                conn_abort(c, EMSGSIZE);
            return;
        }
    }

    ssize_t n = recv(c->fd, c->read_buf + c->r_queued, BUF_SIZE - c->r_queued, 0);
    if (n == 0) {
        conn_abort(c, 0);
        return;
    }
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            int err = errno;
            conn_abort(c, err);
        }
        return;
    }

    c->r_queued += (size_t)n;
    schedule_deliver(c);
}

static void handle_writable(struct conn *c) {
    if (c->w_queued == 0) {
        c->events &= ~EPOLLOUT;
        sync_events(c);
        return;
    }

    ssize_t n = send(c->fd, c->write_buf, c->w_queued, MSG_NOSIGNAL);
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            int err = errno;
            conn_abort(c, err);
        }
        return;
    }

    size_t w = (size_t)n;
    size_t pending = c->w_queued - w;
    memmove(c->write_buf, c->write_buf + w, pending);
    c->w_queued = pending;

    if (c->w_queued == 0) {
        c->events &= ~EPOLLOUT;
        if (c->ops.on_drain != nullptr) {
            c->ops.on_drain(c, c->conn_ctx);
        }
        if (c->closing)
            return;
        sync_events(c);
    }
}

static void io_signal_callback(struct loop *, int, uint32_t events, void *ud) {
    struct conn *c = (struct conn *)ud;
    if (c->closing)
        return; // stale event from the same epoll batch

    if (events & (EPOLLERR | EPOLLHUP)) {
        conn_abort(c, ECONNRESET);
        return;
    }

    if (events & EPOLLIN) {
        handle_readable(c);
        if (c->closing)
            return;
    }

    if (events & EPOLLOUT) {
        handle_writable(c);
    }
}

static void new_conn_incoming(struct loop *l, int, uint32_t, void *ud) {
    struct listener *lis = (struct listener *)(ud);
    int peer_fd = accept4(lis->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (peer_fd < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED) {
            // Nothing ready, or the peer gave up before we accepted.
            return;
        }
        err(EXIT_FAILURE, "can't accept any more");
    }

    struct conn *c = MALLOC(sizeof(struct conn));
    *c = (struct conn){};
    c->fd = peer_fd;
    c->loop = l;
    c->read_buf = MALLOC(BUF_SIZE * sizeof(char));
    c->write_buf = MALLOC(BUF_SIZE * sizeof(char));

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

    bool ok = loop_io_add(lis->loop, new_conn_incoming, lis->fd, EPOLLIN, lis);
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
    if (c->closing || c->started)
        return;

    c->ops = ops;
    c->conn_ctx = ud;
    c->events |= EPOLLIN;

    bool ok = loop_io_add(c->loop, io_signal_callback, c->fd, c->events, c);
    if (!ok) {
        int err = errno;
        conn_abort(c, err);
        return;
    }
    c->started = true;
}

ssize_t conn_write(struct conn *c, const char *buf, size_t len) {
    if (c->closing)
        return -EPIPE;

    if (len == 0)
        return 0;

    if (len > BUF_SIZE)
        return -EMSGSIZE; // can never fit, retrying on on_drain would hang

    if (c->w_queued + len > BUF_SIZE)
        return -ENOBUFS;

    size_t old_queued = c->w_queued;
    uint32_t old_events = c->events;

    memcpy(c->write_buf + old_queued, buf, len);
    c->w_queued += len;
    c->events |= EPOLLOUT;

    if (!sync_events(c)) {
        c->w_queued = old_queued; // nothing queued on failure
        c->events = old_events;
        return -EPIPE;
    }

    return 0;
}

static void reap_cb(struct loop *, void *ud) {
    struct conn *c = (struct conn *)ud;

    if (c->ops.on_close != nullptr) {
        c->ops.on_close(c, c->close_err, c->conn_ctx);
    }

    free(c->write_buf);
    free(c->read_buf);
    free(c);
}

void conn_abort(struct conn *c, int err) {
    if (c->closing)
        return;
    c->closing = true;
    c->close_err = err;

    if (c->deliver_id) {
        loop_timer_cancel(c->loop, c->deliver_id);
        c->deliver_id = 0;
    }

    if (c->started)
        loop_io_remove(c->loop, c->fd);
    close(c->fd);
    c->fd = -1;

    c->reap_id = loop_timer_add(c->loop, 0, reap_cb, c);
}

void conn_read_pause(struct conn *c) {
    if (c->closing)
        return;

    c->events &= ~EPOLLIN;
    sync_events(c);
}

void conn_read_resume(struct conn *c) {
    if (c->closing)
        return;

    c->events |= EPOLLIN;
    sync_events(c);

    // The kernel can't see read_buf, so re-deliver it ourselves.
    schedule_deliver(c);
}