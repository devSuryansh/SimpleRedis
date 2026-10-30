#include "server.h"
#include "util.h"
#include "zmalloc.h"

#include <string.h>
#include <strings.h>

static unsigned int zmemHash(dictNode *node) {
    zmember *m = (zmember *)node;
    sds s = zslNodeMember(m->sl);

    return keyHash(s, sdslen(s));
}

static int zmemCmp(dictNode *node, const void *key, size_t keylen) {
    zmember *m = (zmember *)node;
    sds s = zslNodeMember(m->sl);

    if (sdslen(s) != keylen)
        return 0;
    return memcmp(s, key, keylen) == 0;
}

static void zmemFree(dictNode *node) {
    zfree(node);
}

void freeZsetObject(robj *o) {
    zset *z = o->ptr;

    dictRelease(z->dict);
    zslFree(z->zsl);
    zfree(z);
}

zset *createZset(void) {
    zset *z = zmalloc(sizeof(*z));

    z->dict = dictCreate(zmemCmp, zmemHash, zmemFree);
    z->zsl = zslCreate();
    return z;
}

void zsetInsertOwned(zset *z, double score, sds member) {
    zmember *m = zmalloc(sizeof(*m));

    m->sl = zslInsert(z->zsl, score, member);
    dictAdd(z->dict, &m->node);
}

static zset *lookupZset(client *c, robj *key, int *missing) {
    kvEntry *e = lookupKeyWrite(currentDb(c), key);

    *missing = e == NULL;
    if (!e)
        return NULL;
    if (checkType(c, e, OBJ_ZSET))
        return NULL;
    return e->value->ptr;
}

static zmember *zfind(zset *z, sds member) {
    return (zmember *)dictFind(z->dict, member, sdslen(member), keyHash(member, sdslen(member)));
}

static int zaddSyntax(client *c) {
    int i;

    if ((c->argc % 2) != 0) {
        addReplyError(c, "ERR syntax error");
        return -1;
    }
    for (i = 2; i < c->argc; i += 2) {
        double score;

        if (!getDoubleFromObject(c->argv[i], &score)) {
            addReplyError(c, "ERR value is not a valid float");
            return -1;
        }
    }
    return 0;
}

void zaddCommand(client *c) {
    int missing = 0;
    zset *z = lookupZset(c, c->argv[1], &missing);
    int i;
    int added = 0;

    if (!z && !missing)
        return;
    if (!z) {
        robj *o = createObject(OBJ_ZSET, createZset());
        dbSet(currentDb(c), c->argv[1], o);
        z = o->ptr;
    }
    for (i = 2; i < c->argc; i += 2) {
        double score;
        sds member = objectSds(c->argv[i + 1]);
        zmember *m;

        getDoubleFromObject(c->argv[i], &score);
        m = zfind(z, member);
        if (m) {
            double old = zslNodeScore(m->sl);

            zslDelete(z->zsl, old, member, NULL);
            m->sl = zslInsert(z->zsl, score, sdsdup(member));
        } else {
            m = zmalloc(sizeof(*m));
            m->sl = zslInsert(z->zsl, score, sdsdup(member));
            dictAdd(z->dict, &m->node);
            added++;
        }
    }
    dbTouch(currentDb(c), c->argv[1]);
    addReplyLongLong(c, added);
}

void zremCommand(client *c) {
    int missing = 0;
    zset *z = lookupZset(c, c->argv[1], &missing);
    int i;
    int n = 0;

    if (!z) {
        if (missing)
            addReplyLongLong(c, 0);
        return;
    }
    for (i = 2; i < c->argc; i++) {
        sds member = objectSds(c->argv[i]);
        zmember *m = zfind(z, member);

        if (!m)
            continue;
        zslDelete(z->zsl, zslNodeScore(m->sl), member, NULL);
        dictDelete(z->dict, member, sdslen(member), keyHash(member, sdslen(member)));
        zfree(m);
        n++;
    }
    if (n)
        dbTouch(currentDb(c), c->argv[1]);
    if (zslLength(z->zsl) == 0)
        dbDelete(currentDb(c), c->argv[1]);
    addReplyLongLong(c, n);
}

