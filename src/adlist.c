#include "adlist.h"
#include "zmalloc.h"

list *listCreate(void) {
    list *l = zmalloc(sizeof(*l));

    l->head = NULL;
    l->tail = NULL;
    l->len = 0;
    l->free = NULL;
    return l;
}

void listRelease(list *l) {
    listNode *cur = l->head;

    while (cur) {
        listNode *next = cur->next;

        if (l->free)
            l->free(cur->value);
        zfree(cur);
        cur = next;
    }
    zfree(l);
}

static listNode *listNewNode(void *value) {
    listNode *n = zmalloc(sizeof(*n));

    n->prev = NULL;
    n->next = NULL;
    n->value = value;
    return n;
}

list *listAddNodeHead(list *l, void *value) {
    listNode *n = listNewNode(value);

    if (l->len == 0) {
        l->head = l->tail = n;
    } else {
        n->next = l->head;
        l->head->prev = n;
        l->head = n;
    }
    l->len++;
    return l;
}

list *listAddNodeTail(list *l, void *value) {
    listNode *n = listNewNode(value);

    if (l->len == 0) {
        l->head = l->tail = n;
    } else {
        n->prev = l->tail;
        l->tail->next = n;
        l->tail = n;
    }
    l->len++;
    return l;
}

void listDelNode(list *l, listNode *node) {
    if (node->prev)
        node->prev->next = node->next;
    else
        l->head = node->next;
    if (node->next)
        node->next->prev = node->prev;
    else
        l->tail = node->prev;
    if (l->free)
        l->free(node->value);
    zfree(node);
    l->len--;
}

unsigned long listLength(const list *l) {
    return l->len;
}

listIter *listGetIterator(list *l, int direction) {
    listIter *it = zmalloc(sizeof(*it));

    it->direction = direction;
    it->next = direction == AL_START_HEAD ? l->head : l->tail;
    return it;
}

listNode *listNext(listIter *iter) {
    listNode *cur = iter->next;

    if (cur) {
        iter->next = iter->direction == AL_START_HEAD ? cur->next : cur->prev;
    }
    return cur;
}

void listReleaseIterator(listIter *iter) {
    zfree(iter);
}

void listSetFreeMethod(list *l, void (*freeFn)(void *)) {
    l->free = freeFn;
}

listNode *listFirst(const list *l) {
    return l->head;
}
