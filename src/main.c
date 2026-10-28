// test.c
#include "loop.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>

static int hits;
static void on_read(struct loop *l, int fd, uint32_t ev, void *ud) {
    (void)ev;
    (void)ud;

    char b[8];
    (void)read(fd, b, sizeof b);
    hits++;
    loop_io_remove(l, fd);
    close(fd);
    loop_stop(l);
}

int main(void) {
    struct loop *l = loop_new();
    int p[2];
    assert(pipe(p) == 0);
    int big = dup2(p[0], 1000);            // forces io_by_fd growth past 256
    assert(big == 1000);
    assert(loop_io_add(l, on_read, big, EPOLLIN, NULL));
    assert(write(p[1], "x", 1) == 1);
    loop_run(l);
    assert(hits == 1);
    loop_destroy(l);
    puts("ok");
}