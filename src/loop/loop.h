#include <stdint.h>

struct loop;
struct timer;
typedef void (*io_cb)(struct loop *, int fd, uint32_t events, void *ud);
typedef void (*timer_cb)(struct loop *, struct timer *, void *ud);

struct timer {
    // deadline in  unix?
    uint64_t deadline;

    // call me tonight?
    timer_cb cb;

    // remember to brings things to the party
    void *ud;
};

struct loop *loop_new(void);
int loop_run(struct loop *); /* until loop_stop() */

int loop_io_add(struct loop *, int fd, uint32_t events, io_cb, void *ud);
int loop_io_mod(struct loop *, int fd, uint32_t events);
int loop_io_del(struct loop *, int fd);

void loop_timer_start(struct loop *, struct timer *, uint64_t ms, timer_cb,
                      void *ud);
void loop_timer_stop(struct loop *, struct timer *);
