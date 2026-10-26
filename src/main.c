#include "loop.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>


void on_time(struct loop *l, void *userdata) {
    (void)l;
    int num = (int)(intptr_t)userdata;
    printf("Hello to number: %d\n", num);
}

int main(void) {
    struct loop *l = loop_init();
    loop_timer_add(l, 5000, on_time, (void *)(intptr_t)5);
    loop_timer_add(l, 1000, on_time, (void *)(intptr_t)1);
    loop_timer_add(l, 3000, on_time, (void *)(intptr_t)3);
    loop_run(l);
    loop_destroy(l);
}