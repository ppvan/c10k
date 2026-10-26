
#include "loop.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ANSI terminal colors */
#define GREEN "\033[32m"
#define RED "\033[31m"
#define BOLD "\033[1m"
#define RESET "\033[0m"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static int tests_passed = 0;

/* ---------------------------------------------------------
 * Test runner
 * --------------------------------------------------------- */

#define RUN_TEST(fn)                                                           \
    do {                                                                       \
        printf("  %-36s", #fn);                                                \
        fflush(stdout);                                                        \
        fn();                                                                  \
        printf(GREEN "PASS" RESET "\n");                                       \
        tests_passed++;                                                        \
    } while (0)


/* ---------------------------------------------------------
 * Helpers
 * --------------------------------------------------------- */

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    int rc = clock_gettime(CLOCK_MONOTONIC, &ts);
    assert(rc == 0);

    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void increment_cb(struct loop *l, void *userdata) {
    (void)l;
    int *count = userdata;
    (*count)++;
}


/* ---------------------------------------------------------
 * Invariant verification
 * --------------------------------------------------------- */

/*
 * Verify:
 * 1. Heap count never exceeds capacity.
 * 2. Parent deadline <= child deadline.
 * 3. No NULL pointers among active elements.
 */
static void verify_heap(const struct timers_heap *h) {
    assert(h->count <= h->cap);

    for (size_t i = 0; i < h->count; i++) {
        struct timer *t = h->items[i];

        assert(t != NULL);

        if (i > 0) {
            size_t parent = (i - 1) / 2;

            assert(h->items[parent]->deadline_ms <= t->deadline_ms);
        }
    }
}

/* ---------------------------------------------------------
 * Basic tests
 * --------------------------------------------------------- */

static void test_empty(void) {
    struct timers_heap h;
    timers_heap_init(&h);

    assert(h.count == 0);
    assert(timers_heap_peek(&h) == NULL);
    assert(timers_heap_pop(&h) == NULL);

    verify_heap(&h);
    timers_heap_destroy(&h);
}

static void test_single(void) {
    struct timers_heap h;
    timers_heap_init(&h);

    struct timer t = {.deadline_ms = 100};

    timers_heap_push(&h, &t);

    assert(h.count == 1);
    assert(timers_heap_peek(&h) == &t);

    verify_heap(&h);

    assert(timers_heap_pop(&h) == &t);
    assert(h.count == 0);
    assert(timers_heap_peek(&h) == NULL);

    timers_heap_destroy(&h);
}

static void test_sorted_order(void) {
    struct timers_heap h;
    timers_heap_init(&h);

    struct timer timers[] = {
        {.deadline_ms = 50}, {.deadline_ms = 10}, {.deadline_ms = 40},
        {.deadline_ms = 20}, {.deadline_ms = 30},
    };

    for (size_t i = 0; i < ARRAY_LEN(timers); i++) {
        timers_heap_push(&h, &timers[i]);
        verify_heap(&h);
    }

    assert(h.count == ARRAY_LEN(timers));
    assert(timers_heap_peek(&h)->deadline_ms == 10);

    for (uint64_t expected = 10; expected <= 50; expected += 10) {
        struct timer *t = timers_heap_pop(&h);

        assert(t != NULL);
        assert(t->deadline_ms == expected);
        verify_heap(&h);
    }

    assert(h.count == 0);
    assert(timers_heap_pop(&h) == NULL);

    timers_heap_destroy(&h);
}

static void test_duplicates(void) {
    struct timers_heap h;
    timers_heap_init(&h);

    struct timer timers[] = {
        {.deadline_ms = 10},
        {.deadline_ms = 10},
        {.deadline_ms = 5},
        {.deadline_ms = 5},
    };

    for (size_t i = 0; i < ARRAY_LEN(timers); i++) {
        timers_heap_push(&h, &timers[i]);
        verify_heap(&h);
    }

    const uint64_t expected[] = {5, 5, 10, 10};

    for (size_t i = 0; i < ARRAY_LEN(expected); i++) {
        struct timer *t = timers_heap_pop(&h);

        assert(t != NULL);
        assert(t->deadline_ms == expected[i]);
        verify_heap(&h);
    }

    timers_heap_destroy(&h);
}

static void test_ascending_insert(void) {
    struct timers_heap h;
    timers_heap_init(&h);

    enum { N = 100 };
    struct timer timers[N] = {0};

    for (size_t i = 0; i < N; i++) {
        timers[i].deadline_ms = i;
        timers_heap_push(&h, &timers[i]);
        verify_heap(&h);
    }

    for (size_t i = 0; i < N; i++) {
        assert(timers_heap_pop(&h) == &timers[i]);
        verify_heap(&h);
    }

    timers_heap_destroy(&h);
}

static void test_descending_insert(void) {
    struct timers_heap h;
    timers_heap_init(&h);

    enum { N = 100 };
    struct timer timers[N] = {0};

    for (size_t i = 0; i < N; i++) {
        timers[i].deadline_ms = N - i;
        timers_heap_push(&h, &timers[i]);
        verify_heap(&h);
    }

    for (uint64_t expected = 1; expected <= N; expected++) {
        struct timer *t = timers_heap_pop(&h);

        assert(t != NULL);
        assert(t->deadline_ms == expected);
        verify_heap(&h);
    }

    timers_heap_destroy(&h);
}

/* ---------------------------------------------------------
 * Edge cases
 * --------------------------------------------------------- */

static void test_uint64_boundaries(void) {
    struct timers_heap h;
    timers_heap_init(&h);

    struct timer timers[] = {
        {.deadline_ms = UINT64_MAX},
        {.deadline_ms = 0},
        {.deadline_ms = UINT64_MAX - 1},
        {.deadline_ms = 1},
    };

    for (size_t i = 0; i < ARRAY_LEN(timers); i++) {
        timers_heap_push(&h, &timers[i]);
        verify_heap(&h);
    }

    const uint64_t expected[] = {0, 1, UINT64_MAX - 1, UINT64_MAX};

    for (size_t i = 0; i < ARRAY_LEN(expected); i++) {
        struct timer *t = timers_heap_pop(&h);

        assert(t != NULL);
        assert(t->deadline_ms == expected[i]);
        verify_heap(&h);
    }

    timers_heap_destroy(&h);
}

static void test_all_equal(void) {
    struct timers_heap h;
    timers_heap_init(&h);

    enum { N = 128 };
    struct timer timers[N] = {0};

    for (size_t i = 0; i < N; i++) {
        timers[i].deadline_ms = 42;
        timers_heap_push(&h, &timers[i]);
        verify_heap(&h);
    }

    for (size_t i = 0; i < N; i++) {
        struct timer *t = timers_heap_pop(&h);

        assert(t != NULL);
        assert(t->deadline_ms == 42);
        verify_heap(&h);
    }

    timers_heap_destroy(&h);
}

static void test_peek_does_not_remove(void) {
    struct timers_heap h;
    timers_heap_init(&h);

    struct timer a = {.deadline_ms = 20};
    struct timer b = {.deadline_ms = 10};

    timers_heap_push(&h, &a);
    timers_heap_push(&h, &b);

    for (int i = 0; i < 100; i++) {
        assert(timers_heap_peek(&h) == &b);
        assert(h.count == 2);
    }

    verify_heap(&h);
    timers_heap_destroy(&h);
}

static void test_reuse_after_empty(void) {
    struct timers_heap h;
    timers_heap_init(&h);

    struct timer a = {.deadline_ms = 100};
    struct timer b = {.deadline_ms = 50};

    timers_heap_push(&h, &a);
    assert(timers_heap_pop(&h) == &a);

    assert(h.count == 0);

    timers_heap_push(&h, &b);
    assert(timers_heap_peek(&h) == &b);
    assert(timers_heap_pop(&h) == &b);

    verify_heap(&h);
    timers_heap_destroy(&h);
}

static void test_reinsert_same_timer(void) {
    struct timers_heap h;
    timers_heap_init(&h);

    struct timer t = {.deadline_ms = 100};

    timers_heap_push(&h, &t);
    assert(timers_heap_pop(&h) == &t);

    t.deadline_ms = 5;

    timers_heap_push(&h, &t);
    assert(timers_heap_peek(&h) == &t);

    verify_heap(&h);

    assert(timers_heap_pop(&h) == &t);
    timers_heap_destroy(&h);
}

static void test_interleaved_operations(void) {
    struct timers_heap h;
    timers_heap_init(&h);

    struct timer a = {.deadline_ms = 30};
    struct timer b = {.deadline_ms = 10};
    struct timer c = {.deadline_ms = 20};
    struct timer d = {.deadline_ms = 5};

    timers_heap_push(&h, &a);
    timers_heap_push(&h, &b);
    verify_heap(&h);

    assert(timers_heap_pop(&h) == &b);
    verify_heap(&h);

    timers_heap_push(&h, &c);
    timers_heap_push(&h, &d);
    verify_heap(&h);

    assert(timers_heap_pop(&h) == &d);
    assert(timers_heap_pop(&h) == &c);
    assert(timers_heap_pop(&h) == &a);

    assert(h.count == 0);
    verify_heap(&h);

    timers_heap_destroy(&h);
}

/* ---------------------------------------------------------
 * Stress tests
 * --------------------------------------------------------- */

static void test_large_growth(void) {
    struct timers_heap h;
    timers_heap_init(&h);

    enum { N = 10000 };
    struct timer *timers = calloc(N, sizeof(*timers));
    assert(timers != NULL);

    for (size_t i = 0; i < N; i++) {
        timers[i].deadline_ms = N - i;
        timers_heap_push(&h, &timers[i]);
    }

    assert(h.count == N);
    assert(h.cap >= N);
    verify_heap(&h);

    for (uint64_t expected = 1; expected <= N; expected++) {
        struct timer *t = timers_heap_pop(&h);

        assert(t != NULL);
        assert(t->deadline_ms == expected);
    }

    assert(h.count == 0);

    timers_heap_destroy(&h);
    free(timers);
}

/* Deterministic pseudo-random generator */
static uint32_t rng_state = 0x12345678;

static uint32_t next_random(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}

static void test_randomized_order(void) {
    struct timers_heap h;
    timers_heap_init(&h);

    enum { N = 10000 };
    struct timer *timers = calloc(N, sizeof(*timers));
    assert(timers != NULL);

    for (size_t i = 0; i < N; i++) {
        timers[i].deadline_ms = next_random() % 1000;
        timers_heap_push(&h, &timers[i]);
    }

    verify_heap(&h);

    uint64_t previous = 0;

    for (size_t i = 0; i < N; i++) {
        struct timer *t = timers_heap_pop(&h);

        assert(t != NULL);
        assert(t->deadline_ms >= previous);

        previous = t->deadline_ms;
    }

    assert(h.count == 0);

    timers_heap_destroy(&h);
    free(timers);
}

static void test_randomized_interleaving(void) {
    struct timers_heap h;
    timers_heap_init(&h);

    enum { N = 2000, OPS = 10000 };

    struct timer *timers = calloc(N, sizeof(*timers));
    assert(timers != NULL);

    size_t inserted = 0;
    size_t removed = 0;

    for (size_t op = 0; op < OPS; op++) {
        bool can_push = inserted < N;
        bool can_pop = h.count > 0;

        if (can_push && (!can_pop || (next_random() & 1))) {
            timers[inserted].deadline_ms = next_random();
            timers_heap_push(&h, &timers[inserted]);
            inserted++;
        } else if (can_pop) {
            struct timer *t = timers_heap_pop(&h);
            assert(t != NULL);
            removed++;
        }

        assert(h.count == inserted - removed);
        verify_heap(&h);
    }

    uint64_t previous = 0;

    while (h.count > 0) {
        struct timer *t = timers_heap_pop(&h);

        assert(t != NULL);
        assert(t->deadline_ms >= previous);
        previous = t->deadline_ms;
    }

    timers_heap_destroy(&h);
    free(timers);
}

/* ---------------------------------------------------------
 * Basic tests
 * --------------------------------------------------------- */

static void test_init_destroy(void) {
    struct loop *l = loop_init();
    assert(l != NULL);
    loop_destroy(l);
}

static void test_empty_loop(void) {
    struct loop *l = loop_init();
    assert(l != NULL);

    uint64_t start = monotonic_ms();
    loop_run(l);
    uint64_t elapsed = monotonic_ms() - start;

    assert(elapsed < 100);

    loop_destroy(l);
}

static void test_single_timer(void) {
    struct loop *l = loop_init();
    assert(l != NULL);

    int count = 0;

    loop_timer_add(l, 10, increment_cb, &count);
    loop_run(l);

    assert(count == 1);

    loop_destroy(l);
}

/* ---------------------------------------------------------
 * Timer ordering
 * --------------------------------------------------------- */

struct order_ctx {
    int values[16];
    size_t count;
};

struct order_event {
    struct order_ctx *ctx;
    int value;
};

static void order_cb(struct loop *l, void *userdata) {
    (void)l;

    struct order_event *event = userdata;
    struct order_ctx *ctx = event->ctx;

    assert(ctx->count < ARRAY_LEN(ctx->values));
    ctx->values[ctx->count++] = event->value;
}

static void test_timer_order(void) {
    struct loop *l = loop_init();
    assert(l != NULL);

    struct order_ctx ctx = {0};

    struct order_event events[] = {
        {&ctx, 5}, {&ctx, 1}, {&ctx, 3}, {&ctx, 2}, {&ctx, 4},
    };

    loop_timer_add(l, 50, order_cb, &events[0]);
    loop_timer_add(l, 10, order_cb, &events[1]);
    loop_timer_add(l, 30, order_cb, &events[2]);
    loop_timer_add(l, 20, order_cb, &events[3]);
    loop_timer_add(l, 40, order_cb, &events[4]);

    loop_run(l);

    assert(ctx.count == 5);

    for (size_t i = 0; i < ctx.count; i++) {
        assert(ctx.values[i] == (int)i + 1);
    }

    loop_destroy(l);
}

static void test_same_deadline(void) {
    struct loop *l = loop_init();
    assert(l != NULL);

    enum { N = 100 };
    int count = 0;

    for (int i = 0; i < N; i++) {
        loop_timer_add(l, 10, increment_cb, &count);
    }

    loop_run(l);

    assert(count == N);

    /* Equal deadlines do not imply stable ordering. */

    loop_destroy(l);
}

/* ---------------------------------------------------------
 * Callback behavior
 * --------------------------------------------------------- */

static void test_userdata(void) {
    struct loop *l = loop_init();
    assert(l != NULL);

    int a = 0;
    int b = 10;
    int c = 100;

    loop_timer_add(l, 10, increment_cb, &a);
    loop_timer_add(l, 10, increment_cb, &b);
    loop_timer_add(l, 10, increment_cb, &c);

    loop_run(l);

    assert(a == 1);
    assert(b == 11);
    assert(c == 101);

    loop_destroy(l);
}

static void test_zero_delay_timer(void) {
    struct loop *l = loop_init();
    assert(l != NULL);

    int count = 0;

    loop_timer_add(l, 0, increment_cb, &count);
    loop_run(l);

    assert(count == 1);

    loop_destroy(l);
}

/* ---------------------------------------------------------
 * Timing correctness
 * --------------------------------------------------------- */

struct timing_ctx {
    uint64_t fired_at;
    int count;
};

static void timing_cb(struct loop *l, void *userdata) {
    (void)l;

    struct timing_ctx *ctx = userdata;
    ctx->fired_at = monotonic_ms();
    ctx->count++;
}

static void test_timer_not_early(void) {
    struct loop *l = loop_init();
    assert(l != NULL);

    struct timing_ctx ctx = {0};

    uint64_t start = monotonic_ms();

    loop_timer_add(l, 50, timing_cb, &ctx);
    loop_run(l);

    assert(ctx.count == 1);
    assert(ctx.fired_at >= start);
    assert(ctx.fired_at - start >= 50);

    loop_destroy(l);
}

/* ---------------------------------------------------------
 * Dynamic scheduling
 * --------------------------------------------------------- */

struct chain_ctx {
    int count;
    int target;
};

static void chain_cb(struct loop *l, void *userdata) {
    struct chain_ctx *ctx = userdata;

    ctx->count++;

    if (ctx->count < ctx->target) {
        loop_timer_add(l, 1, chain_cb, ctx);
    }
}

static void test_timer_added_from_callback(void) {
    struct loop *l = loop_init();
    assert(l != NULL);

    struct chain_ctx ctx = {
        .count = 0,
        .target = 10,
    };

    loop_timer_add(l, 1, chain_cb, &ctx);
    loop_run(l);

    assert(ctx.count == ctx.target);

    loop_destroy(l);
}

/* ---------------------------------------------------------
 * Stress tests
 * --------------------------------------------------------- */

static void test_many_timers(void) {
    struct loop *l = loop_init();
    assert(l != NULL);

    enum { N = 1000 };
    int count = 0;

    for (int i = 0; i < N; i++) {
        loop_timer_add(l, (size_t)(i % 20), increment_cb, &count);
    }

    loop_run(l);

    assert(count == N);

    loop_destroy(l);
}

/* ---------------------------------------------------------
 * Loop reuse
 * --------------------------------------------------------- */

static void test_reuse_loop(void) {
    struct loop *l = loop_init();
    assert(l != NULL);

    int count = 0;

    loop_timer_add(l, 1, increment_cb, &count);
    loop_run(l);

    assert(count == 1);

    loop_timer_add(l, 1, increment_cb, &count);
    loop_run(l);

    assert(count == 2);

    loop_destroy(l);
}


/* ---------------------------------------------------------
 * Main
 * --------------------------------------------------------- */

int main(void) {
    printf("\n" BOLD "Timers Heap Test Suite" RESET "\n");
    printf("--------------------------------------------\n");

    RUN_TEST(test_empty);
    RUN_TEST(test_single);
    RUN_TEST(test_sorted_order);
    RUN_TEST(test_duplicates);
    RUN_TEST(test_ascending_insert);
    RUN_TEST(test_descending_insert);
    RUN_TEST(test_uint64_boundaries);
    RUN_TEST(test_all_equal);
    RUN_TEST(test_peek_does_not_remove);
    RUN_TEST(test_reuse_after_empty);
    RUN_TEST(test_reinsert_same_timer);
    RUN_TEST(test_interleaved_operations);
    RUN_TEST(test_large_growth);
    RUN_TEST(test_randomized_order);
    RUN_TEST(test_randomized_interleaving);


    printf("\n" BOLD "Event Loop Test Suite" RESET "\n");
    printf("------------------------------------------------\n");

    RUN_TEST(test_init_destroy);
    RUN_TEST(test_empty_loop);
    RUN_TEST(test_single_timer);
    RUN_TEST(test_timer_order);
    RUN_TEST(test_same_deadline);
    RUN_TEST(test_userdata);
    RUN_TEST(test_zero_delay_timer);
    RUN_TEST(test_timer_not_early);
    RUN_TEST(test_timer_added_from_callback);
    RUN_TEST(test_many_timers);
    RUN_TEST(test_reuse_loop);

    printf("------------------------------------------------\n");
    printf(GREEN BOLD "All %d tests passed!" RESET "\n\n", tests_passed);

    return 0;
}
