
#include "loop/timers_heap.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>


static void heapify_up(struct timers_heap *h, int index) {
    while (index > 0 && h->items[(index - 1) / 2]->deadline_ns >
                            h->items[index]->deadline_ns) {
        struct timer *tmp = h->items[(index - 1) / 2];
        h->items[(index - 1) / 2] = h->items[index];
        h->items[index] = tmp;

        index = (index - 1) / 2;
    }
}

static void heapify_down(struct timers_heap *h, int index) {
    int smallest = index;
    int left = 2 * index + 1;
    int right = 2 * index + 2;

    // Check if left child is smaller than the current smallest
    if (left < h->count &&
        h->items[left]->deadline_ns < h->items[smallest]->deadline_ns) {
        smallest = left;
    }
    // Check if right child is smaller than the current smallest
    if (right < h->count &&
        h->items[right]->deadline_ns < h->items[smallest]->deadline_ns) {
        smallest = right;
    }

    // If the smallest is not the parent, swap and recurse down
    if (smallest != index) {
        struct timer *tmp = h->items[index];
        h->items[index] = h->items[smallest];
        h->items[smallest] = tmp;

        heapify_down(h, smallest);
    }
}

void timers_heap_init(struct timers_heap *h) {
    h->cap = 256;
    h->count = 0;
    h->items = malloc((size_t)h->cap * sizeof(struct timer *));
}

void timers_heap_destroy(struct timers_heap *h) {
    // for (size_t i = 0; i < h->count; i++) {
    //     free(h->items[i]);
    //     h->items[i] = NULL;
    // }

    free(h->items);
    h->items = NULL;
}

void timers_heap_push(struct timers_heap *h, struct timer *t) {
    if (h->count >= h->cap) {
        h->cap *= 2;
        h->items = realloc(h->items, (size_t)h->cap * sizeof(struct timer *));

        assert(h->items && "out of memory, buy more ram");
    }

    h->items[h->count++] = t;
    heapify_up(h, h->count - 1);
}

struct timer *timers_heap_peek(const struct timers_heap *h) {
    if (h->count == 0) {
        return NULL;
    }
    return h->items[0];
}

struct timer *timers_heap_pop(struct timers_heap *h) {
    if (h->count <= 0) {
        return NULL;
    }

    struct timer *root = h->items[0];

    h->items[0] = h->items[h->count - 1];
    h->count--;
    if (h->count > 0) {
        heapify_down(h, 0);
    }

    return root;
}
