#ifndef DICT_H
#define DICT_H

#include <stddef.h>

typedef struct dictNode {
    struct dictNode *next;
} dictNode;

typedef struct dict dict;
typedef struct dictIterator dictIterator;

typedef int (*dictKeyCompare)(dictNode *node, const void *key, size_t keylen);
typedef void (*dictNodeFree)(dictNode *node);
typedef unsigned int (*dictNodeHash)(dictNode *node);

dict *dictCreate(dictKeyCompare cmp, dictNodeHash hashFn, dictNodeFree freeFn);
void dictAdd(dict *d, dictNode *node);
dictNode *dictFind(dict *d, const void *key, size_t keylen, unsigned int hash);
dictNode *dictDelete(dict *d, const void *key, size_t keylen, unsigned int hash);
unsigned long dictSize(const dict *d);
void dictRelease(dict *d);
dictIterator *dictGetIterator(dict *d);
dictNode *dictNext(dictIterator *it);
void dictReleaseIterator(dictIterator *it);
/* Move up to n occupied buckets from the old table into the new one. */
void dictRehash(dict *d, int n);
int dictIsRehashing(const dict *d);
unsigned long dictSlotCount(const dict *d, int table);
dictNode *dictSlotHead(const dict *d, int table, unsigned long slot);

#endif
