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

/* Seed of the randomized test currently running (0 = none). Printed on
 * failure so a run can be replayed with: SEED=<n> ./test */
static uint64_t current_seed = 0;

/* Like assert(), but always evaluates its argument (safe with -DNDEBUG, so
 * it can wrap calls with side effects such as loop_timer_cancel()). */
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "\n" RED "CHECK failed" RESET ": %s (%s:%d)\n",    \
                    #cond, __FILE__, __LINE__);                                \
            if (current_seed != 0) {                                           \
                fprintf(stderr, "  replay with: SEED=%llu ./test\n",           \
                        (unsigned long long)current_seed);                     \
            }                                                                  \
            abort();                                                           \
        }                                                                      \
    } while (0)

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
    (void)rc;

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
 * Cancel: basic behavior
 * --------------------------------------------------------- */

static void test_cancel_prevents_fire(void) {
    struct loop *l = loop_new();
    CHECK(l != NULL);

    int cancelled = 0;
    int kept = 0;

    uint64_t id = loop_timer_add(l, 10, increment_cb, &cancelled);
    loop_timer_add(l, 20, increment_cb, &kept);

    CHECK(loop_timer_cancel(l, id));
    loop_run(l);

    CHECK(cancelled == 0);
    CHECK(kept == 1);

    loop_destroy(l);
}

static void test_cancel_only_timer(void) {
    struct loop *l = loop_new();
    CHECK(l != NULL);

    int count = 0;
    uint64_t id = loop_timer_add(l, 5000, increment_cb, &count);
    CHECK(loop_timer_cancel(l, id));

    /* Heap is empty again, so run must return immediately. */
    uint64_t start = monotonic_ms();
    loop_run(l);
    CHECK(monotonic_ms() - start < 100);
    CHECK(count == 0);

    loop_destroy(l);
}

static void test_cancel_twice(void) {
    struct loop *l = loop_new();
    CHECK(l != NULL);

    int count = 0;
    uint64_t id = loop_timer_add(l, 10, increment_cb, &count);
    loop_timer_add(l, 20, increment_cb, &count);

    CHECK(loop_timer_cancel(l, id));
    CHECK(!loop_timer_cancel(l, id));
    CHECK(!loop_timer_cancel(l, id));

    loop_run(l);
    CHECK(count == 1);

    loop_destroy(l);
}

static void test_cancel_after_fire(void) {
    struct loop *l = loop_new();
    CHECK(l != NULL);

    int count = 0;
    uint64_t id = loop_timer_add(l, 1, increment_cb, &count);
    loop_run(l);
    CHECK(count == 1);

    CHECK(!loop_timer_cancel(l, id));

    loop_destroy(l);
}

static void test_cancel_invalid_ids(void) {
    struct loop *l = loop_new();
    CHECK(l != NULL);

    /* On an empty loop: must not touch the heap or underflow counters. */
    CHECK(!loop_timer_cancel(l, 0));
    CHECK(!loop_timer_cancel(l, UINT64_MAX));
    CHECK(!loop_timer_cancel(l, ((uint64_t)1 << 32) | 99999));

    /* With a live timer: wrong generation / out-of-range slot are rejected,
     * and the real id still works afterwards. */
    int count = 0;
    uint64_t id = loop_timer_add(l, 10, increment_cb, &count);
    CHECK(id != 0);
    CHECK(!loop_timer_cancel(l, id + ((uint64_t)1 << 32)));
    CHECK(!loop_timer_cancel(l, 0));
    CHECK(!loop_timer_cancel(l, UINT64_MAX));

    loop_run(l);
    CHECK(count == 1);

    loop_destroy(l);
}

/* A cancelled timer that was NOT the last heap element gets replaced by the
 * last element, which may be smaller than its new parent (needs sift-up).
 *
 * Heap array after pushes (no sifting happens on push):
 *   idx: 0   1    2   3    4    5   6
 *        10  100  20  110  120  30  40
 * Cancel 110 (idx 3): 40 moves to idx 3, its parent is 100, so 40 must
 * sift up. Without heapify_up, 100 would fire before 40. */
