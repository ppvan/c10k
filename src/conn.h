#ifndef CONN_H
#define CONN_H

#include "loop.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct conn;
struct listener;

struct conn_ops {
    size_t (*on_read)(struct conn *, const void *buf, size_t len, void *ud);
    void (*on_close)(struct conn *, int err, void *ud);
    void (*on_drain)(struct conn *, void *ud);

    /* Optional. Outgoing connections only: the handshake finished and the
     * connection is ready. A failed connect never gets here, it is reported
     * through on_close(err) instead. */
    void (*on_connect)(struct conn *, void *ud);
};

typedef void (*listener_accept_cb)(struct listener *, struct conn *, void *ud);

struct listener *listener_new(struct loop *l,
                              const char *address,
                              uint16_t port,
                              int backlog,
                              listener_accept_cb on_accept,
                              void *ud);
void listener_close(struct listener *);


struct conn *conn_connect(struct loop *l,
                          const char *ip,
                          uint16_t port,
                          uint64_t timeout_ms,
                          struct conn_ops ops,
                          void *ud);

void conn_abort(struct conn *, int err);
void conn_start(struct conn *, const struct conn_ops, void *ud);
ssize_t conn_write(struct conn *, const char *buf, size_t len);
void conn_read_pause(struct conn *);
void conn_read_resume(struct conn *);

#endif