#ifndef LOOOP_H
#define LOOOP_H

#include <stddef.h>
#include <stdint.h>

struct loop;
typedef void (*io_cb)(struct loop *loop, int fd, uint32_t events,
                      void *userdata);
typedef void (*timer_cb)(struct loop *loop, void *userdata);

struct loop *loop_new(void);
void loop_destroy(struct loop *l);
void loop_run(struct loop *l);

struct timer* loop_timer_add(struct loop *l, size_t expire_ns, timer_cb cb, void* userdata);
void loop_timer_cancel(struct loop *l, size_t id);

#endif