#include "server.h"
#include "zmalloc.h"

#include <string.h>

static unsigned int fieldHash(dictNode *node) {
    hashField *f = (hashField *)node;
    return keyHash(f->field, sdslen(f->field));
}

static int fieldCmp(dictNode *node, const void *key, size_t keylen) {
    hashField *f = (hashField *)node;

    if (sdslen(f->field) != keylen)
        return 0;
    return memcmp(f->field, key, keylen) == 0;
}

static void fieldFree(dictNode *node) {
    hashField *f = (hashField *)node;

    sdsfree(f->field);
    sdsfree(f->value);
    zfree(f);
}

void freeHashObject(robj *o) {
    dictRelease(o->ptr);
}

dict *createHashDict(void) {
    return dictCreate(fieldCmp, fieldHash, fieldFree);
}

static dict *lookupHash(client *c, robj *key, int *missing) {
    kvEntry *e = lookupKeyWrite(currentDb(c), key);

    *missing = e == NULL;
    if (!e)
        return NULL;
    if (checkType(c, e, OBJ_HASH))
        return NULL;
    return e->value->ptr;
}

static hashField *hashFind(dict *h, sds field) {
    return (hashField *)dictFind(h, field, sdslen(field), keyHash(field, sdslen(field)));
}

static int hashSyntax(client *c) {
    if ((c->argc % 2) != 0) {
        addReplyError(c, "ERR wrong number of arguments for 'hset' command");
        return -1;
    }
    return 0;
}

void hsetCommand(client *c) {
    int missing = 0;
    dict *h = lookupHash(c, c->argv[1], &missing);
    int i;
    int added = 0;

    if (!h && !missing)
        return;
    if (!h) {
        robj *o = createObject(OBJ_HASH, createHashDict());
        dbSet(currentDb(c), c->argv[1], o);
        h = o->ptr;
    }
    for (i = 2; i < c->argc; i += 2) {
        sds field = objectSds(c->argv[i]);
        sds value = objectSds(c->argv[i + 1]);
        hashField *f = hashFind(h, field);

        if (f) {
            sdsfree(f->value);
            f->value = sdsdup(value);
        } else {
            f = zmalloc(sizeof(*f));
            f->field = sdsdup(field);
            f->value = sdsdup(value);
            dictAdd(h, &f->node);
            added++;
        }
    }
    dbTouch(currentDb(c), c->argv[1]);
    addReplyLongLong(c, added);
}

void hgetCommand(client *c) {
    int missing = 0;
    dict *h = lookupHash(c, c->argv[1], &missing);
    hashField *f;

    if (!h) {
        if (missing)
            addReplyNull(c);
        return;
    }
    f = hashFind(h, objectSds(c->argv[2]));
    if (!f)
        addReplyNull(c);
    else
        addReplyBulkSds(c, f->value);
}

void hdelCommand(client *c) {
    int missing = 0;
    dict *h = lookupHash(c, c->argv[1], &missing);
    int i;
    int n = 0;

    if (!h) {
        if (missing)
            addReplyLongLong(c, 0);
        return;
    }
    for (i = 2; i < c->argc; i++) {
        sds field = objectSds(c->argv[i]);
        hashField *f = (hashField *)dictDelete(h, field, sdslen(field),
                                                keyHash(field, sdslen(field)));

        if (f) {
            fieldFree(&f->node);
            n++;
        }
    }
    if (n)
        dbTouch(currentDb(c), c->argv[1]);
    if (dictSize(h) == 0)
        dbDelete(currentDb(c), c->argv[1]);
    addReplyLongLong(c, n);
}

void hexistsCommand(client *c) {
    int missing = 0;
    dict *h = lookupHash(c, c->argv[1], &missing);

    if (!h) {
        if (missing)
            addReplyLongLong(c, 0);
        return;
    }
    addReplyLongLong(c, hashFind(h, objectSds(c->argv[2])) ? 1 : 0);
}

void hlenCommand(client *c) {
    int missing = 0;
    dict *h = lookupHash(c, c->argv[1], &missing);

    if (!h) {
        if (missing)
            addReplyLongLong(c, 0);
        return;
    }
    addReplyLongLong(c, (long long)dictSize(h));
}

static void hashIterateReply(client *c, dict *h, int which) {
    dictIterator *it = dictGetIterator(h);
    dictNode *n;

    if (which == 0) {
        if (c->proto >= 3)
            addReplyMapLen(c, (long long)dictSize(h));
        else
            addReplyArrayLen(c, (long long)dictSize(h) * 2);
    } else {
        addReplyArrayLen(c, (long long)dictSize(h));
    }
    while ((n = dictNext(it)) != NULL) {
        hashField *f = (hashField *)n;

        if (which != 2)
            addReplyBulkSds(c, f->field);
        if (which != 1)
            addReplyBulkSds(c, f->value);
    }
    dictReleaseIterator(it);
}

void hgetallCommand(client *c) {
    int missing = 0;
    dict *h = lookupHash(c, c->argv[1], &missing);

    if (!h) {
        if (missing) {
            if (c->proto >= 3)
                addReplyMapLen(c, 0);
            else
                addReplyArrayLen(c, 0);
        }
        return;
    }
    hashIterateReply(c, h, 0);
}

void hkeysCommand(client *c) {
    int missing = 0;
    dict *h = lookupHash(c, c->argv[1], &missing);

    if (!h) {
        if (missing)
            addReplyArrayLen(c, 0);
        return;
    }
    hashIterateReply(c, h, 1);
}

void hvalsCommand(client *c) {
    int missing = 0;
    dict *h = lookupHash(c, c->argv[1], &missing);

    if (!h) {
        if (missing)
            addReplyArrayLen(c, 0);
        return;
    }
    hashIterateReply(c, h, 2);
}

void initHashCommands(void) {
    registerCommand("hset", hsetCommand, hashSyntax, -4, CMD_WRITE | CMD_DENYOOM, 1, 1, 1);
    registerCommand("hget", hgetCommand, NULL, 3, CMD_READONLY, 1, 1, 1);
    registerCommand("hdel", hdelCommand, NULL, -3, CMD_WRITE, 1, 1, 1);
    registerCommand("hexists", hexistsCommand, NULL, 3, CMD_READONLY, 1, 1, 1);
    registerCommand("hlen", hlenCommand, NULL, 2, CMD_READONLY, 1, 1, 1);
    registerCommand("hgetall", hgetallCommand, NULL, 2, CMD_READONLY, 1, 1, 1);
    registerCommand("hkeys", hkeysCommand, NULL, 2, CMD_READONLY, 1, 1, 1);
    registerCommand("hvals", hvalsCommand, NULL, 2, CMD_READONLY, 1, 1, 1);
}