static void test_cancel_triggers_sift_up(void) {
    struct loop *l = loop_new();
    CHECK(l != NULL);

    struct order_ctx ctx = {0};
    const int delays[] = {10, 100, 20, 110, 120, 30, 40};
    struct order_event events[ARRAY_LEN(delays)];
    uint64_t ids[ARRAY_LEN(delays)];

    for (size_t i = 0; i < ARRAY_LEN(delays); i++) {
        events[i] = (struct order_event){&ctx, delays[i]};
        ids[i] = loop_timer_add(l, (size_t)delays[i], order_cb, &events[i]);
    }

    CHECK(loop_timer_cancel(l, ids[3]));
    loop_run(l);

    const int expected[] = {10, 20, 30, 40, 100, 120};
    CHECK(ctx.count == ARRAY_LEN(expected));
    for (size_t i = 0; i < ARRAY_LEN(expected); i++) {
        CHECK(ctx.values[i] == expected[i]);
    }

    loop_destroy(l);
}

/* ---------------------------------------------------------
 * Cancel: IDs and slot reuse
 * --------------------------------------------------------- */

/* Regression: releasing a slot must bump THAT slot's generation. */
static void test_slot_reuse_rejects_stale_id(void) {
    struct loop *l = loop_new();
    CHECK(l != NULL);

    int count = 0;
    uint64_t a = loop_timer_add(l, 1000, increment_cb, &count); /* slot 0 */
    uint64_t b = loop_timer_add(l, 1000, increment_cb, &count); /* slot 1 */

    CHECK(loop_timer_cancel(l, b));
    uint64_t d = loop_timer_add(l, 1000, increment_cb, &count); /* reuses 1 */

    CHECK(d != b);                  /* same slot, new generation */
    CHECK(!loop_timer_cancel(l, b)); /* stale id must not kill d */
    CHECK(loop_timer_cancel(l, a)); /* slot 0 must still be cancellable */
    CHECK(loop_timer_cancel(l, d)); /* d must still be alive */

    loop_run(l);
    CHECK(count == 0);

    loop_destroy(l);
}

static void test_ids_unique_and_nonzero(void) {
    struct loop *l = loop_new();
    CHECK(l != NULL);

    enum { N = 1000 }; /* > initial 256 slots, forces slots_grow() */
    static uint64_t ids[N];
    int count = 0;

    for (int i = 0; i < N; i++) {
        ids[i] = loop_timer_add(l, 5000, increment_cb, &count);
        CHECK(ids[i] != 0);
    }

    for (int i = 0; i < N; i++) {
        for (int j = i + 1; j < N; j++) {
            CHECK(ids[i] != ids[j]);
        }
    }

    for (int i = 0; i < N; i++) {
        CHECK(loop_timer_cancel(l, ids[i]));
    }

    loop_destroy(l);
}

static void test_cancel_all_then_reuse(void) {
    struct loop *l = loop_new();
    CHECK(l != NULL);

    enum { N = 1000 };
    static uint64_t ids[N];
    int count = 0;

    for (int i = 0; i < N; i++) {
        ids[i] = loop_timer_add(l, 5000, increment_cb, &count);
    }
    for (int i = N - 1; i >= 0; i--) { /* reverse order */
        CHECK(loop_timer_cancel(l, ids[i]));
    }

    uint64_t start = monotonic_ms();
    loop_run(l);
    CHECK(monotonic_ms() - start < 100);
    CHECK(count == 0);

    /* Every old id is stale now. */
    for (int i = 0; i < N; i++) {
        CHECK(!loop_timer_cancel(l, ids[i]));
    }

    /* All slots are free again and fully usable. */
    for (int i = 0; i < N; i++) {
        loop_timer_add(l, 1, increment_cb, &count);
    }
    loop_run(l);
    CHECK(count == N);

    loop_destroy(l);
}

static void test_destroy_with_pending_timers(void) {
    struct loop *l = loop_new();
    CHECK(l != NULL);

    int count = 0;
    uint64_t ids[300];

    for (size_t i = 0; i < ARRAY_LEN(ids); i++) {
        ids[i] = loop_timer_add(l, 5000, increment_cb, &count);
    }
    for (size_t i = 0; i < ARRAY_LEN(ids); i += 3) {
        CHECK(loop_timer_cancel(l, ids[i]));
    }

    /* Never run: destroy must free what is left (AddressSanitizer's leak
     * checker reports it otherwise). */
    loop_destroy(l);
    CHECK(count == 0);
}

/* ---------------------------------------------------------
 * Cancel from inside callbacks
 * --------------------------------------------------------- */

struct self_ctx {
    uint64_t id;
    int fired;
    bool cancel_result;
};

