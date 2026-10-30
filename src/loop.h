#ifndef LOOP_H
#define LOOP_H

#include <err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* Core loop API */
struct loop;
struct loop *loop_new(void);
void loop_destroy(struct loop *);
void loop_run(struct loop *);
void loop_stop(struct loop *);

/* Timers */
typedef void (*timer_cb)(struct loop *, void *);
uint64_t loop_timer_add(struct loop *, uint64_t, timer_cb, void *);
bool loop_timer_cancel(struct loop *, uint64_t);

/* I/O readiness */
typedef void (*io_cb)(struct loop *, int, uint32_t, void *);
bool loop_io_add(struct loop *, io_cb, int, uint32_t, void *);
bool loop_io_remove(struct loop *, int);
bool loop_io_mod(struct loop *, int, uint32_t);

void *must_alloc(void *p, const char *file, int line);

#define MALLOC(size) must_alloc(malloc(size), __FILE__, __LINE__)
#define REALLOC(ptr, size)                                                     \
    must_alloc(realloc((ptr), (size)), __FILE__, __LINE__)

#endif