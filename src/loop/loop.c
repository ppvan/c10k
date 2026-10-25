#include "loop.h"
#include <stddef.h>
#include <stdlib.h>

struct loop {
    struct timers_heap* timers;
};

struct timers_heap {
    struct timer *item;
    size_t count;
    size_t cap;
};

struct loop *loop_new(void) {
    struct loop* l = malloc(sizeof(struct loop));
    l->timers = malloc(sizeof(struct timers_heap));

    l->timers->cap = 256;
    l->timers->count = 0;
    l->timers->item = malloc(l->timers->cap * sizeof(struct timer));

    return l;
}

int loop_run(struct loop* l) {


    return 0;
}


