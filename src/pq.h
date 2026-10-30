#ifndef PQ_H
#define PQ_H

#include <stddef.h>

typedef struct pqItem {
    long long expire_at_ms;
    void *entry;
} pqItem;

typedef struct pq pq;
typedef size_t (*pqGetIndex)(void *entry);
typedef void (*pqSetIndex)(void *entry, size_t idx);

pq *pqCreate(pqGetIndex getIndex, pqSetIndex setIndex);
void pqRelease(pq *h);
void pqUpsert(pq *h, void *entry, long long expire_at_ms);
void pqRemove(pq *h, void *entry);
const pqItem *pqPeek(const pq *h);
size_t pqLen(const pq *h);

#endif
