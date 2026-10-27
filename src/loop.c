#include "loop.h"
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

struct timer {
    uint64_t expiration_ms;
    timer_cb handler;
    void *userdata;
};

struct loop {

    struct timer **timers;
    size_t timers_count;
    size_t timers_cap;
};

static bool less(const struct timer *a, const struct timer *b) {
    if (a->expiration_ms < b->expiration_ms) {
        return true;
    } else {
        return false;
    }
}

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void swap(struct timer **a, struct timer **b) {
    void *tmp = *a;
    *a = *b;
    *b = tmp;
}

static void heapify_up(struct loop *l, size_t index) {
    while (index > 0 && less(l->timers[index], l->timers[(index - 1) / 2])) {
        struct timer *tmp = l->timers[(index - 1) / 2];
        l->timers[(index - 1) / 2] = l->timers[index];
        l->timers[index] = tmp;

        index = (index - 1) / 2;
    }
}

static void heapify_down(struct loop *l, size_t index) {
    size_t smallest = index;
    size_t left = 2 * index + 1;
    size_t right = 2 * index + 2;

    if (left < l->timers_count && less(l->timers[left], l->timers[smallest])) {
        smallest = left;
    }

    if (right < l->timers_count && less(l->timers[right], l->timers[smallest])) {
        smallest = right;
    }

    if (smallest != index) {
        swap(&l->timers[index], &l->timers[smallest]);
        heapify_down(l, smallest);
    }
}

static void timers_heap_push(struct loop *l, struct timer *t) {
    if (l->timers_count >= l->timers_cap) {
        l->timers_cap *= 2;
        l->timers = realloc(l->timers, (size_t)l->timers_cap * sizeof(struct timer *));

        assert(l->timers && "out of memory, buy more ram");
    }

    l->timers[l->timers_count++] = t;
    heapify_up(l, l->timers_count - 1);
}

static struct timer *timers_heap_peek(const struct loop *l) {
    if (l->timers_count == 0) {
        return NULL;
    }
    return l->timers[0];
}

static struct timer *timers_heap_pop(struct loop *l) {
    if (l->timers_count <= 0) {
        return NULL;
    }

    struct timer *root = l->timers[0];

    l->timers[0] = l->timers[l->timers_count - 1];
    l->timers_count--;
    if (l->timers_count > 0) {
        heapify_down(l, 0);
    }

    return root;
}

struct loop *loop_new(void) {
    struct loop *l = malloc(sizeof(struct loop));
    l->timers_cap = 256;
    l->timers_count = 0;
    l->timers = malloc((size_t)l->timers_cap * sizeof(struct timer *));

    return l;
}

void loop_destroy(struct loop *l) {
    free(l->timers);
    free(l);
}

struct timer* loop_timer_add(struct loop *l, size_t expire_ms, timer_cb cb,
                      void *userdata) {
    struct timer *t = malloc(sizeof(struct timer));

    uint64_t now = monotonic_ms();
    t->expiration_ms = expire_ms + now;
    t->handler = cb;
    t->userdata = userdata;

    timers_heap_push(l, t);

    return t;
}

void loop_run(struct loop *l) {
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    assert(epfd >= 0);
    struct epoll_event events[1];

    while (l->timers_count > 0) {
        struct timer *t = timers_heap_peek(l);
        uint64_t now = monotonic_ms();

        if (now >= t->expiration_ms) {
            t = timers_heap_pop(l);
            t->handler(l, t->userdata);

            free(t);
            continue;
        }

        int timeout = (int)(t->expiration_ms - now);

        int rc = epoll_wait(epfd, events, 1, timeout);
        if (rc < 0 && errno != EINTR) {
            close(epfd);
            err(EXIT_FAILURE, "epoll");
        }
    }

    close(epfd);
}