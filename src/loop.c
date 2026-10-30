#include "loop.h"
#include <err.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

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

struct io_watch {
    int fd;
    uint32_t gen;
    uint32_t events;
    io_cb handler;
    void *userdata;
};

struct loop {

    struct timer **timers;
    size_t timers_count;
    size_t timers_cap;

    struct slot *slots;
    uint32_t slots_cap, free_head;

    // io_watch
    struct io_watch **io_by_fd;
    size_t io_cap;
    size_t io_count;

    int epoll_fd;
    bool stopped;
    uint32_t io_gen;
};

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static bool timer_less(const struct timer *a, const struct timer *b) {
    if (a->expiration_ms < b->expiration_ms) {
        return true;
    } else {
        return false;
    }
}

static void timer_swap(struct timer **a, struct timer **b) {
    void *tmp = *a;
    *a = *b;
    *b = tmp;

    // swap idx again so the end result idx does not change
    size_t tmp_idx = (*a)->heap_idx;
    (*a)->heap_idx = (*b)->heap_idx;
    (*b)->heap_idx = tmp_idx;
}

static void heapify_up(struct loop *l, size_t index) {
    while (index > 0 &&
           timer_less(l->timers[index], l->timers[(index - 1) / 2])) {
        timer_swap(&l->timers[(index - 1) / 2], &l->timers[index]);
        index = (index - 1) / 2;
    }
}

