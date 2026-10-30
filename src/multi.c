#include "server.h"
#include "zmalloc.h"

#include <string.h>

void freeMultiCommand(void *ptr) {
    multiCmd *mc = ptr;
    int i;

    for (i = 0; i < mc->argc; i++)
        decrRefCount(mc->argv[i]);
    zfree(mc->argv);
    zfree(mc);
}

static watchKey *findWatch(redisDb *db, sds key) {
    return (watchKey *)dictFind(db->watched_keys, key, sdslen(key), keyHash(key, sdslen(key)));
}

static void watchKeyAdd(client *c, robj *key) {
    redisDb *db = currentDb(c);
    sds k = objectSds(key);
    watchKey *w = findWatch(db, k);
    listIter *it;
    listNode *ln;
    int present = 0;

    if (!w) {
        w = zmalloc(sizeof(*w));
        w->key = sdsdup(k);
        w->clients = listCreate();
        dictAdd(db->watched_keys, &w->node);
    }
    it = listGetIterator(w->clients, AL_START_HEAD);
    while ((ln = listNext(it)) != NULL) {
        if (listNodeValue(ln) == c)
            present = 1;
    }
    listReleaseIterator(it);
    if (!present)
        listAddNodeTail(w->clients, c);
    it = listGetIterator(c->watched_keys, AL_START_HEAD);
    present = 0;
    while ((ln = listNext(it)) != NULL) {
        sds existing = listNodeValue(ln);
        if (sdslen(existing) == sdslen(k) && memcmp(existing, k, sdslen(k)) == 0)
            present = 1;
    }
    listReleaseIterator(it);
    if (!present)
        listAddNodeTail(c->watched_keys, sdsdup(k));
}

void unwatchAll(client *c) {
    listIter *it;
    listNode *ln;
    redisDb *db;

    if (!c->watched_keys || !server.db)
        return;
    db = currentDb(c);
    it = listGetIterator(c->watched_keys, AL_START_HEAD);
    while ((ln = listNext(it)) != NULL) {
        sds key = listNodeValue(ln);
        watchKey *w = findWatch(db, key);
        listIter *cit;
        listNode *cln;

        if (!w)
            continue;
        cit = listGetIterator(w->clients, AL_START_HEAD);
        while ((cln = listNext(cit)) != NULL) {
            if (listNodeValue(cln) == c) {
                listDelNode(w->clients, cln);
                break;
            }
        }
        listReleaseIterator(cit);
        if (listLength(w->clients) == 0) {
            dictDelete(db->watched_keys, key, sdslen(key), keyHash(key, sdslen(key)));
            sdsfree(w->key);
            listRelease(w->clients);
            zfree(w);
        }
    }
    listReleaseIterator(it);
    listRelease(c->watched_keys);
    c->watched_keys = listCreate();
    listSetFreeMethod(c->watched_keys, (void (*)(void *))sdsfree);
    c->flags &= ~CLIENT_DIRTY_CAS;
}

static void discardQueue(client *c) {
    listRelease(c->multi_queue);
    c->multi_queue = listCreate();
    listSetFreeMethod(c->multi_queue, freeMultiCommand);
    c->flags &= ~(CLIENT_MULTI | CLIENT_DIRTY_EXEC | CLIENT_DIRTY_CAS);
    unwatchAll(c);
}

void queueMultiCommand(client *c, redisCommand *cmd) {
    multiCmd *mc = zmalloc(sizeof(*mc));
    int i;

    mc->cmd = cmd;
    mc->argc = c->argc;
    mc->argv = zmalloc((size_t)c->argc * sizeof(robj *));
    for (i = 0; i < c->argc; i++)
        mc->argv[i] = dupStringObject(c->argv[i]);
    listAddNodeTail(c->multi_queue, mc);
}

void multiCommand(client *c) {
    if (c->flags & CLIENT_MULTI) {
        addReplyError(c, "ERR MULTI calls can not be nested");
        return;
    }
    c->flags |= CLIENT_MULTI;
    addReplySimple(c, "OK");
}

void discardCommand(client *c) {
    if (!(c->flags & CLIENT_MULTI)) {
        addReplyError(c, "ERR DISCARD without MULTI");
        return;
    }
    discardQueue(c);
    addReplySimple(c, "OK");
}

void execTransaction(client *c) {
    list *queue;
    listIter *it;
    listNode *ln;
    int dirty_exec = c->flags & CLIENT_DIRTY_EXEC;
    int dirty_cas = c->flags & CLIENT_DIRTY_CAS;

    if (!(c->flags & CLIENT_MULTI)) {
        addReplyError(c, "ERR EXEC without MULTI");
        return;
    }
    queue = c->multi_queue;
    c->multi_queue = listCreate();
    listSetFreeMethod(c->multi_queue, freeMultiCommand);
    c->flags &= ~(CLIENT_MULTI | CLIENT_DIRTY_EXEC | CLIENT_DIRTY_CAS);
    if (dirty_exec) {
        listRelease(queue);
        unwatchAll(c);
        addReplyError(c, "EXECABORT Transaction discarded because of previous errors.");
        return;
    }
    if (dirty_cas) {
        listRelease(queue);
        unwatchAll(c);
        addReplyNullArray(c);
        return;
    }
    addReplyArrayLen(c, (long long)listLength(queue));
    it = listGetIterator(queue, AL_START_HEAD);
    while ((ln = listNext(it)) != NULL) {
        multiCmd *mc = listNodeValue(ln);
        robj **saved_argv = c->argv;
        int saved_argc = c->argc;

        c->argv = mc->argv;
        c->argc = mc->argc;
        callCommand(c, mc->cmd);
        c->argv = saved_argv;
        c->argc = saved_argc;
    }
    listReleaseIterator(it);
    listRelease(queue);
    unwatchAll(c);
}

void watchCommand(client *c) {
    int i;

    if (c->flags & CLIENT_MULTI) {
        addReplyError(c, "ERR WATCH inside MULTI is not allowed");
        c->flags |= CLIENT_DIRTY_EXEC;
        return;
    }
    for (i = 1; i < c->argc; i++)
        watchKeyAdd(c, c->argv[i]);
    addReplySimple(c, "OK");
}

void unwatchCommand(client *c) {
    unwatchAll(c);
    addReplySimple(c, "OK");
}

void initMultiCommands(void) {
    registerCommand("multi", multiCommand, NULL, 1, CMD_READONLY, 0, 0, 0);
    registerCommand("exec", execTransaction, NULL, 1, CMD_READONLY, 0, 0, 0);
    registerCommand("discard", discardCommand, NULL, 1, CMD_READONLY, 0, 0, 0);
    registerCommand("watch", watchCommand, NULL, -2, CMD_READONLY, 1, -1, 1);
    registerCommand("unwatch", unwatchCommand, NULL, 1, CMD_READONLY, 0, 0, 0);
}
