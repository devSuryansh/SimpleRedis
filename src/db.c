#include "server.h"
#include "util.h"
#include "zmalloc.h"

#include <string.h>

static unsigned int kvHash(dictNode *node) {
    kvEntry *e = (kvEntry *)node;
    return keyHash(e->key, sdslen(e->key));
}

static int kvCmp(dictNode *node, const void *key, size_t keylen) {
    kvEntry *e = (kvEntry *)node;

    if (sdslen(e->key) != keylen)
        return 0;
    return memcmp(e->key, key, keylen) == 0;
}

static void kvFree(dictNode *node) {
    kvEntry *e = (kvEntry *)node;

    sdsfree(e->key);
    decrRefCount(e->value);
    zfree(e);
}

static unsigned int watchHash(dictNode *node) {
    watchKey *w = (watchKey *)node;
    return keyHash(w->key, sdslen(w->key));
}

static int watchCmp(dictNode *node, const void *key, size_t keylen) {
    watchKey *w = (watchKey *)node;

    if (sdslen(w->key) != keylen)
        return 0;
    return memcmp(w->key, key, keylen) == 0;
}

static void watchFree(dictNode *node) {
    watchKey *w = (watchKey *)node;

    sdsfree(w->key);
    listRelease(w->clients);
    zfree(w);
}

static size_t kvGetIndex(void *entry) {
    return ((kvEntry *)entry)->heap_idx;
}

static void kvSetIndex(void *entry, size_t idx) {
    ((kvEntry *)entry)->heap_idx = idx;
}

static dict *newKeyDict(void) {
    return dictCreate(kvCmp, kvHash, kvFree);
}

static dict *newWatchDict(void) {
    return dictCreate(watchCmp, watchHash, watchFree);
}

void initDatabases(void) {
    int i;

    server.db = zmalloc(sizeof(redisDb) * (size_t)server.databases);
    for (i = 0; i < server.databases; i++) {
        server.db[i].id = i;
        server.db[i].dict = newKeyDict();
        server.db[i].watched_keys = newWatchDict();
        server.db[i].expires = pqCreate(kvGetIndex, kvSetIndex);
    }
}

redisDb *currentDb(client *c) {
    return &server.db[c->db_id];
}

static kvEntry *findEntry(redisDb *db, robj *key) {
    sds k = objectSds(key);
    return (kvEntry *)dictFind(db->dict, k, sdslen(k), keyHash(k, sdslen(k)));
}

static void deleteEntry(redisDb *db, kvEntry *e) {
    unsigned int h = keyHash(e->key, sdslen(e->key));

    signalModifiedKey(db, e->key);
    server.dirty++;
    if (e->heap_idx != (size_t)-1)
        pqRemove(db->expires, e);
    dictDelete(db->dict, e->key, sdslen(e->key), h);
    sdsfree(e->key);
    decrRefCount(e->value);
    zfree(e);
}

int expireIfNeeded(redisDb *db, kvEntry *e) {
    if (!e || e->expire_at_ms == -1)
        return 0;
    if (e->expire_at_ms > mstime())
        return 0;
    deleteEntry(db, e);
    return 1;
}

kvEntry *lookupKeyRead(redisDb *db, robj *key) {
    kvEntry *e = findEntry(db, key);

    if (e && expireIfNeeded(db, e))
        return NULL;
    return e;
}

kvEntry *lookupKeyWrite(redisDb *db, robj *key) {
    return lookupKeyRead(db, key);
}

void dbSet(redisDb *db, robj *key, robj *val) {
    sds k = objectSds(key);
    kvEntry *e = findEntry(db, key);

    if (e && expireIfNeeded(db, e))
        e = NULL;
    signalModifiedKey(db, k);
    server.dirty++;
    if (e) {
        decrRefCount(e->value);
        e->value = val;
        return;
    }
    e = zmalloc(sizeof(*e));
    e->key = sdsdup(k);
    e->value = val;
    e->expire_at_ms = -1;
    e->heap_idx = (size_t)-1;
    dictAdd(db->dict, &e->node);
}

int dbDelete(redisDb *db, robj *key) {
    kvEntry *e = findEntry(db, key);

    if (!e)
        return 0;
    deleteEntry(db, e);
    return 1;
}

void dbSetExpire(redisDb *db, kvEntry *e, long long when_ms) {
    e->expire_at_ms = when_ms;
    pqUpsert(db->expires, e, when_ms);
    server.dirty++;
    signalModifiedKey(db, e->key);
}

