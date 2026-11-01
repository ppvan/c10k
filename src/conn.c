#include "conn.h"
#include "loop.h"
#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUF_SIZE 1048576
#define DRAIN_TIMEOUT_MS 10000

struct conn {
    struct loop *loop;
    int fd;
    uint32_t events;
    bool started;    /* registered with the loop */
    bool connecting; /* outgoing connect still in flight */

    char *write_buf;
    size_t w_queued;

    char *read_buf;
    size_t r_queued;

    /* connection state */
    bool paused; /* app called conn_read_pause() */
    bool rd_eof; /* peer sent FIN */
    bool eof_notified;
    bool draining;        /* conn_close(): flush queue, then close */
    bool shut_wr_pending; /* conn_shutdown_write(): FIN once queue is empty */
    bool wr_shut;         /* our FIN is sent */
    uint64_t drain_timer; /* deadline for the flush */

    bool closing;
    int close_err;
    uint64_t reap_id;

    uint64_t deliver_id;
    uint64_t connect_timer; /* connect timeout, 0 = none */
    struct conn_ops ops;
    void *conn_ctx;
};

struct listener {
    int fd;
    int spare_fd; /* parked fd, see shed_one() */
    struct loop *loop;

    listener_accept_cb handler;
    void *listener_ctx;
};

/* ---------- helpers ---------- */

static struct conn *conn_alloc(struct loop *l, int fd) {
    struct conn *c = MALLOC(sizeof(struct conn));
    *c = (struct conn){};
    c->fd = fd;
    c->loop = l;
    c->read_buf = MALLOC(BUF_SIZE * sizeof(char));
    c->write_buf = MALLOC(BUF_SIZE * sizeof(char));
    return c;
}

/* Numeric IPv4 or IPv6 only. */
static bool parse_addr(const char *ip, uint16_t port, struct sockaddr_storage *ss, socklen_t *len) {
    struct sockaddr_in *sin = (struct sockaddr_in *)ss;
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)ss;

    memset(ss, 0, sizeof *ss);
    if (inet_pton(AF_INET, ip, &sin->sin_addr) == 1) {
        sin->sin_family = AF_INET;
        sin->sin_port = htons(port);
        *len = sizeof *sin;
        return true;
    }
    if (inet_pton(AF_INET6, ip, &sin6->sin6_addr) == 1) {
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(port);
        *len = sizeof *sin6;
        return true;
    }
    return false;
}

static bool sync_events(struct conn *c) {
    if (!c->started)
        return true;
    return loop_io_mod(c->loop, c->fd, c->events);
}

static void notify_eof(struct conn *c) {
    c->eof_notified = true;
    if (c->wr_shut) { /* we sent FIN, now they did: done */
        conn_abort(c, 0);
        return;
    }
    if (c->ops.on_eof != nullptr)
        c->ops.on_eof(c, c->conn_ctx);
    else
        conn_close(c); /* default: flush replies, then close */
}

static void deliver(struct conn *c) {
    if (c->closing || c->r_queued == 0 || c->paused)
        return;

    size_t n = c->r_queued;
    size_t consumed = c->ops.on_read(c, c->read_buf, n, c->conn_ctx);
    /* app.on_read() close connection or abort */
    if (c->closing || c->draining)
        return;

    if (consumed > n) {
        conn_abort(c, EPROTO);
        return;
    }

    memmove(c->read_buf, c->read_buf + consumed, n - consumed);
    c->r_queued = n - consumed;

    if (c->rd_eof && !c->eof_notified && !c->paused)
        notify_eof(c);
}

static void deliver_cb(struct loop *, void *ud) {
    struct conn *c = ud;
    c->deliver_id = 0;
    deliver(c);
}

static void schedule_deliver(struct conn *c) {
    if (c->closing || c->draining || c->deliver_id)
        return;
    if (c->r_queued == 0 && !(c->rd_eof && !c->eof_notified))
        return;
    c->deliver_id = loop_timer_add(c->loop, 0, deliver_cb, c);
}

static void on_peer_eof(struct conn *c) {
    c->rd_eof = true;
    c->events &= ~EPOLLIN; /* else level-triggered epoll reports EOF forever */
    if (!sync_events(c)) {
        int e = errno;
        conn_abort(c, e);
        return;
    }
    schedule_deliver(c); /* hand over leftover bytes, then notify_eof */
}

#define DRAIN_TIMEOUT_MS 10000

static int socket_error(struct conn *c) {
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err == 0)
        return ECONNRESET;
    return err;
}

