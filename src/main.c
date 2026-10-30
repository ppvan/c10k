// test.c
#include "conn.h"
#include "loop.h"
#include <assert.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

static void on_accept(struct listener *lis, struct conn* c, void *ud) {
    (void)lis;
    (void)c;
    (void)ud;
}

int main(void) {

    struct loop* l = loop_new();
    struct listener *lis = listener_new(l, "127.0.0.1", 8080, 5, on_accept, nullptr);

    listener_close(lis);
    loop_destroy(l);
}