#include "loop.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>


void on_time(struct loop *l, void *userdata) {
    int num = (int)(intptr_t)userdata;
    if (num > 0) {
        printf("Hello to number: %d\n", num);
        num--;
        size_t expire_ns = 1000000000; // 1 sec
        loop_timer_add(l, expire_ns, on_time, (void *)(intptr_t)num);
    }
}

int main(void) {
    struct loop *l = loop_init();
    size_t expire_ns = 1000000000; // 1 sec
    int n = 5;

    loop_timer_add(l, expire_ns, on_time, (void *)(intptr_t)n);
    loop_run(l);
    loop_destroy(l);
}