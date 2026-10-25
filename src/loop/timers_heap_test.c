
#include "loop/timers_heap.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void test_empty(void) {
    struct timers_heap h;
    timers_heap_init(&h);

    assert(h.count == 0);
    assert(timers_heap_peek(&h) == NULL);
    assert(timers_heap_pop(&h) == NULL);

    timers_heap_destroy(&h);
}

static void test_single(void) {
    struct timers_heap h;
    timers_heap_init(&h);

    struct timer t = {.deadline_ns = 100};

    timers_heap_push(&h, &t);

    assert(h.count == 1);
    assert(timers_heap_peek(&h) == &t);
    assert(timers_heap_pop(&h) == &t);
    assert(h.count == 0);

    timers_heap_destroy(&h);
}

static void test_sorted_order(void) {
    struct timers_heap h;
    timers_heap_init(&h);

    struct timer timers[] = {
        {.deadline_ns = 50}, {.deadline_ns = 10}, {.deadline_ns = 40},
        {.deadline_ns = 20}, {.deadline_ns = 30},
    };

    for (int i = 0; i < 5; i++) {
        timers_heap_push(&h, &timers[i]);
    }

    assert(h.count == 5);
    assert(timers_heap_peek(&h)->deadline_ns == 10);

    for (int expected = 10; expected <= 50; expected += 10) {
        struct timer *t = timers_heap_pop(&h);
        assert(t != NULL);
        assert(t->deadline_ns == (uint64_t)expected);
    }

    assert(h.count == 0);
    assert(timers_heap_pop(&h) == NULL);

    timers_heap_destroy(&h);
}

static void test_duplicates(void) {
    struct timers_heap h;
    timers_heap_init(&h);

    struct timer timers[] = {
        {.deadline_ns = 10},
        {.deadline_ns = 10},
        {.deadline_ns = 5},
        {.deadline_ns = 5},
    };

    for (int i = 0; i < 4; i++) {
        timers_heap_push(&h, &timers[i]);
    }

    assert(timers_heap_pop(&h)->deadline_ns == 5);
    assert(timers_heap_pop(&h)->deadline_ns == 5);
    assert(timers_heap_pop(&h)->deadline_ns == 10);
    assert(timers_heap_pop(&h)->deadline_ns == 10);

    timers_heap_destroy(&h);
}

static void test_growth(void) {
    struct timers_heap h;
    timers_heap_init(&h);

    enum { N = 5 };
    struct timer timers[N];

    // Insert in descending order.
    for (int i = 0; i < N; i++) {
        
        timers[i].deadline_ns = (uint64_t)(N - i);
        timers_heap_push(&h, &timers[i]);

    }

    assert(h.count == N);
    assert(h.cap >= N);

    // Pop must return ascending order.
    for (int i = 1; i <= N; i++) {
        struct timer *t = timers_heap_pop(&h);

        assert(t != NULL);
        assert(t->deadline_ns == (uint64_t)i);
    }

    assert(h.count == 0);

    timers_heap_destroy(&h);
}

static void test_reuse(void) {
    struct timers_heap h;
    timers_heap_init(&h);

    struct timer a = {.deadline_ns = 100};
    struct timer b = {.deadline_ns = 50};

    timers_heap_push(&h, &a);
    assert(timers_heap_pop(&h) == &a);

    timers_heap_push(&h, &b);
    assert(timers_heap_peek(&h) == &b);
    assert(timers_heap_pop(&h) == &b);

    timers_heap_destroy(&h);
}

int main(void) {
    test_empty();
    test_single();
    test_sorted_order();
    test_duplicates();
    test_growth();
    test_reuse();

    puts("All timers_heap tests passed!");
    return 0;
}