static void heapify_down(struct loop *l, size_t index) {

    while (true) {
        size_t smallest = index;
        size_t left = 2 * index + 1;
        size_t right = 2 * index + 2;

        if (left < l->timers_count &&
            timer_less(l->timers[left], l->timers[smallest])) {
            smallest = left;
        }

        if (right < l->timers_count &&
            timer_less(l->timers[right], l->timers[smallest])) {
            smallest = right;
        }

        if (smallest != index) {
            timer_swap(&l->timers[index], &l->timers[smallest]);
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

    timer_swap(&l->timers[0], &l->timers[l->timers_count - 1]);
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

    l->io_cap = 256;
    l->io_count = 0;
    l->io_by_fd = MALLOC(l->io_cap * sizeof(struct io_watch));
    for (uint32_t i = 0; i < l->io_cap; i++) {
        l->io_by_fd[i] = NULL;
    }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        err(EXIT_FAILURE, "epoll_create1");
    }

    l->epoll_fd = epfd;

    return l;
}
void loop_destroy(struct loop *l) {
    if (l == NULL)
        return;

    for (size_t i = 0; i < l->timers_count; i++)
        free(l->timers[i]);

    for (size_t i = 0; i < l->io_cap; i++)
        free(l->io_by_fd[i]);

    close(l->epoll_fd);
    free(l->io_by_fd);
    free(l->slots);
    free(l->timers);
    free(l);
}
uint64_t loop_timer_add(struct loop *l, uint64_t delay_ms, timer_cb cb,
                        void *userdata) {
    struct timer *t = MALLOC(sizeof(struct timer));

    if(delay_ms == 0) delay_ms = 1;

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
        timer_swap(&l->timers[idx], &l->timers[last]);
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
    struct epoll_event events[1024];
    const int max_events = (int)(sizeof events / sizeof events[0]);

    l->stopped = false;

    while (!l->stopped && (l->timers_count > 0 || l->io_count > 0)) {
        int timeout = -1;
        struct timer *t = timers_heap_peek(l);
        if (t != NULL) {
            uint64_t now = monotonic_ms();
            if (t->expiration_ms <= now) {
                timeout = 0;
            } else {
                uint64_t diff = t->expiration_ms - now;
                timeout = diff > INT32_MAX ? INT32_MAX : (int)diff;
            }
        }

        /* 2. Wait for IO or the next timer */
        int rc = epoll_wait(l->epoll_fd, events, max_events, timeout);
        if (rc < 0) {
            if (errno != EINTR)
                return; /* or record errno on the loop */
            rc = 0;     /* still go check timers */
        }

        /* 3. Dispatch IO */
        for (int i = 0; i < rc && !l->stopped; i++) {
            int fd = (int)(uint32_t)events[i].data.u64;
            uint32_t gen = (uint32_t)(events[i].data.u64 >> 32);

            if ((size_t)fd >= l->io_cap)
                continue;
            struct io_watch *w = l->io_by_fd[fd];
            if (w == NULL || w->gen != gen)
                continue; /* removed or fd reused */

            w->handler(l, fd, events[i].events, w->userdata);
        }

        /* 4. Run expired timers (fresh clock, one snapshot per pass) */
        uint64_t now = monotonic_ms();
        while (!l->stopped && (t = timers_heap_peek(l)) != NULL &&
               t->expiration_ms <= now) {
            t = timers_heap_pop(l);
            slot_release(l, t);

            timer_cb cb = t->handler;
            void *ud = t->userdata;
            free(t);

            cb(l, ud);
        }
    }
}


static uint64_t io_token(uint32_t gen, int fd) {
    return ((uint64_t)gen << 32) | (uint32_t)fd;
}

static void io_ensure_cap(struct loop *l, int fd) {
    size_t need = (size_t)fd + 1;
    if (need <= l->io_cap)
        return;

    size_t cap = l->io_cap;
    while (cap < need)
        cap *= 2;

    l->io_by_fd = REALLOC(l->io_by_fd, cap * sizeof *l->io_by_fd);
    for (size_t i = l->io_cap; i < cap; i++)
        l->io_by_fd[i] = NULL;
    l->io_cap = cap;
}

bool loop_io_add(struct loop *l, io_cb callback, int fd, uint32_t events,
                 void *userdata) {
    if (fd < 0) {
        errno = EBADF;
        return false;
    }
    io_ensure_cap(l, fd);

    uint32_t gen = ++l->io_gen;
    if (gen == 0)                       // 0 is never a valid generation
        gen = ++l->io_gen;

    struct epoll_event ev = {.events = events, .data.u64 = io_token(gen, fd)};
    if (epoll_ctl(l->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
        return false;                   // errno left for the caller (EEXIST, EBADF, ...)

    struct io_watch *w = MALLOC(sizeof *w);
    *w = (struct io_watch){
        .fd = fd,
        .gen = gen,
        .events = events,
        .handler = callback,
        .userdata = userdata,
    };

    // If the user closed an fd without calling loop_io_remove and the number was
    // reused, the kernel already dropped the old registration. Replace, don't leak.
    struct io_watch *old = l->io_by_fd[fd];
    l->io_by_fd[fd] = w;
    if (old != NULL)
        free(old);
    else
        l->io_count++;

    return true;
}

bool loop_io_remove(struct loop *l, int fd) {
    if (fd < 0 || (size_t)fd >= l->io_cap || l->io_by_fd[fd] == NULL)
        return false;

    // EBADF/ENOENT means the fd was already closed and the kernel dropped it.
    // That's fine: we still need to free our side.
    if (epoll_ctl(l->epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0 &&
        errno != EBADF && errno != ENOENT)
        return false;

    free(l->io_by_fd[fd]);
    l->io_by_fd[fd] = NULL;
    l->io_count--;
    return true;
}

bool loop_io_mod(struct loop *l, int fd, uint32_t events) {
    if (fd < 0 || (size_t)fd >= l->io_cap)
        return false;

    struct io_watch *w = l->io_by_fd[fd];
    if (w == NULL)
        return false;

    // MOD replaces the stored data too, so it must carry the same gen.
    struct epoll_event ev = {.events = events, .data.u64 = io_token(w->gen, fd)};
    if (epoll_ctl(l->epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0)
        return false;

    w->events = events;
    return true;
}
void loop_stop(struct loop *l) { l->stopped = true; }

void *must_alloc(void *p, const char *file, int line) {
    if (p == NULL) {
        errx(EXIT_FAILURE, "%s:%d: out of memory", file, line);
    }
    return p;
}