static void flush_complete(struct conn *c) {
    if (c->draining) { /* conn_close(): all out, finish */
        conn_abort(c, c->close_err);
        return;
    }
    if (c->shut_wr_pending) { /* conn_shutdown_write(): send FIN */
        c->shut_wr_pending = false;
        c->wr_shut = true;
        if (shutdown(c->fd, SHUT_WR) < 0 && errno != ENOTCONN) {
            int e = errno;
            conn_abort(c, e);
            return;
        }
        if (c->rd_eof) { /* both directions finished */
            conn_abort(c, 0);
            return;
        }
    } else if (c->ops.on_drain != nullptr) {
        c->ops.on_drain(c, c->conn_ctx);
        if (c->closing)
            return;
    }
    sync_events(c);
}

static void handle_readable(struct conn *c) {

    if (c->rd_eof || c->draining)
        return;

    if (c->r_queued == BUF_SIZE) {
        deliver(c);
        if (c->closing)
            return;
        if (c->r_queued == BUF_SIZE) {
            // Buffer still full. If the app paused inside on_read this is
            // legitimate backpressure; otherwise it broke the contract.
            // Either way never recv() with length 0 (it looks like EOF).
            if (c->paused)
                conn_abort(c, EMSGSIZE);
            return;
        }
    }

    ssize_t n = recv(c->fd, c->read_buf + c->r_queued, BUF_SIZE - c->r_queued, 0);
    if (n == 0) {
        on_peer_eof(c);
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
        flush_complete(c);
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

/* The socket became writable (or failed) while connecting. The real outcome is
 * in SO_ERROR: EPOLLERR/EPOLLHUP alone only say "it failed", not why. */
static void handle_connect(struct conn *c, uint32_t events) {
    if (!(events & (EPOLLOUT | EPOLLERR | EPOLLHUP)))
        return;

    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
        err = errno;
    if (err == 0 && (events & (EPOLLERR | EPOLLHUP))) {
        err = ECONNRESET; // failed, but the kernel has no specific error left
    }

    if (err != 0) {
        conn_abort(c, err);
        return;
    }

    c->connecting = false;
    if (c->connect_timer != 0) {
        loop_timer_cancel(c->loop, c->connect_timer);
        c->connect_timer = 0;
    }

    c->events = (c->paused ? 0 : EPOLLIN) | (c->w_queued > 0 ? EPOLLOUT : 0);
    if (!sync_events(c)) {
        int e = errno;
        conn_abort(c, e);
        return;
    }

    if (c->ops.on_connect != nullptr)
        c->ops.on_connect(c, c->conn_ctx);

    if (!c->closing && c->shut_wr_pending && c->w_queued == 0)
        flush_complete(c);
}

static void drain_timeout_cb(struct loop *, void *ud) {
    struct conn *c = ud;
    c->drain_timer = 0; /* already fired, id is dead */
    conn_abort(c, ETIMEDOUT);
}

void conn_close(struct conn *c) {
    if (c->closing || c->draining)
        return;

    /* Nothing to flush, or not even registered or connected yet. */
    if (c->w_queued == 0 || c->connecting || !c->started) {
        conn_abort(c, 0);
        return;
    }

    c->draining = true;
    c->close_err = 0;

    /* Reads stop now and unread input is discarded: the app said it is done. */
    c->r_queued = 0;
    if (c->deliver_id) {
        loop_timer_cancel(c->loop, c->deliver_id);
        c->deliver_id = 0;
    }
    c->events = EPOLLOUT;
    if (!sync_events(c)) {
        int e = errno;
        conn_abort(c, e);
        return;
    }

    /* Abort if final write never read by peer */
    c->drain_timer = loop_timer_add(c->loop, DRAIN_TIMEOUT_MS, drain_timeout_cb, c);
}

void conn_shutdown_write(struct conn *c) {
    if (c->closing || c->draining || c->shut_wr_pending || c->wr_shut)
        return;

    c->shut_wr_pending = true;
    if (c->w_queued == 0 && !c->connecting) {
        c->events &= ~EPOLLOUT;
        flush_complete(c); /* nothing queued: FIN right now */
    }
}

static void io_signal_callback(struct loop *, int, uint32_t events, void *ud) {
    struct conn *c = (struct conn *)ud;
    if (c->closing)
        return; // stale event from the same epoll batch

    if (c->connecting) {
        handle_connect(c, events);
        return;
    }

    if (events & EPOLLERR) {
        conn_abort(c, socket_error(c));
        return;
    }

    if (events & (EPOLLIN | EPOLLHUP)) {
        handle_readable(c);
        if (c->closing)
            return;
    }

    if (events & EPOLLOUT) {
        handle_writable(c);
    }
}

/* Out of file descriptors. With level-triggered epoll the listener would fire
 * again immediately (100% CPU) and the waiting client would just hang in the
 * backlog. Instead: give back the spare fd, accept the connection so it leaves
 * the queue, close it (the client sees the close right away), take the spare
 * fd back. Connections we already have are not affected. */
static void shed_one(struct listener *lis) {
    if (lis->spare_fd < 0)
        return;
    close(lis->spare_fd);
    int fd = accept4(lis->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd >= 0)
        close(fd);
    lis->spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
}

#define ACCEPT_BATCH 4096 /* per wakeup, so one busy listener can't starve the loop */

static void new_conn_incoming(struct loop *l, int, uint32_t, void *ud) {
    struct listener *lis = (struct listener *)(ud);

    for (int i = 0; i < ACCEPT_BATCH; i++) {
        int peer_fd = accept4(lis->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (peer_fd < 0) {
            switch (errno) {
            case EINTR:
            case ECONNABORTED: // peer gave up before we accepted
            case EPROTO:       // man accept(2): Linux reports pending network
            case ENETDOWN:     // errors here, treat them like EAGAIN
            case ENOPROTOOPT:
            case EHOSTDOWN:
            case ENONET:
            case EHOSTUNREACH:
            case EOPNOTSUPP:
            case ENETUNREACH:
                continue;
            case EMFILE:
            case ENFILE:
                shed_one(lis);
                return;
            default: // EAGAIN: queue drained; ENOMEM/ENOBUFS: try again later
                return;
            }
        }

        struct conn *c = conn_alloc(l, peer_fd);
        lis->handler(lis, c, lis->listener_ctx);
    }
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

    if (!parse_addr(address, port, &addr, &addrlen))
        return NULL;

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
        .spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC),
        .handler = on_accept,
        .loop = l,
        .listener_ctx = ud,
    };

    bool ok = loop_io_add(lis->loop, new_conn_incoming, lis->fd, EPOLLIN, lis);
    if (!ok) {
        int e = errno;
        close(lis->fd);
        if (lis->spare_fd >= 0)
            close(lis->spare_fd);
        free(lis);
        errno = e;
        return NULL;
    }

    return lis;
}

void listener_close(struct listener *lis) {
    loop_io_remove(lis->loop, lis->fd);
    close(lis->fd);
    if (lis->spare_fd >= 0)
        close(lis->spare_fd);
    free(lis);
}

/* ---------- connection ---------- */

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

struct conn *conn_connect(struct loop *l, const char *ip, uint16_t port, const struct conn_ops ops, void *ud) {
    struct sockaddr_storage addr;
    socklen_t addrlen;

    if (!parse_addr(ip, port, &addr, &addrlen)) {
        errno = EINVAL;
        return nullptr;
    }

    int fd = socket(addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return nullptr;

    // EINPROGRESS is the normal answer. A result of 0 (instant success, can
    // happen on loopback) takes the same path: epoll reports writable at once.
    if (connect(fd, (struct sockaddr *)&addr, addrlen) < 0 && errno != EINPROGRESS) {
        int err = errno;
        close(fd);
        errno = err;
        return nullptr;
    }

    struct conn *c = conn_alloc(l, fd);
    c->ops = ops;
    c->conn_ctx = ud;
    c->connecting = true;
    c->events = EPOLLOUT; // writable means "connect finished, check SO_ERROR"

    if (!loop_io_add(l, io_signal_callback, fd, c->events, c)) {
        int err = errno;
        close(fd);
        free(c->write_buf);
        free(c->read_buf);
        free(c);
        errno = err;
        return nullptr;
    }
    c->started = true;

    return c;
}

int conn_write(struct conn *c, const void *buf, size_t len) {
    if (c->closing || c->draining || c->shut_wr_pending || c->wr_shut)
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

    if (c->connect_timer) {
        loop_timer_cancel(c->loop, c->connect_timer);
        c->connect_timer = 0;
    }

    if (c->drain_timer) {
        loop_timer_cancel(c->loop, c->drain_timer);
        c->drain_timer = 0;
    }

    if (c->started)
        loop_io_remove(c->loop, c->fd);
    close(c->fd);
    c->fd = -1;

    c->reap_id = loop_timer_add(c->loop, 0, reap_cb, c);
}

void conn_read_pause(struct conn *c) {
    if (c->closing || c->draining)
        return;
    c->paused = true;
    c->events &= ~EPOLLIN;
    sync_events(c);
}

void conn_read_resume(struct conn *c) {
    if (c->closing || c->draining)
        return;
    c->paused = false;
    if (!c->rd_eof) { // never re-arm EPOLLIN after EOF
        c->events |= EPOLLIN;
        sync_events(c);
    }

    schedule_deliver(c);
}

struct loop *conn_loop(const struct conn *c) {
    if (c->closing)
        return nullptr;

    return c->loop;
}

size_t conn_pending(const struct conn *c) {
    if (c->closing)
        return 0;

    return c->w_queued;
}