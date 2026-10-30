#include "server.h"

#include <string.h>

void freeListObject(robj *o) {
    listRelease(o->ptr);
}

static list *listValue(kvEntry *e) {
    return e->value->ptr;
}

static list *lookupList(client *c, robj *key, int *missing) {
    kvEntry *e = lookupKeyWrite(currentDb(c), key);

    *missing = e == NULL;
    if (!e)
        return NULL;
    if (checkType(c, e, OBJ_LIST))
        return NULL;
    return listValue(e);
}

static robj *createListObject(void) {
    list *l = listCreate();

    listSetFreeMethod(l, (void (*)(void *))sdsfree);
    return createObject(OBJ_LIST, l);
}

static listNode *listIndexNode(list *l, long long idx) {
    long long len = (long long)listLength(l);
    listNode *n;

    if (idx < 0)
        idx = len + idx;
    if (idx < 0 || idx >= len)
        return NULL;
    if (idx <= len / 2) {
        n = listFirst(l);
        while (idx-- > 0)
            n = n->next;
    } else {
        n = listLast(l);
        idx = len - 1 - idx;
        while (idx-- > 0)
            n = n->prev;
    }
    return n;
}

static void pushGeneric(client *c, int head) {
    int missing = 0;
    list *l = lookupList(c, c->argv[1], &missing);
    int i;

    if (!l && !missing)
        return;
    if (!l) {
        robj *o = createListObject();
        dbSet(currentDb(c), c->argv[1], o);
        l = o->ptr;
    }
    for (i = 2; i < c->argc; i++) {
        sds v = sdsdup(objectSds(c->argv[i]));
        if (head)
            listAddNodeHead(l, v);
        else
            listAddNodeTail(l, v);
    }
    addReplyLongLong(c, (long long)listLength(l));
}

void lpushCommand(client *c) {
    pushGeneric(c, 1);
}
void rpushCommand(client *c) {
    pushGeneric(c, 0);
}

static void popGeneric(client *c, int head) {
    int missing = 0;
    list *l = lookupList(c, c->argv[1], &missing);
    listNode *n;
    sds v;

    if (!l) {
        if (missing)
            addReplyNull(c);
        return;
    }
    n = head ? listFirst(l) : listLast(l);
    v = sdsdup(listNodeValue(n));
    listDelNode(l, n);
    addReplyBulk(c, v, sdslen(v));
    sdsfree(v);
    if (listLength(l) == 0)
        dbDelete(currentDb(c), c->argv[1]);
}

void lpopCommand(client *c) {
    popGeneric(c, 1);
}
void rpopCommand(client *c) {
    popGeneric(c, 0);
}

void llenCommand(client *c) {
    int missing = 0;
    list *l = lookupList(c, c->argv[1], &missing);

    if (!l) {
        if (missing)
            addReplyLongLong(c, 0);
        return;
    }
    addReplyLongLong(c, (long long)listLength(l));
}

void lindexCommand(client *c) {
    int missing = 0;
    list *l = lookupList(c, c->argv[1], &missing);
    long long idx;
    listNode *n;

    if (!getLongLongFromObject(c->argv[2], &idx)) {
        addReplyError(c, "ERR value is not an integer or out of range");
        return;
    }
    if (!l) {
        if (missing)
            addReplyNull(c);
        return;
    }
    n = listIndexNode(l, idx);
    if (!n)
        addReplyNull(c);
    else
        addReplyBulkSds(c, listNodeValue(n));
}

void lrangeCommand(client *c) {
    int missing = 0;
    list *l = lookupList(c, c->argv[1], &missing);
    long long start, stop, len, i;
    listNode *n;

    if (!getLongLongFromObject(c->argv[2], &start) || !getLongLongFromObject(c->argv[3], &stop)) {
        addReplyError(c, "ERR value is not an integer or out of range");
        return;
    }
    if (!l) {
        if (missing)
            addReplyArrayLen(c, 0);
        return;
    }
    len = (long long)listLength(l);
    if (start < 0)
        start = len + start;
    if (stop < 0)
        stop = len + stop;
    if (start < 0)
        start = 0;
    if (stop >= len)
        stop = len - 1;
    if (start > stop || start >= len) {
        addReplyArrayLen(c, 0);
        return;
    }
    addReplyArrayLen(c, stop - start + 1);
    n = listIndexNode(l, start);
    for (i = start; i <= stop && n; i++) {
        addReplyBulkSds(c, listNodeValue(n));
        n = n->next;
    }
}

static int indexSyntax(client *c) {
    long long n;

    if (!getLongLongFromObject(c->argv[2], &n)) {
        addReplyError(c, "ERR value is not an integer or out of range");
        return -1;
    }
    return 0;
}

static int rangeSyntax(client *c) {
    long long a, b;

    if (!getLongLongFromObject(c->argv[2], &a) || !getLongLongFromObject(c->argv[3], &b)) {
        addReplyError(c, "ERR value is not an integer or out of range");
        return -1;
    }
    return 0;
}

void initListCommands(void) {
    registerCommand("lpush", lpushCommand, NULL, -3, CMD_WRITE | CMD_DENYOOM, 1, 1, 1);
    registerCommand("rpush", rpushCommand, NULL, -3, CMD_WRITE | CMD_DENYOOM, 1, 1, 1);
    registerCommand("lpop", lpopCommand, NULL, 2, CMD_WRITE, 1, 1, 1);
    registerCommand("rpop", rpopCommand, NULL, 2, CMD_WRITE, 1, 1, 1);
    registerCommand("llen", llenCommand, NULL, 2, CMD_READONLY, 1, 1, 1);
    registerCommand("lindex", lindexCommand, indexSyntax, 3, CMD_READONLY, 1, 1, 1);
    registerCommand("lrange", lrangeCommand, rangeSyntax, 4, CMD_READONLY, 1, 1, 1);
}
