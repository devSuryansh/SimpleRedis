#include "pq.h"
#include "zmalloc.h"

#include <stdlib.h>

struct pq {
    pqItem *items;
    size_t len;
    size_t cap;
    pqGetIndex getIndex;
    pqSetIndex setIndex;
};

pq *pqCreate(pqGetIndex getIndex, pqSetIndex setIndex) {
    pq *h = zmalloc(sizeof(*h));

    h->items = NULL;
    h->len = 0;
    h->cap = 0;
    h->getIndex = getIndex;
    h->setIndex = setIndex;
    return h;
}

void pqRelease(pq *h) {
    size_t i;

    for (i = 0; i < h->len; i++)
        h->setIndex(h->items[i].entry, (size_t)-1);
    zfree(h->items);
    zfree(h);
}

static void pqSwap(pq *h, size_t i, size_t j) {
    pqItem tmp = h->items[i];

    h->items[i] = h->items[j];
    h->items[j] = tmp;
    h->setIndex(h->items[i].entry, i);
    h->setIndex(h->items[j].entry, j);
}

static void pqSiftUp(pq *h, size_t idx) {
    while (idx > 0) {
        size_t parent = (idx - 1) / 2;

        if (h->items[parent].expire_at_ms <= h->items[idx].expire_at_ms)
            break;
        pqSwap(h, parent, idx);
        idx = parent;
    }
}

static void pqSiftDown(pq *h, size_t idx) {
    for (;;) {
        size_t left = idx * 2 + 1;
        size_t right = left + 1;
        size_t smallest = idx;

        if (left < h->len &&
            h->items[left].expire_at_ms < h->items[smallest].expire_at_ms)
            smallest = left;
        if (right < h->len &&
            h->items[right].expire_at_ms < h->items[smallest].expire_at_ms)
            smallest = right;
        if (smallest == idx)
            break;
        pqSwap(h, idx, smallest);
        idx = smallest;
    }
}

static void pqGrow(pq *h) {
    size_t cap = h->cap ? h->cap * 2 : 8;

    h->items = zrealloc(h->items, cap * sizeof(pqItem));
    h->cap = cap;
}

void pqUpsert(pq *h, void *entry, long long expire_at_ms) {
    size_t idx = h->getIndex(entry);

    if (idx == (size_t)-1) {
        if (h->len == h->cap)
            pqGrow(h);
        idx = h->len++;
        h->items[idx].entry = entry;
        h->items[idx].expire_at_ms = expire_at_ms;
        h->setIndex(entry, idx);
        pqSiftUp(h, idx);
        return;
    }
    if (expire_at_ms < h->items[idx].expire_at_ms) {
        h->items[idx].expire_at_ms = expire_at_ms;
        pqSiftUp(h, idx);
    } else {
        h->items[idx].expire_at_ms = expire_at_ms;
        pqSiftDown(h, idx);
    }
}

void pqRemove(pq *h, void *entry) {
    size_t idx = h->getIndex(entry);
    long long old;

    if (idx == (size_t)-1)
        return;
    h->setIndex(entry, (size_t)-1);
    h->len--;
    if (idx == h->len)
        return;
    old = h->items[idx].expire_at_ms;
    h->items[idx] = h->items[h->len];
    h->setIndex(h->items[idx].entry, idx);
    if (h->items[idx].expire_at_ms < old)
        pqSiftUp(h, idx);
    else
        pqSiftDown(h, idx);
}

const pqItem *pqPeek(const pq *h) {
    if (h->len == 0)
        return NULL;
    return &h->items[0];
}

size_t pqLen(const pq *h) {
    return h->len;
}
