#ifndef ADLIST_H
#define ADLIST_H

#define AL_START_HEAD 0
#define AL_START_TAIL 1

typedef struct listNode {
    struct listNode *prev;
    struct listNode *next;
    void *value;
} listNode;

typedef struct listIter {
    listNode *next;
    int direction;
} listIter;

typedef struct list {
    listNode *head;
    listNode *tail;
    unsigned long len;
    void (*free)(void *ptr);
} list;

list *listCreate(void);
void listRelease(list *list);
list *listAddNodeHead(list *list, void *value);
list *listAddNodeTail(list *list, void *value);
void listDelNode(list *list, listNode *node);
unsigned long listLength(const list *list);
listIter *listGetIterator(list *list, int direction);
listNode *listNext(listIter *iter);
void listReleaseIterator(listIter *iter);
void listSetFreeMethod(list *list, void (*free)(void *));
listNode *listFirst(const list *list);
listNode *listLast(const list *list);

static inline void *listNodeValue(const listNode *node) {
    return node->value;
}

#endif