void dbPersist(redisDb *db, kvEntry *e) {
    if (e->heap_idx != (size_t)-1)
        pqRemove(db->expires, e);
    e->expire_at_ms = -1;
    e->heap_idx = (size_t)-1;
    server.dirty++;
    signalModifiedKey(db, e->key);
}

int checkType(client *c, kvEntry *e, int type) {
    if (!e || e->value->type == (unsigned)type)
        return 0;
    addReplyError(c, "WRONGTYPE Operation against a key holding the wrong kind of value");
    return 1;
}

void signalModifiedKey(redisDb *db, sds key) {
    unsigned int h = keyHash(key, sdslen(key));
    watchKey *w = (watchKey *)dictFind(db->watched_keys, key, sdslen(key), h);
    listIter *it;
    listNode *ln;

    if (!w)
        return;
    it = listGetIterator(w->clients, AL_START_HEAD);
    while ((ln = listNext(it)) != NULL) {
        client *watching = listNodeValue(ln);
        watching->flags |= CLIENT_DIRTY_CAS;
    }
    listReleaseIterator(it);
}

void touchWatchedKey(redisDb *db, sds key) {
    signalModifiedKey(db, key);
}

static int entryExpired(kvEntry *e, long long now) {
    return e->expire_at_ms != -1 && e->expire_at_ms <= now;
}

void emptyDb(redisDb *db) {
    dictIterator *it;
    dictNode *n;
    list *names;
    listIter *li;
    listNode *ln;

    it = dictGetIterator(db->watched_keys);
    while ((n = dictNext(it)) != NULL)
        signalModifiedKey(db, ((watchKey *)n)->key);
    dictReleaseIterator(it);

    names = listCreate();
    listSetFreeMethod(names, (void (*)(void *))sdsfree);
    it = dictGetIterator(db->dict);
    while ((n = dictNext(it)) != NULL)
        listAddNodeTail(names, sdsdup(((kvEntry *)n)->key));
    dictReleaseIterator(it);
    li = listGetIterator(names, AL_START_HEAD);
    while ((ln = listNext(li)) != NULL) {
        sds key = listNodeValue(ln);
        robj *k = createStringObject(key, sdslen(key));
        dbDelete(db, k);
        decrRefCount(k);
    }
    listReleaseIterator(li);
    listRelease(names);
}

void flushAll(void) {
    int i;

    for (i = 0; i < server.databases; i++)
        emptyDb(&server.db[i]);
}

int dbSizeLive(redisDb *db) {
    dictIterator *it = dictGetIterator(db->dict);
    dictNode *n;
    long long now = mstime();
    int count = 0;
    list *dead = listCreate();

    listSetFreeMethod(dead, (void (*)(void *))sdsfree);
    while ((n = dictNext(it)) != NULL) {
        kvEntry *e = (kvEntry *)n;

        if (entryExpired(e, now))
            listAddNodeTail(dead, sdsdup(e->key));
        else
            count++;
    }
    dictReleaseIterator(it);
    {
        listIter *li = listGetIterator(dead, AL_START_HEAD);
        listNode *ln;

        while ((ln = listNext(li)) != NULL) {
            sds key = listNodeValue(ln);
            robj *k = createStringObject(key, sdslen(key));
            dbDelete(db, k);
            decrRefCount(k);
        }
        listReleaseIterator(li);
    }
    listRelease(dead);
    return count;
}

void activeExpireCycle(int max_keys) {
    int i;
    int n = 0;
    long long now;

    if (!server.db)
        return;
    now = mstime();
    for (i = 0; i < server.databases && n < max_keys; i++) {
        redisDb *db = &server.db[i];

        while (n < max_keys) {
            const pqItem *item = pqPeek(db->expires);
            kvEntry *e;

            if (!item || item->expire_at_ms > now)
                break;
            e = item->entry;
            deleteEntry(db, e);
            n++;
        }
    }
}

int overMaxmemory(void) {
    if (server.maxmemory == 0)
        return 0;
    return zmallocUsed() > server.maxmemory;
}

int getLongLongFromObject(robj *o, long long *target) {
    sds s = objectSds(o);
    return string2ll(s, sdslen(s), target);
}

int getDoubleFromObject(robj *o, double *target) {
    sds s = objectSds(o);
    return string2d(s, sdslen(s), target);
}
