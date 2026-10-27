
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

static void test_init_destroy(void) {
    struct loop *l = loop_new();
    assert(l != NULL);
    loop_destroy(l);
}

static void test_empty_loop(void) {
    struct loop *l = loop_new();
    assert(l != NULL);

    uint64_t start = monotonic_ms();
    loop_run(l);
    uint64_t elapsed = monotonic_ms() - start;

    assert(elapsed < 100);

    loop_destroy(l);
}

static void test_single_timer(void) {
    struct loop *l = loop_new();
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
    struct loop *l = loop_new();
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
    struct loop *l = loop_new();
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
    struct loop *l = loop_new();
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
    struct loop *l = loop_new();
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
    struct loop *l = loop_new();
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
    struct loop *l = loop_new();
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
    struct loop *l = loop_new();
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
    struct loop *l = loop_new();
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
