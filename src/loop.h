#ifndef LOOP_H
#define LOOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct loop;
typedef void (*io_cb)(struct loop *loop, int fd, uint32_t events,
                      void *userdata);
typedef void (*timer_cb)(struct loop *loop, void *userdata);

struct loop *loop_new(void);
void loop_destroy(struct loop *l);
void loop_run(struct loop *l);

uint64_t loop_timer_add(struct loop *l, uint64_t delay_ms, timer_cb cb,
                        void *userdata);
bool loop_timer_cancel(struct loop *l, uint64_t id);

#endif