void zscoreCommand(client *c) {
    int missing = 0;
    zset *z = lookupZset(c, c->argv[1], &missing);
    zmember *m;
    char buf[64];
    int n;

    if (!z) {
        if (missing)
            addReplyNull(c);
        return;
    }
    m = zfind(z, objectSds(c->argv[2]));
    if (!m) {
        addReplyNull(c);
        return;
    }
    n = d2string(buf, sizeof(buf), zslNodeScore(m->sl));
    addReplyBulk(c, buf, (size_t)n);
}

void zcardCommand(client *c) {
    int missing = 0;
    zset *z = lookupZset(c, c->argv[1], &missing);

    if (!z) {
        if (missing)
            addReplyLongLong(c, 0);
        return;
    }
    addReplyLongLong(c, (long long)zslLength(z->zsl));
}

static int rankSyntax(client *c) {
    long long a, b;

    if (c->argc == 3)
        return 0;
    if (!getLongLongFromObject(c->argv[2], &a) || !getLongLongFromObject(c->argv[3], &b)) {
        addReplyError(c, "ERR value is not an integer or out of range");
        return -1;
    }
    return 0;
}

void zrangeCommand(client *c) {
    int missing = 0;
    zset *z = lookupZset(c, c->argv[1], &missing);
    long long start, stop, len, i;
    int withscores = 0;

    if (!getLongLongFromObject(c->argv[2], &start) || !getLongLongFromObject(c->argv[3], &stop)) {
        addReplyError(c, "ERR value is not an integer or out of range");
        return;
    }
    if (c->argc == 5) {
        sds opt = objectSds(c->argv[4]);
        if (sdslen(opt) == 10 && strncasecmp(opt, "withscores", 10) == 0)
            withscores = 1;
        else {
            addReplyError(c, "ERR syntax error");
            return;
        }
    }
    if (!z) {
        if (missing)
            addReplyArrayLen(c, 0);
        return;
    }
    len = (long long)zslLength(z->zsl);
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
    addReplyArrayLen(c, (stop - start + 1) * (withscores ? 2 : 1));
    for (i = start; i <= stop; i++) {
        zskiplistNode *node = zslGetElementByRank(z->zsl, (unsigned long)i + 1);
        char buf[64];
        int n;

        addReplyBulkSds(c, zslNodeMember(node));
        if (withscores) {
            n = d2string(buf, sizeof(buf), zslNodeScore(node));
            addReplyBulk(c, buf, (size_t)n);
        }
    }
}

void zrankCommand(client *c) {
    int missing = 0;
    zset *z = lookupZset(c, c->argv[1], &missing);
    unsigned long rank;

    if (!z) {
        if (missing)
            addReplyNull(c);
        return;
    }
    rank = zslGetRank(z->zsl, zfind(z, objectSds(c->argv[2]))
                                  ? zslNodeScore(zfind(z, objectSds(c->argv[2]))->sl)
                                  : 0,
                      objectSds(c->argv[2]));
    if (!zfind(z, objectSds(c->argv[2])) || rank == 0)
        addReplyNull(c);
    else
        addReplyLongLong(c, (long long)rank - 1);
}

void initZsetCommands(void) {
    registerCommand("zadd", zaddCommand, zaddSyntax, -4, CMD_WRITE | CMD_DENYOOM, 1, 1, 1);
    registerCommand("zrem", zremCommand, NULL, -3, CMD_WRITE, 1, 1, 1);
    registerCommand("zscore", zscoreCommand, NULL, 3, CMD_READONLY, 1, 1, 1);
    registerCommand("zcard", zcardCommand, NULL, 2, CMD_READONLY, 1, 1, 1);
    registerCommand("zrange", zrangeCommand, rankSyntax, -4, CMD_READONLY, 1, 1, 1);
    registerCommand("zrank", zrankCommand, NULL, 3, CMD_READONLY, 1, 1, 1);
}
