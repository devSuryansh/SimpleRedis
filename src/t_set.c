#include "server.h"
#include "zmalloc.h"

#include <string.h>

static unsigned int memberHash(dictNode *node) {
    setMember *m = (setMember *)node;
    return keyHash(m->member, sdslen(m->member));
}

static int memberCmp(dictNode *node, const void *key, size_t keylen) {
    setMember *m = (setMember *)node;

    if (sdslen(m->member) != keylen)
        return 0;
    return memcmp(m->member, key, keylen) == 0;
}

static void memberFree(dictNode *node) {
    setMember *m = (setMember *)node;

    sdsfree(m->member);
    zfree(m);
}

void freeSetObject(robj *o) {
    dictRelease(o->ptr);
}

static dict *lookupSet(client *c, robj *key, int *missing) {
    kvEntry *e = lookupKeyWrite(currentDb(c), key);

    *missing = e == NULL;
    if (!e)
        return NULL;
    if (checkType(c, e, OBJ_SET))
        return NULL;
    return e->value->ptr;
}

void saddCommand(client *c) {
    int missing = 0;
    dict *set = lookupSet(c, c->argv[1], &missing);
    int i;
    int added = 0;

    if (!set && !missing)
        return;
    if (!set) {
        robj *o = createObject(OBJ_SET, dictCreate(memberCmp, memberHash, memberFree));
        dbSet(currentDb(c), c->argv[1], o);
        set = o->ptr;
    }
    for (i = 2; i < c->argc; i++) {
        sds member = objectSds(c->argv[i]);
        unsigned int h = keyHash(member, sdslen(member));

        if (dictFind(set, member, sdslen(member), h))
            continue;
        setMember *m = zmalloc(sizeof(*m));
        m->member = sdsdup(member);
        dictAdd(set, &m->node);
        added++;
    }
    if (added)
        signalModifiedKey(currentDb(c), objectSds(c->argv[1]));
    addReplyLongLong(c, added);
}

void sremCommand(client *c) {
    int missing = 0;
    dict *set = lookupSet(c, c->argv[1], &missing);
    int i;
    int n = 0;

    if (!set) {
        if (missing)
            addReplyLongLong(c, 0);
        return;
    }
    for (i = 2; i < c->argc; i++) {
        sds member = objectSds(c->argv[i]);
        setMember *m = (setMember *)dictDelete(set, member, sdslen(member),
                                                keyHash(member, sdslen(member)));

        if (m) {
            memberFree(&m->node);
            n++;
        }
    }
    if (n)
        signalModifiedKey(currentDb(c), objectSds(c->argv[1]));
    if (dictSize(set) == 0)
        dbDelete(currentDb(c), c->argv[1]);
    addReplyLongLong(c, n);
}

void sismemberCommand(client *c) {
    int missing = 0;
    dict *set = lookupSet(c, c->argv[1], &missing);
    sds member;

    if (!set) {
        if (missing)
            addReplyLongLong(c, 0);
        return;
    }
    member = objectSds(c->argv[2]);
    addReplyLongLong(c, dictFind(set, member, sdslen(member), keyHash(member, sdslen(member))) ? 1
                                                                                                : 0);
}

void smembersCommand(client *c) {
    int missing = 0;
    dict *set = lookupSet(c, c->argv[1], &missing);
    dictIterator *it;
    dictNode *n;

    if (!set) {
        if (missing)
            addReplyArrayLen(c, 0);
        return;
    }
    addReplyArrayLen(c, (long long)dictSize(set));
    it = dictGetIterator(set);
    while ((n = dictNext(it)) != NULL)
        addReplyBulkSds(c, ((setMember *)n)->member);
    dictReleaseIterator(it);
}

void scardCommand(client *c) {
    int missing = 0;
    dict *set = lookupSet(c, c->argv[1], &missing);

    if (!set) {
        if (missing)
            addReplyLongLong(c, 0);
        return;
    }
    addReplyLongLong(c, (long long)dictSize(set));
}

void initSetCommands(void) {
    registerCommand("sadd", saddCommand, NULL, -3, CMD_WRITE | CMD_DENYOOM, 1, 1, 1);
    registerCommand("srem", sremCommand, NULL, -3, CMD_WRITE, 1, 1, 1);
    registerCommand("sismember", sismemberCommand, NULL, 3, CMD_READONLY, 1, 1, 1);
    registerCommand("smembers", smembersCommand, NULL, 2, CMD_READONLY, 1, 1, 1);
    registerCommand("scard", scardCommand, NULL, 2, CMD_READONLY, 1, 1, 1);
}
