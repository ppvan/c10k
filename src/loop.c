#include "loop.h"
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

static bool less(const struct timer *a, const struct timer *b) {
    if (a->deadline_ns < b->deadline_ns) {
        return true;
    } else {
        return false;
    }
}

void swap(struct timer **a, struct timer **b) {
    void *tmp = *a;
    *a = *b;
    *b = tmp;
}

static void heapify_up(struct timers_heap *h, size_t index) {
    while (index > 0 && h->items[(index - 1) / 2]->deadline_ns >
                            h->items[index]->deadline_ns) {
        struct timer *tmp = h->items[(index - 1) / 2];
        h->items[(index - 1) / 2] = h->items[index];
        h->items[index] = tmp;

        index = (index - 1) / 2;
    }
}

static void heapify_down(struct timers_heap *h, size_t index) {
    size_t smallest = index;
    size_t left = 2 * index + 1;
    size_t right = 2 * index + 2;

    if (left < h->count && less(h->items[left], h->items[smallest])) {
        smallest = left;
    }
    if (right < h->count && less(h->items[right], h->items[smallest])) {
        smallest = right;
    }

    if (smallest != index) {
        swap(&h->items[index], &h->items[smallest]);
        heapify_down(h, smallest);
    }
}

void timers_heap_init(struct timers_heap *h) {
    h->cap = 256;
    h->count = 0;
    h->items = malloc((size_t)h->cap * sizeof(struct timer *));
}

void timers_heap_destroy(struct timers_heap *h) {
    // for (size_t i = 0; i < h->count; i++) {
    //     free(h->items[i]);
    //     h->items[i] = NULL;
    // }

    free(h->items);
    h->items = NULL;
}

void timers_heap_push(struct timers_heap *h, struct timer *t) {
    if (h->count >= h->cap) {
        h->cap *= 2;
        h->items = realloc(h->items, (size_t)h->cap * sizeof(struct timer *));

        assert(h->items && "out of memory, buy more ram");
    }

    h->items[h->count++] = t;
    heapify_up(h, h->count - 1);
}

struct timer *timers_heap_peek(const struct timers_heap *h) {
    if (h->count == 0) {
        return NULL;
    }
    return h->items[0];
}

struct timer *timers_heap_pop(struct timers_heap *h) {
    if (h->count <= 0) {
        return NULL;
    }

    struct timer *root = h->items[0];

    h->items[0] = h->items[h->count - 1];
    h->count--;
    if (h->count > 0) {
        heapify_down(h, 0);
    }

    return root;
}

struct loop {
    struct timers_heap *h;
};

struct loop *loop_init(void) {
    struct loop *l = malloc(sizeof(struct loop));
    l->h = malloc(sizeof(struct timers_heap));
    timers_heap_init(l->h);

    return l;
}
void loop_destroy(struct loop *l) {
    timers_heap_destroy(l->h);
    free(l);
}

uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void loop_timer_add(struct loop *l, size_t expire_ns, timer_cb cb,
                    void *userdata) {
    struct timer *t = malloc(sizeof(struct timer));

    uint64_t now = monotonic_ns();
    t->deadline_ns = expire_ns + now;
    t->handler = cb;
    t->userdata = userdata;

    timers_heap_push(l->h, t);
}

void loop_run(struct loop *l) {
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event events[1];
    assert(epfd >= 0);

    while (l->h->count > 0) {
        struct timer *t = timers_heap_peek(l->h);
        uint64_t now = monotonic_ns();

        if (now >= t->deadline_ns) {
            t = timers_heap_pop(l->h);
            t->handler(l, t->userdata);
            continue;
        }

        uint64_t remaining = t->deadline_ns - now;

        struct timespec timeout = {
            .tv_sec = (time_t)(remaining / 1000000000ULL),
            .tv_nsec = (long)(remaining % 1000000000ULL),
        };

        int rc = epoll_pwait2(epfd, events, 1, &timeout, NULL);

        if (rc < 0 && errno != EINTR) {
            break;
        }
    }

    close(epfd);
}