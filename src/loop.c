#include "loop.h"
#include <err.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

static void *must_alloc(void *p, const char *file, int line) {
    if (p == NULL) {
        errx(EXIT_FAILURE, "%s:%d: out of memory", file, line);
    }
    return p;
}

#define MALLOC(size) must_alloc(malloc(size), __FILE__, __LINE__)
#define REALLOC(ptr, size)                                                     \
    must_alloc(realloc((ptr), (size)), __FILE__, __LINE__)

struct timer {
    uint64_t expiration_ms;
    timer_cb handler;
    void *userdata;

    size_t heap_idx;
    uint32_t slot_idx;
};

#define NO_SLOT UINT32_MAX
struct slot {
    struct timer *t; // NULL when free
    uint32_t gen;    // starts at 1, bumped on release
    uint32_t next_free;
};

struct loop {

    struct timer **timers;
    size_t timers_count;
    size_t timers_cap;

    struct slot *slots;
    uint32_t slots_cap, free_head;
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

    // swap idx again so the end result idx does not change
    size_t tmp_idx = (*a)->heap_idx;
    (*a)->heap_idx = (*b)->heap_idx;
    (*b)->heap_idx = tmp_idx;
}

static void heapify_up(struct loop *l, size_t index) {
    while (index > 0 && less(l->timers[index], l->timers[(index - 1) / 2])) {
        swap(&l->timers[(index - 1) / 2], &l->timers[index]);
        index = (index - 1) / 2;
    }
}

static void heapify_down(struct loop *l, size_t index) {

    while (true) {
        size_t smallest = index;
        size_t left = 2 * index + 1;
        size_t right = 2 * index + 2;

        if (left < l->timers_count &&
            less(l->timers[left], l->timers[smallest])) {
            smallest = left;
        }

        if (right < l->timers_count &&
            less(l->timers[right], l->timers[smallest])) {
            smallest = right;
        }

        if (smallest != index) {
            swap(&l->timers[index], &l->timers[smallest]);
            index = smallest;
        } else {
            break;
        }
    }
}

static void timers_heap_push(struct loop *l, struct timer *t) {
    if (l->timers_count >= l->timers_cap) {
        l->timers_cap *= 2;
        l->timers = REALLOC(l->timers, l->timers_cap * sizeof(struct timer *));
    }

    t->heap_idx = l->timers_count;
    l->timers[l->timers_count++] = t;
    heapify_up(l, l->timers_count - 1);
}

static void slots_grow(struct loop *l) {
    uint32_t old = l->slots_cap;
    uint32_t cap = old * 2;
    struct slot *s = REALLOC(l->slots, cap * sizeof(struct slot));
    for (uint32_t i = old; i < cap; i++) {
        s[i] = (struct slot){.t = NULL,
                             .gen = 1,
                             .next_free = (i + 1 < cap) ? i + 1 : l->free_head};
    }

    l->slots = s;
    l->free_head = old;
    l->slots_cap = cap;
}

static uint64_t slot_acquire(struct loop *l, struct timer *t) {
    if (l->free_head == NO_SLOT) {
        slots_grow(l);
    }

    struct slot *s = l->slots;
    uint32_t i = l->free_head;
    l->free_head = s[i].next_free;
    s[i].t = t;
    t->slot_idx = i;

    // Return the id, which is pack index and gen to a number.
    return ((uint64_t)l->slots[i].gen << 32) | i;
}

static void slot_release(struct loop *l, struct timer *t) {
    uint32_t i = t->slot_idx;
    struct slot *s = l->slots;

    s[i].t = NULL;
    // wrap around when overflow
    if (++s[i].gen == 0) {
        s[i].gen = 1;
    }
    s[i].next_free = l->free_head;
    l->free_head = i;
}

static struct timer *timer_lookup(struct loop *l, uint64_t id) {
    uint32_t slot = (uint32_t)id, gen = (uint32_t)(id >> 32);

    if (slot >= l->slots_cap || l->slots[slot].gen != gen) {
        return NULL;
    }

    return l->slots[slot].t;
}

static struct timer *timers_heap_peek(const struct loop *l) {
    if (l->timers_count == 0) {
        return NULL;
    }
    return l->timers[0];
}

static struct timer *timers_heap_pop(struct loop *l) {
    if (l->timers_count == 0) {
        return NULL;
    }

    struct timer *root = l->timers[0];

    swap(&l->timers[0], &l->timers[l->timers_count - 1]);
    l->timers_count--;

    if (l->timers_count > 0) {
        heapify_down(l, 0);
    }

    return root;
}

struct loop *loop_new(void) {
    struct loop *l = MALLOC(sizeof(struct loop));

    l->timers_cap = 256;
    l->timers_count = 0;
    l->timers = MALLOC(l->timers_cap * sizeof(struct timer *));

    l->slots_cap = 256;
    l->slots = MALLOC(l->slots_cap * sizeof(struct slot));

    for (uint32_t i = 0; i < l->slots_cap; i++) {
        l->slots[i] = (struct slot){
            .t = NULL,
            .gen = 1,
            .next_free = (i + 1 < l->slots_cap) ? i + 1 : NO_SLOT};
    }
    l->free_head = 0;

    return l;
}

void loop_destroy(struct loop *l) {
    for (size_t i = 0; i < l->timers_count; i++) {
        free(l->timers[i]);
    }

    free(l->timers);
    free(l->slots);
    free(l);
}

uint64_t loop_timer_add(struct loop *l, uint64_t delay_ms, timer_cb cb,
                        void *userdata) {
    struct timer *t = MALLOC(sizeof(struct timer));

    uint64_t now = monotonic_ms();
    t->expiration_ms = delay_ms + now;
    t->handler = cb;
    t->userdata = userdata;

    timers_heap_push(l, t);

    return slot_acquire(l, t);
}

bool loop_timer_cancel(struct loop *l, uint64_t id) {
    struct timer *t = timer_lookup(l, id);
    if (t == NULL) {
        return false;
    }

    size_t idx = t->heap_idx;
    size_t last = l->timers_count - 1;

    if (idx != last) {
        swap(&l->timers[idx], &l->timers[last]);
        l->timers_count--;
        heapify_down(l, idx);
        heapify_up(l, idx);
    } else {
        l->timers_count--;
    }

    slot_release(l, t);
    free(t);

    return true;
}

void loop_run(struct loop *l) {
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        err(EXIT_FAILURE, "epoll_create1");
    }
    struct epoll_event events[1];

    while (l->timers_count > 0) {
        uint64_t now = monotonic_ms();

        struct timer *t = timers_heap_peek(l);
        if (now >= t->expiration_ms) {
            // Order matters: the callback may add timers or change the loop, so
            // the expired timer must be fully removed before it runs.
            //   1. Pop it from the heap.
            //   2. Release its slot (invalidates the timer id).
            //   3. Copy handler/userdata, then free the timer.
            //   4. Call the handler last.
            t = timers_heap_pop(l);
            slot_release(l, t);

            timer_cb cb = t->handler;
            void *ud = t->userdata;
            free(t);

            cb(l, ud);
            continue;
        }

        uint64_t diff = t->expiration_ms - now;
        int timeout = diff > INT32_MAX ? INT32_MAX : (int)diff;

        int rc = epoll_wait(epfd, events, 1, timeout);
        if (rc < 0 && errno != EINTR) {
            close(epfd);
            err(EXIT_FAILURE, "epoll");
        }
    }

    close(epfd);
}