// test.c
#include "conn.h"
#include "loop.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>

size_t on_read(struct conn *c, const void *buf, size_t len, void *ud) {
    (void)ud;
    (void)buf;

    printf("len = %lu \n", len);
    conn_abort(c);

    return len;
}

void on_close(struct conn *c, int err, void *ud) {
    (void)err;
    (void)ud;
    (void)c;
}

static void on_accept(struct listener *lis, struct conn *c, void *ud) {
    (void)lis;
    (void)c;
    printf("connection!\n");

    struct conn_ops op = {
        .on_read = on_read,
        .on_close = on_close,
    };

    // Register events to the connection
    conn_start(c, op, ud);
}

int main(void) {

    struct loop *l = loop_new();
    struct listener *lis = listener_new(l, "127.0.0.1", 8080, 5, on_accept, nullptr);

    loop_run(l);

    listener_close(lis);
    loop_destroy(l);
}