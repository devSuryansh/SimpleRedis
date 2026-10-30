#include "test.h"
#include "adlist.h"

#include <stdlib.h>

static int freed;

static void freeInt(void *p) {
    freed++;
    free(p);
}

int main(void) {
    list *l = listCreate();
    listIter *it;
    listNode *node;
    int *a = malloc(sizeof(int));
    int *b = malloc(sizeof(int));

    *a = 1;
    *b = 2;
    listSetFreeMethod(l, freeInt);
    listAddNodeHead(l, a);
    listAddNodeTail(l, b);
    EXPECT(listLength(l) == 2);
    it = listGetIterator(l, AL_START_HEAD);
    node = listNext(it);
    EXPECT(*(int *)listNodeValue(node) == 1);
    node = listNext(it);
    EXPECT(*(int *)listNodeValue(node) == 2);
    EXPECT(listNext(it) == NULL);
    listReleaseIterator(it);
    listDelNode(l, listFirst(l));
    EXPECT(listLength(l) == 1);
    EXPECT(freed == 1);
    listRelease(l);
    EXPECT(freed == 2);
    return test_finish();
}
