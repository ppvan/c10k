#ifndef LOOOP_H
#define LOOOP_H

#include <stddef.h>
#include <stdint.h>

struct loop;

typedef void (*io_cb)(struct loop *loop, int fd, uint32_t events,
                      void *userdata);
typedef void (*timer_cb)(struct loop *loop, void *userdata);

// Timers.
struct timer {
    uint64_t deadline_ns;
    timer_cb handler;
    void* userdata;
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

struct loop *loop_init(void);
void loop_destroy(struct loop *l);
void loop_run(struct loop *l);
void loop_timer_add(struct loop *l, size_t expire_ns, timer_cb cb, void* userdata);

#endif