#ifndef CONN_H
#define CONN_H

#include "loop.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct conn;
struct listener;

struct conn_ops {
    /* Optional. Outgoing connections only: handshake finished, safe to write.
     * Failure is reported through on_close, never here. */
    void (*on_connect)(struct conn *, void *ud);

    /* Required. `buf` holds ALL unread bytes (valid only during the call).
     * Return how many bytes you consumed; the rest stays buffered and is
     * presented again, with more data appended, on the next call.
     * Returning less than len with no new data is fine: it waits for more.
     * Returning 0 when the buffer is full closes with EMSGSIZE. */
    size_t (*on_read)(struct conn *, const void *buf, size_t len, void *ud);

    /* Optional. The write queue went from non-empty to empty. */
    void (*on_drain)(struct conn *, void *ud);

    /* Required. Called exactly once, always last. After it returns the
     * `struct conn *` is invalid, so free your per-connection state here.
     * err: 0 = clean close (peer EOF or local conn_close),
     *      else an errno value (ECONNRESET, ETIMEDOUT for idle timeout,
     *      ECONNREFUSED, EMSGSIZE, ENOBUFS, ...). */
    void (*on_close)(struct conn *, int err, void *ud);
};

/* Called once per accepted connection. You must call conn_start() on it
 * (or conn_abort() to reject it). */
typedef void (*listener_accept_cb)(struct listener *, struct conn *, void *ud);

struct listener *listener_new(struct loop *l,
                              const char *address,
                              uint16_t port,
                              int backlog,
                              listener_accept_cb on_accept,
                              void *ud);
void listener_close(struct listener *);

/* Starts a non-blocking connect to a numeric IPv4/IPv6 address (no DNS here,
 * since getaddrinfo blocks). Returns NULL only for immediate failure (errno
 * set); later failures arrive via on_close. */
struct conn *conn_connect(struct loop *l, const char *ip, uint16_t port, const struct conn_ops ops, void *ud);

void conn_abort(struct conn *, int err);
/* Accepted connections start paused. Attach handlers with this, then reading
 * begins. Call exactly once, from inside on_accept. */
void conn_start(struct conn *c, const struct conn_ops ops, void *ud);

/* Queue data (copied). Never blocks, never calls your callbacks re-entrantly.
 *   0       queued
 *  -ENOBUFS would exceed max_write_buf; nothing queued, pause your producer
 *           and resume from on_drain
 *  -EPIPE   connection is closing; nothing queued */
int conn_write(struct conn *c, const void *buf, size_t len);

/* Backpressure: app -> conn layer */
void conn_read_pause(struct conn *);
void conn_read_resume(struct conn *);

/* public property */ 
struct loop *conn_loop(const struct conn *c);
bool conn_peer(const struct conn *c, char *ip, size_t ip_len, uint16_t *port);

#endif