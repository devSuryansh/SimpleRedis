#include "test.h"
#include "dict.h"

#include <stdlib.h>

typedef struct item {
    dictNode node;
    int key;
} item;

static int itemCmp(dictNode *node, const void *key, size_t keylen) {
    const item *it = (const item *)node;
    (void)keylen;
    return it->key == *(const int *)key;
}

static unsigned int itemHash(dictNode *node) {
    return (unsigned int)((const item *)node)->key * 2654435761u;
}

static unsigned int keyHash(int key) {
    return (unsigned int)key * 2654435761u;
}

static int freed;

static void itemFree(dictNode *node) {
    freed++;
    free(node);
}

int main(void) {
    dict *d = dictCreate(itemCmp, itemHash, itemFree);
    int i;
    int seen[100];
    dictIterator *it;
    dictNode *n;
    int count;

    for (i = 0; i < 100; i++) {
        item *itx = malloc(sizeof(*itx));
        itx->key = i;
        dictAdd(d, &itx->node);
    }
    EXPECT(dictSize(d) == 100);
    for (i = 0; i < 100; i++) {
        n = dictFind(d, &i, sizeof(i), keyHash(i));
        EXPECT(n != NULL);
        EXPECT(((item *)n)->key == i);
    }
    for (i = 0; i < 100; i += 2) {
        n = dictDelete(d, &i, sizeof(i), keyHash(i));
        EXPECT(n != NULL);
        free(n);
    }
    EXPECT(dictSize(d) == 50);
    for (i = 1; i < 100; i += 2) {
        n = dictFind(d, &i, sizeof(i), keyHash(i));
        EXPECT(n != NULL);
    }
    i = 0;
    n = dictFind(d, &i, sizeof(i), keyHash(i));
    EXPECT(n == NULL);

    for (i = 0; i < 100; i++)
        seen[i] = 0;
    it = dictGetIterator(d);
    count = 0;
    while ((n = dictNext(it)) != NULL) {
        int k = ((item *)n)->key;
        EXPECT(k % 2 == 1);
        EXPECT(seen[k] == 0);
        seen[k] = 1;
        count++;
    }
    dictReleaseIterator(it);
    EXPECT(count == 50);
    dictRelease(d);
    EXPECT(freed == 50);
    return test_finish();
}
