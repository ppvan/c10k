#ifndef TIMERS_HEAP_H
#define TIMERS_HEAP_H

#include <stddef.h>
#include <stdint.h>

struct event_loop;

typedef void (*io_cb)(struct event_loop *loop, int fd, uint32_t events, void *userdata);

// Timers.
struct timer {
    uint64_t deadline_ns;
};

struct timers_heap {
    struct timer **items;
    size_t count;
    size_t cap;
};

void timers_heap_init(struct timers_heap *h);
void timers_heap_destroy(struct timers_heap *h);

struct timer *timers_heap_peek(const struct timers_heap *h);
void timers_heap_push(struct timers_heap *h, struct timer *t);
struct timer *timers_heap_pop(struct timers_heap *h);

void timers_heap_remove(struct timers_heap *h, struct timer *t);
void timers_heap_update(struct timers_heap *h, struct timer *t);

#endif