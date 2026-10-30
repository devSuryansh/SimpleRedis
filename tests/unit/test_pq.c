#include "test.h"
#include "pq.h"

#include <stddef.h>

typedef struct ent {
    size_t idx;
} ent;

static void setIdx(void *entry, size_t idx) {
    ((ent *)entry)->idx = idx;
}

static size_t getIdx(void *entry) {
    return ((ent *)entry)->idx;
}

int main(void) {
    pq *h = pqCreate(getIdx, setIdx);
    ent a, b, c;
    const pqItem *top;

    a.idx = b.idx = c.idx = (size_t)-1;
    pqUpsert(h, &a, 5);
    pqUpsert(h, &b, 1);
    pqUpsert(h, &c, 3);
    top = pqPeek(h);
    EXPECT(top->expire_at_ms == 1);
    EXPECT(top->entry == &b);
    pqUpsert(h, &a, 0);
    top = pqPeek(h);
    EXPECT(top->expire_at_ms == 0);
    EXPECT(top->entry == &a);
    pqRemove(h, &a);
    EXPECT(a.idx == (size_t)-1);
    top = pqPeek(h);
    EXPECT(top->expire_at_ms == 1);
    pqRelease(h);
    EXPECT(b.idx == (size_t)-1);
    EXPECT(c.idx == (size_t)-1);
    return test_finish();
}