static void self_cancel_cb(struct loop *l, void *userdata) {
    struct self_ctx *ctx = userdata;
    ctx->fired++;
    /* The id is released before the handler runs, so this must be a no-op. */
    ctx->cancel_result = loop_timer_cancel(l, ctx->id);
}

static void test_self_cancel_in_callback(void) {
    struct loop *l = loop_new();
    CHECK(l != NULL);

    struct self_ctx ctx = {0};
    ctx.cancel_result = true;
    ctx.id = loop_timer_add(l, 5, self_cancel_cb, &ctx);

    loop_run(l);

    CHECK(ctx.fired == 1);
    CHECK(!ctx.cancel_result);

    loop_destroy(l);
}

struct killer_ctx {
    uint64_t victim;
    bool result;
};

static void killer_cb(struct loop *l, void *userdata) {
    struct killer_ctx *ctx = userdata;
    ctx->result = loop_timer_cancel(l, ctx->victim);
}

static void test_cancel_other_from_callback(void) {
    struct loop *l = loop_new();
    CHECK(l != NULL);

    int victim_fired = 0;
    int bystander_fired = 0;
    struct killer_ctx killer = {0};

    killer.victim = loop_timer_add(l, 50, increment_cb, &victim_fired);
    loop_timer_add(l, 5, killer_cb, &killer);
    loop_timer_add(l, 60, increment_cb, &bystander_fired);

    loop_run(l);

    CHECK(killer.result);
    CHECK(victim_fired == 0);
    CHECK(bystander_fired == 1);

    loop_destroy(l);
}

/* ---------------------------------------------------------
 * Randomized, model-based tests
 *
 * A model tracks, for every timer ever added, whether it is live, fired or
 * cancelled. Random add/cancel operations (including cancels of dead ids)
 * run against the loop, and every loop_timer_cancel() result must match
 * the model. After loop_run() we check:
 *   - every timer fired exactly once or was cancelled, never both
 *   - nothing fired before its deadline
 *   - timers fired in non-decreasing deadline order
 *   - every id is stale afterwards
 * In callback mode, handlers also cancel random timers and add new ones.
 *
 * Replay a failure with:  SEED=<n> ./test
 * --------------------------------------------------------- */

static uint64_t rng_state = 1;

