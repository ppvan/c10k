// test.c
#include "conn.h"
#include "loop.h"
#include <assert.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <unistd.h>

static size_t echo_read(struct conn *c, const void *buf, size_t len, void *ud) {
    (void)ud;
    if (conn_write(c, buf, len) < 0) {
        // conn_read_pause(c); /* slow reader: stop reading until drained */
    }
    return len;         /* consumed everything */
}

static void echo_drain(struct conn *c, void *ud) {
    (void)ud;
    // conn_read_resume(c);
}

static void echo_close(struct conn *c, int err, void *ud) {
    (void)c;
    (void)err;
    (void)ud;
}

static const struct conn_ops echo_ops = {
    .on_read = echo_read,
    // .on_drain = echo_drain,
    .on_close = echo_close,
};

static void on_accept(struct listener *ls, struct conn *c, void *ud) {
    (void)ls;
    (void)ud;
    conn_start(c, echo_ops, NULL);
}

int main(void) {

    struct loop *l = loop_new();
    struct listener *lis = listener_new(l, "127.0.0.1", 8080, 5, on_accept, nullptr);

    loop_run(l);

    listener_close(lis);
    loop_destroy(l);
}