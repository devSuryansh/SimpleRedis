#include "dict.h"
#include "zmalloc.h"

#include <stdlib.h>

struct dict {
    dictNode **ht[2];
    unsigned long size[2];
    unsigned long used[2];
    long rehashidx;
    dictKeyCompare cmp;
    dictNodeHash hashFn;
    dictNodeFree freeFn;
};

struct dictIterator {
    dict *d;
    int table;
    unsigned long index;
    dictNode *entry;
};

dict *dictCreate(dictKeyCompare cmp, dictNodeHash hashFn, dictNodeFree freeFn) {
    dict *d = zmalloc(sizeof(*d));

    d->ht[0] = calloc(4, sizeof(dictNode *));
    d->size[0] = 4;
    d->used[0] = 0;
    d->ht[1] = NULL;
    d->size[1] = 0;
    d->used[1] = 0;
    d->rehashidx = -1;
    d->cmp = cmp;
    d->hashFn = hashFn;
    d->freeFn = freeFn;
    return d;
}

static int dictExpand(dict *d, unsigned long size) {
    if (d->rehashidx != -1)
        return -1;
    d->ht[1] = calloc(size, sizeof(dictNode *));
    if (!d->ht[1])
        return -1;
    d->size[1] = size;
    d->used[1] = 0;
    d->rehashidx = 0;
    return 0;
}

void dictRehash(dict *d, int n) {
    if (d->rehashidx == -1)
        return;
    while (n > 0 && d->used[0] > 0) {
        dictNode *de;
        dictNode *next;

        while (d->rehashidx < (long)d->size[0] &&
               d->ht[0][d->rehashidx] == NULL)
            d->rehashidx++;
        if (d->rehashidx >= (long)d->size[0])
            break;
        de = d->ht[0][d->rehashidx];
        d->ht[0][d->rehashidx] = NULL;
        while (de) {
            unsigned int h = d->hashFn(de) % (unsigned int)d->size[1];

            next = de->next;
            de->next = d->ht[1][h];
            d->ht[1][h] = de;
            d->used[0]--;
            d->used[1]++;
            de = next;
        }
        d->rehashidx++;
        n--;
    }
    if (d->used[0] == 0) {
        free(d->ht[0]);
        d->ht[0] = d->ht[1];
        d->size[0] = d->size[1];
        d->used[0] = d->used[1];
        d->ht[1] = NULL;
        d->size[1] = 0;
        d->used[1] = 0;
        d->rehashidx = -1;
    }
}

void dictAdd(dict *d, dictNode *node) {
    unsigned int h;
    int table;

    dictRehash(d, 1);
    if (d->rehashidx == -1 && d->used[0] >= d->size[0])
        dictExpand(d, d->size[0] * 2);
    h = d->hashFn(node);
    table = d->rehashidx == -1 ? 0 : 1;
    h %= (unsigned int)d->size[table];
    node->next = d->ht[table][h];
    d->ht[table][h] = node;
    d->used[table]++;
}

static dictNode *dictFindRaw(dict *d, const void *key, size_t keylen,
                              unsigned int hash, int unlink) {
    int table;

    for (table = 0; table <= 1; table++) {
        dictNode *de;
        dictNode *prev = NULL;
        unsigned int slot;

        if (!d->ht[table] || (table == 1 && d->rehashidx == -1))
            continue;
        slot = hash % (unsigned int)d->size[table];
        de = d->ht[table][slot];
        while (de) {
            if (d->cmp(de, key, keylen)) {
                if (unlink) {
                    if (prev)
                        prev->next = de->next;
                    else
                        d->ht[table][slot] = de->next;
                    d->used[table]--;
                    de->next = NULL;
                }
                return de;
            }
            prev = de;
            de = de->next;
        }
        if (d->rehashidx == -1)
            break;
    }
    return NULL;
}

dictNode *dictFind(dict *d, const void *key, size_t keylen, unsigned int hash) {
    dictRehash(d, 1);
    return dictFindRaw(d, key, keylen, hash, 0);
}

dictNode *dictDelete(dict *d, const void *key, size_t keylen, unsigned int hash) {
    dictRehash(d, 1);
    return dictFindRaw(d, key, keylen, hash, 1);
}

unsigned long dictSize(const dict *d) {
    return d->used[0] + d->used[1];
}

void dictRelease(dict *d) {
    int table;

    for (table = 0; table <= 1; table++) {
        unsigned long i;

        if (!d->ht[table])
            continue;
        for (i = 0; i < d->size[table]; i++) {
            dictNode *de = d->ht[table][i];

            while (de) {
                dictNode *next = de->next;

                if (d->freeFn)
                    d->freeFn(de);
                de = next;
            }
        }
        free(d->ht[table]);
    }
    zfree(d);
}

dictIterator *dictGetIterator(dict *d) {
    dictIterator *it = zmalloc(sizeof(*it));

    it->d = d;
    it->table = 0;
    it->index = 0;
    it->entry = NULL;
    return it;
}

dictNode *dictNext(dictIterator *it) {
    dict *d = it->d;

    for (;;) {
        if (it->entry) {
            dictNode *cur = it->entry;

            it->entry = cur->next;
            return cur;
        }
        if (it->table > 1)
            return NULL;
        if (!d->ht[it->table] || (it->table == 1 && d->rehashidx == -1)) {
            it->table++;
            it->index = 0;
            continue;
        }
        if (it->index >= d->size[it->table]) {
            it->table++;
            it->index = 0;
            continue;
        }
        it->entry = d->ht[it->table][it->index++];
    }
}

void dictReleaseIterator(dictIterator *it) {
    zfree(it);
}

int dictIsRehashing(const dict *d) {
    return d->rehashidx != -1;
}

unsigned long dictSlotCount(const dict *d, int table) {
    if (table < 0 || table > 1)
        return 0;
    return d->size[table];
}

dictNode *dictSlotHead(const dict *d, int table, unsigned long slot) {
    if (table < 0 || table > 1 || !d->ht[table] || slot >= d->size[table])
        return NULL;
    return d->ht[table][slot];
}
