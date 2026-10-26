
#include "loop.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

            assert(h->items[parent]->deadline_ns <= t->deadline_ns);
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

    struct timer t = {.deadline_ns = 100};

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
        {.deadline_ns = 50}, {.deadline_ns = 10}, {.deadline_ns = 40},
        {.deadline_ns = 20}, {.deadline_ns = 30},
    };

    for (size_t i = 0; i < ARRAY_LEN(timers); i++) {
        timers_heap_push(&h, &timers[i]);
        verify_heap(&h);
    }

    assert(h.count == ARRAY_LEN(timers));
    assert(timers_heap_peek(&h)->deadline_ns == 10);

    for (uint64_t expected = 10; expected <= 50; expected += 10) {
        struct timer *t = timers_heap_pop(&h);

        assert(t != NULL);
        assert(t->deadline_ns == expected);
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
        {.deadline_ns = 10},
        {.deadline_ns = 10},
        {.deadline_ns = 5},
        {.deadline_ns = 5},
    };

    for (size_t i = 0; i < ARRAY_LEN(timers); i++) {
        timers_heap_push(&h, &timers[i]);
        verify_heap(&h);
    }

    const uint64_t expected[] = {5, 5, 10, 10};

    for (size_t i = 0; i < ARRAY_LEN(expected); i++) {
        struct timer *t = timers_heap_pop(&h);

        assert(t != NULL);
        assert(t->deadline_ns == expected[i]);
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
        timers[i].deadline_ns = i;
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
        timers[i].deadline_ns = N - i;
        timers_heap_push(&h, &timers[i]);
        verify_heap(&h);
    }

    for (uint64_t expected = 1; expected <= N; expected++) {
        struct timer *t = timers_heap_pop(&h);

        assert(t != NULL);
        assert(t->deadline_ns == expected);
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
        {.deadline_ns = UINT64_MAX},
        {.deadline_ns = 0},
        {.deadline_ns = UINT64_MAX - 1},
        {.deadline_ns = 1},
    };

    for (size_t i = 0; i < ARRAY_LEN(timers); i++) {
        timers_heap_push(&h, &timers[i]);
        verify_heap(&h);
    }

    const uint64_t expected[] = {0, 1, UINT64_MAX - 1, UINT64_MAX};

    for (size_t i = 0; i < ARRAY_LEN(expected); i++) {
        struct timer *t = timers_heap_pop(&h);

        assert(t != NULL);
        assert(t->deadline_ns == expected[i]);
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
        timers[i].deadline_ns = 42;
        timers_heap_push(&h, &timers[i]);
        verify_heap(&h);
    }

    for (size_t i = 0; i < N; i++) {
        struct timer *t = timers_heap_pop(&h);

        assert(t != NULL);
        assert(t->deadline_ns == 42);
        verify_heap(&h);
    }

    timers_heap_destroy(&h);
}

static void test_peek_does_not_remove(void) {
    struct timers_heap h;
    timers_heap_init(&h);

    struct timer a = {.deadline_ns = 20};
    struct timer b = {.deadline_ns = 10};

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

    struct timer a = {.deadline_ns = 100};
    struct timer b = {.deadline_ns = 50};

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

    struct timer t = {.deadline_ns = 100};

    timers_heap_push(&h, &t);
    assert(timers_heap_pop(&h) == &t);

    t.deadline_ns = 5;

    timers_heap_push(&h, &t);
    assert(timers_heap_peek(&h) == &t);

    verify_heap(&h);

    assert(timers_heap_pop(&h) == &t);
    timers_heap_destroy(&h);
}

static void test_interleaved_operations(void) {
    struct timers_heap h;
    timers_heap_init(&h);

    struct timer a = {.deadline_ns = 30};
    struct timer b = {.deadline_ns = 10};
    struct timer c = {.deadline_ns = 20};
    struct timer d = {.deadline_ns = 5};

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
        timers[i].deadline_ns = N - i;
        timers_heap_push(&h, &timers[i]);
    }

    assert(h.count == N);
    assert(h.cap >= N);
    verify_heap(&h);

    for (uint64_t expected = 1; expected <= N; expected++) {
        struct timer *t = timers_heap_pop(&h);

        assert(t != NULL);
        assert(t->deadline_ns == expected);
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
        timers[i].deadline_ns = next_random() % 1000;
        timers_heap_push(&h, &timers[i]);
    }

    verify_heap(&h);

    uint64_t previous = 0;

    for (size_t i = 0; i < N; i++) {
        struct timer *t = timers_heap_pop(&h);

        assert(t != NULL);
        assert(t->deadline_ns >= previous);

        previous = t->deadline_ns;
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
            timers[inserted].deadline_ns = next_random();
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
        assert(t->deadline_ns >= previous);
        previous = t->deadline_ns;
    }

    timers_heap_destroy(&h);
    free(timers);
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

    printf("--------------------------------------------\n");
    printf(GREEN BOLD "All %d tests passed!" RESET "\n\n", tests_passed);

    return 0;
}