static uint64_t rng_next(void) {
    /* xorshift64 */
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

static uint64_t rng_below(uint64_t n) { return rng_next() % n; }

static void rng_seed(uint64_t seed) {
    rng_state = (seed + 1) * 0x9E3779B97F4A7C15ULL;
    if (rng_state == 0) {
        rng_state = 1;
    }
}

enum { RT_MAX = 2000, RT_INITIAL_OPS = 1500, RT_MAX_DELAY_MS = 30 };

struct rt_world;

struct rt_entry {
    struct rt_world *w;
    size_t idx;
    uint64_t id;
    uint64_t lo; /* earliest possible deadline (measured before add) */
    uint64_t hi; /* latest possible deadline (measured after add)    */
    uint64_t fired_at;
    bool live;
    bool fired;
    bool cancelled;
};

struct rt_world {
    struct loop *l;
    struct rt_entry *e; /* fixed size: entries are handed out as userdata */
    size_t n;
    size_t *order; /* entry indexes in the order they fired */
    size_t order_n;
    bool cb_ops;
};

static void rt_cb(struct loop *l, void *userdata);

static void rt_add(struct rt_world *w, uint64_t delay) {
    CHECK(w->n < RT_MAX);

    struct rt_entry *e = &w->e[w->n];
    memset(e, 0, sizeof(*e));
    e->w = w;
    e->idx = w->n;
    e->live = true;

    e->lo = monotonic_ms() + delay;
    e->id = loop_timer_add(w->l, (size_t)delay, rt_cb, e);
    e->hi = monotonic_ms() + delay;

    CHECK(e->id != 0);
    for (size_t i = 0; i < w->n; i++) {
        CHECK(!(w->e[i].live && w->e[i].id == e->id));
    }

    w->n++;
}

static void rt_cancel_random(struct rt_world *w) {
    if (w->n == 0) {
        return;
    }

    struct rt_entry *e = &w->e[rng_below(w->n)];
    bool expected = e->live;
    bool result = loop_timer_cancel(w->l, e->id);

    CHECK(result == expected);
    if (result) {
        e->live = false;
        e->cancelled = true;
    }
}

static void rt_cb(struct loop *l, void *userdata) {
    struct rt_entry *e = userdata;
    struct rt_world *w = e->w;

    CHECK(l == w->l);
    CHECK(e->live); /* never fire a cancelled or already-fired timer */

    e->live = false;
    e->fired = true;
    e->fired_at = monotonic_ms();
    CHECK(w->order_n < RT_MAX);
    w->order[w->order_n++] = e->idx;

    /* Own id is already released: cancelling it must fail. */
    CHECK(!loop_timer_cancel(l, e->id));

    if (!w->cb_ops) {
        return;
    }

    for (int k = 0; k < 2; k++) {
        uint64_t r = rng_below(10);
        if (r < 4) {
            rt_cancel_random(w);
        } else if (r < 6 && w->n < RT_MAX) {
            rt_add(w, rng_below(RT_MAX_DELAY_MS));
        }
    }
}

static void rt_run(uint64_t seed, bool cb_ops) {
    current_seed = seed;
    rng_seed(seed);

    struct rt_world w = {0};
    w.cb_ops = cb_ops;
    w.e = calloc(RT_MAX, sizeof(*w.e));
    w.order = calloc(RT_MAX, sizeof(*w.order));
    CHECK(w.e != NULL && w.order != NULL);
    w.l = loop_new();
    CHECK(w.l != NULL);

    /* Phase 1: random adds and cancels, nothing fires yet. */
    for (int i = 0; i < RT_INITIAL_OPS; i++) {
        if (rng_below(100) < 60) {
            rt_add(&w, rng_below(RT_MAX_DELAY_MS));
        } else {
            rt_cancel_random(&w);
        }
    }

    /* Phase 2: let everything fire (handlers may mutate in cb_ops mode). */
    loop_run(w.l);

    /* Phase 3: verify. */
    size_t fired = 0;
    for (size_t i = 0; i < w.n; i++) {
        struct rt_entry *e = &w.e[i];
        CHECK(!e->live);
        CHECK(e->fired != e->cancelled);
        if (e->fired) {
            fired++;
        }
    }
    CHECK(fired == w.order_n);

    /* If A fired before B then deadline(A) <= deadline(B), which implies
     * lo(A) <= hi(B) given the measurement windows. */
    uint64_t max_lo = 0;
    for (size_t k = 0; k < w.order_n; k++) {
        struct rt_entry *e = &w.e[w.order[k]];
        CHECK(e->fired_at >= e->lo); /* not early */
        CHECK(max_lo <= e->hi);      /* ordered */
        if (e->lo > max_lo) {
            max_lo = e->lo;
        }
    }

    /* Every id ever handed out is dead now, slots reused or not. */
    for (size_t i = 0; i < w.n; i++) {
        CHECK(!loop_timer_cancel(w.l, w.e[i].id));
    }

    loop_destroy(w.l);
    free(w.e);
    free(w.order);
    current_seed = 0;
}

static void rt_for_each_seed(bool cb_ops, uint64_t first) {
    const char *env = getenv("SEED");
    if (env != NULL) {
        rt_run(strtoull(env, NULL, 10), cb_ops);
        return;
    }
    for (uint64_t seed = first; seed < first + 20; seed++) {
        rt_run(seed, cb_ops);
    }
}

static void test_random_add_cancel(void) { rt_for_each_seed(false, 1); }

static void test_random_with_callbacks(void) { rt_for_each_seed(true, 1000); }

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

    RUN_TEST(test_cancel_prevents_fire);
    RUN_TEST(test_cancel_only_timer);
    RUN_TEST(test_cancel_twice);
    RUN_TEST(test_cancel_after_fire);
    RUN_TEST(test_cancel_invalid_ids);
    RUN_TEST(test_cancel_triggers_sift_up);
    RUN_TEST(test_slot_reuse_rejects_stale_id);
    RUN_TEST(test_ids_unique_and_nonzero);
    RUN_TEST(test_cancel_all_then_reuse);
    RUN_TEST(test_destroy_with_pending_timers);
    RUN_TEST(test_self_cancel_in_callback);
    RUN_TEST(test_cancel_other_from_callback);

    RUN_TEST(test_random_add_cancel);
    RUN_TEST(test_random_with_callbacks);

    printf("------------------------------------------------\n");
    printf(GREEN BOLD "All %d tests passed!" RESET "\n\n", tests_passed);

    return 0;
}