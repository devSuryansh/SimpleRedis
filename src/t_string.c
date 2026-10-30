#include "server.h"
#include "dict.h"
#include "util.h"
#include "zmalloc.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int eqOpt(robj *o, const char *lit) {
    sds s = objectSds(o);
    size_t n = strlen(lit);
    size_t i;

    if (sdslen(s) != n)
        return 0;
    for (i = 0; i < n; i++) {
        char a = s[i];
        char b = lit[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return 1;
}

static const char *typeName(unsigned type) {
    switch (type) {
    case OBJ_STRING:
        return "string";
    case OBJ_LIST:
        return "list";
    case OBJ_HASH:
        return "hash";
    case OBJ_SET:
        return "set";
    case OBJ_ZSET:
        return "zset";
    default:
        return "none";
    }
}

static int parseSetOpts(client *c, int *nx, int *xx, int *get, long long *expire_ms, int *has_expire) {
    int i = 3;

    *nx = *xx = *get = *has_expire = 0;
    *expire_ms = 0;
    while (i < c->argc) {
        if (eqOpt(c->argv[i], "nx")) {
            if (*xx)
                return -1;
            *nx = 1;
            i++;
        } else if (eqOpt(c->argv[i], "xx")) {
            if (*nx)
                return -1;
            *xx = 1;
            i++;
        } else if (eqOpt(c->argv[i], "get")) {
            *get = 1;
            i++;
        } else if (eqOpt(c->argv[i], "ex") || eqOpt(c->argv[i], "px")) {
            long long ttl;
            int px = eqOpt(c->argv[i], "px");

            if (*has_expire || i + 1 >= c->argc)
                return -1;
            if (!getLongLongFromObject(c->argv[i + 1], &ttl))
                return -1;
            *has_expire = 1;
            *expire_ms = px ? ttl : ttl * 1000;
            i += 2;
        } else {
            return -1;
        }
    }
    return 0;
}

static int setSyntax(client *c) {
    int nx, xx, get, has_expire;
    long long expire_ms;

    if (parseSetOpts(c, &nx, &xx, &get, &expire_ms, &has_expire) != 0) {
        addReplyError(c, "ERR syntax error");
        return -1;
    }
    return 0;
}

static int msetSyntax(client *c) {
    if (c->argc < 3 || (c->argc % 2) == 0) {
        addReplyError(c, "ERR wrong number of arguments for 'mset' command");
        return -1;
    }
    return 0;
}

static void replyGet(client *c, kvEntry *e) {
    if (!e)
        addReplyNull(c);
    else
        addReplyBulkSds(c, objectSds(e->value));
}

static void setKeyExpire(client *c, robj *key, long long ttl_ms) {
    kvEntry *e;
    long long when;

    if (ttl_ms <= 0) {
        dbDelete(currentDb(c), key);
        return;
    }
    e = lookupKeyWrite(currentDb(c), key);
    if (!e)
        return;
    when = mstime() + ttl_ms;
    dbSetExpire(currentDb(c), e, when);
}

void setCommand(client *c) {
    int nx, xx, get, has_expire;
    long long expire_ms;
    redisDb *db = currentDb(c);
    kvEntry *e;
    sds old = NULL;
    int exists;

    parseSetOpts(c, &nx, &xx, &get, &expire_ms, &has_expire);
    e = lookupKeyWrite(db, c->argv[1]);
    exists = e != NULL;
    if ((nx && exists) || (xx && !exists)) {
        addReplyNull(c);
        return;
    }
    if (get && e && e->value->type != OBJ_STRING) {
        addReplyError(c, "WRONGTYPE Operation against a key holding the wrong kind of value");
        return;
    }
    if (get && e)
        old = sdsdup(objectSds(e->value));
    dbSet(db, c->argv[1], createStringObject(objectSds(c->argv[2]), sdslen(objectSds(c->argv[2]))));
    c->flags |= CLIENT_SKIP_AOF;
    aofFeedPlainSet(c->db_id, c->argv[1], c->argv[2]);
    if (has_expire) {
        long long when = mstime() + expire_ms;

        if (expire_ms <= 0)
            when = mstime() - 1;
        setKeyExpire(c, c->argv[1], expire_ms);
        aofFeedPexpireat(c->db_id, c->argv[1], when);
    } else {
        e = lookupKeyWrite(db, c->argv[1]);
        if (e && e->expire_at_ms != -1)
            dbPersist(db, e);
    }
    if (get) {
        if (old)
            addReplyBulk(c, old, sdslen(old));
        else
            addReplyNull(c);
        sdsfree(old);
    } else {
        addReplySimple(c, "OK");
    }
}

void setnxCommand(client *c) {
    kvEntry *e = lookupKeyWrite(currentDb(c), c->argv[1]);

    if (e) {
        addReplyLongLong(c, 0);
        return;
    }
    dbSet(currentDb(c), c->argv[1],
          createStringObject(objectSds(c->argv[2]), sdslen(objectSds(c->argv[2]))));
    addReplyLongLong(c, 1);
}

static int setexSyntax(client *c) {
    long long sec;

    if (!getLongLongFromObject(c->argv[2], &sec) || sec <= 0) {
        addReplyError(c, "ERR invalid expire time");
        return -1;
    }
    return 0;
}

void setexCommand(client *c) {
    long long sec;

    getLongLongFromObject(c->argv[2], &sec);
    dbSet(currentDb(c), c->argv[1],
          createStringObject(objectSds(c->argv[3]), sdslen(objectSds(c->argv[3]))));
    setKeyExpire(c, c->argv[1], sec * 1000);
    c->flags |= CLIENT_SKIP_AOF;
    aofFeedPlainSet(c->db_id, c->argv[1], c->argv[3]);
    aofFeedPexpireat(c->db_id, c->argv[1], mstime() + sec * 1000);
    addReplySimple(c, "OK");
}

void getCommand(client *c) {
    kvEntry *e = lookupKeyRead(currentDb(c), c->argv[1]);

    if (e && checkType(c, e, OBJ_STRING))
        return;
    replyGet(c, e);
}

void mgetCommand(client *c) {
    int i;

    addReplyArrayLen(c, c->argc - 1);
    for (i = 1; i < c->argc; i++) {
        kvEntry *e = lookupKeyRead(currentDb(c), c->argv[i]);

        if (e && e->value->type != OBJ_STRING)
            addReplyNull(c);
        else
            replyGet(c, e);
    }
}

void msetCommand(client *c) {
    int i;

    for (i = 1; i < c->argc; i += 2) {
        kvEntry *e = lookupKeyWrite(currentDb(c), c->argv[i]);

        if (e && checkType(c, e, OBJ_STRING))
            return;
    }
    for (i = 1; i < c->argc; i += 2) {
        kvEntry *e;

        dbSet(currentDb(c), c->argv[i],
              createStringObject(objectSds(c->argv[i + 1]), sdslen(objectSds(c->argv[i + 1]))));
        e = lookupKeyWrite(currentDb(c), c->argv[i]);
        if (e && e->expire_at_ms != -1)
            dbPersist(currentDb(c), e);
    }
    addReplySimple(c, "OK");
}

static int addOverflow(long long cur, long long incr) {
    if (incr > 0)
        return cur > LLONG_MAX - incr;
    if (incr < 0)
        return cur < LLONG_MIN - incr;
    return 0;
}

static void incrGeneric(client *c, long long incr) {
    redisDb *db = currentDb(c);
    kvEntry *e = lookupKeyWrite(db, c->argv[1]);
    long long cur = 0;
    long long neu;
    sds num;

    if (e) {
        if (checkType(c, e, OBJ_STRING))
            return;
        if (!string2ll(objectSds(e->value), sdslen(objectSds(e->value)), &cur)) {
            addReplyError(c, "ERR value is not an integer or out of range");
            return;
        }
    }
    if (addOverflow(cur, incr)) {
        addReplyError(c, "ERR increment or decrement would overflow");
        return;
    }
    neu = cur + incr;
    num = sdsfromlonglong(neu);
    dbSet(db, c->argv[1], createStringObject(num, sdslen(num)));
    sdsfree(num);
    addReplyLongLong(c, neu);
}

void incrCommand(client *c) {
    incrGeneric(c, 1);
}

void decrCommand(client *c) {
    incrGeneric(c, -1);
}

void incrbyCommand(client *c) {
    long long incr;

    if (!getLongLongFromObject(c->argv[2], &incr)) {
        addReplyError(c, "ERR value is not an integer or out of range");
        return;
    }
    incrGeneric(c, incr);
}

void decrbyCommand(client *c) {
    long long incr;

    if (!getLongLongFromObject(c->argv[2], &incr)) {
        addReplyError(c, "ERR value is not an integer or out of range");
        return;
    }
    if (incr == LLONG_MIN) {
        addReplyError(c, "ERR increment or decrement would overflow");
        return;
    }
    incrGeneric(c, -incr);
}

void appendCommand(client *c) {
    redisDb *db = currentDb(c);
    kvEntry *e = lookupKeyWrite(db, c->argv[1]);
    sds neu;

    if (e && checkType(c, e, OBJ_STRING))
        return;
    if (e)
        neu = sdsdup(objectSds(e->value));
    else
        neu = sdsempty();
    neu = sdscatlen(neu, objectSds(c->argv[2]), sdslen(objectSds(c->argv[2])));
    dbSet(db, c->argv[1], createObject(OBJ_STRING, neu));
    addReplyLongLong(c, (long long)sdslen(neu));
}

void strlenCommand(client *c) {
    kvEntry *e = lookupKeyRead(currentDb(c), c->argv[1]);

    if (!e) {
        addReplyLongLong(c, 0);
        return;
    }
    if (checkType(c, e, OBJ_STRING))
        return;
    addReplyLongLong(c, (long long)sdslen(objectSds(e->value)));
}

void delCommand(client *c) {
    int i;
    int n = 0;

    for (i = 1; i < c->argc; i++) {
        if (lookupKeyRead(currentDb(c), c->argv[i]))
            n += dbDelete(currentDb(c), c->argv[i]);
    }
    addReplyLongLong(c, n);
}

void existsCommand(client *c) {
    int i;
    int n = 0;

    for (i = 1; i < c->argc; i++) {
        if (lookupKeyRead(currentDb(c), c->argv[i]))
            n++;
    }
    addReplyLongLong(c, n);
}

void typeCommand(client *c) {
    kvEntry *e = lookupKeyRead(currentDb(c), c->argv[1]);

    if (!e)
        addReplySimple(c, "none");
    else
        addReplySimple(c, typeName(e->value->type));
}

void keysCommand(client *c) {
    redisDb *db = currentDb(c);
    sds pat = objectSds(c->argv[1]);
    dictIterator *it = dictGetIterator(db->dict);
    dictNode *n;
    list *names = listCreate();
    listIter *li;
    listNode *ln;
    long long now = mstime();

    listSetFreeMethod(names, (void (*)(void *))sdsfree);
    while ((n = dictNext(it)) != NULL) {
        kvEntry *e = (kvEntry *)n;

        if (e->expire_at_ms != -1 && e->expire_at_ms <= now)
            continue;
        if (stringmatch(pat, sdslen(pat), e->key, sdslen(e->key)))
            listAddNodeTail(names, sdsdup(e->key));
    }
    dictReleaseIterator(it);
    addReplyArrayLen(c, (long long)listLength(names));
    li = listGetIterator(names, AL_START_HEAD);
    while ((ln = listNext(li)) != NULL)
        addReplyBulkSds(c, listNodeValue(ln));
    listReleaseIterator(li);
    listRelease(names);
}

void scanCommand(client *c) {
    unsigned long long cursor;
    unsigned long long count = 10;
    sds pat = NULL;
    int i;
    redisDb *db = currentDb(c);
    int table;
    unsigned long bucket;
    unsigned long long next;
    list *names = listCreate();
    listIter *li;
    listNode *ln;
    long long now = mstime();
    unsigned long visited = 0;

    listSetFreeMethod(names, (void (*)(void *))sdsfree);
    cursor = strtoull(objectSds(c->argv[1]), NULL, 10);
    for (i = 2; i < c->argc; i++) {
        if (eqOpt(c->argv[i], "match") && i + 1 < c->argc) {
            pat = objectSds(c->argv[++i]);
        } else if (eqOpt(c->argv[i], "count") && i + 1 < c->argc) {
            long long n;
            if (!getLongLongFromObject(c->argv[++i], &n) || n < 1) {
                addReplyError(c, "ERR syntax error");
                listRelease(names);
                return;
            }
            count = (unsigned long long)n;
        } else {
            addReplyError(c, "ERR syntax error");
            listRelease(names);
            return;
        }
    }
    table = (int)(cursor / 1000000000ULL);
    bucket = (unsigned long)(cursor % 1000000000ULL);
    while (visited < count && table <= 1) {
        unsigned long slots;
        dictNode *node;

        if (table == 1 && !dictIsRehashing(db->dict))
            break;
        slots = dictSlotCount(db->dict, table);
        if (slots == 0 || bucket >= slots) {
            table++;
            bucket = 0;
            continue;
        }
        node = dictSlotHead(db->dict, table, bucket);
        while (node) {
            kvEntry *e = (kvEntry *)node;

            if (!(e->expire_at_ms != -1 && e->expire_at_ms <= now)) {
                if (!pat || stringmatch(pat, sdslen(pat), e->key, sdslen(e->key)))
                    listAddNodeTail(names, sdsdup(e->key));
            }
            node = node->next;
        }
        bucket++;
        visited++;
    }
    if (table > 1 || (table == 1 && !dictIsRehashing(db->dict)) ||
        (table <= 1 && bucket >= dictSlotCount(db->dict, table) && table == 1))
        next = 0;
    else if (table <= 1 && bucket >= dictSlotCount(db->dict, table))
        next = (unsigned long long)(table + 1) * 1000000000ULL;
    else
        next = (unsigned long long)table * 1000000000ULL + bucket;
    if (table > 1)
        next = 0;
    addReplyArrayLen(c, 2);
    {
        sds curs = sdsfromlonglong((long long)next);
        addReplyBulkSds(c, curs);
        sdsfree(curs);
    }
    addReplyArrayLen(c, (long long)listLength(names));
    li = listGetIterator(names, AL_START_HEAD);
    while ((ln = listNext(li)) != NULL)
        addReplyBulkSds(c, listNodeValue(ln));
    listReleaseIterator(li);
    listRelease(names);
}

static void expireGeneric(client *c, int absolute, int milliseconds) {
    long long ttl;
    long long when;
    kvEntry *e;

    if (!getLongLongFromObject(c->argv[2], &ttl)) {
        addReplyError(c, "ERR value is not an integer or out of range");
        return;
    }
    e = lookupKeyWrite(currentDb(c), c->argv[1]);
    c->flags |= CLIENT_SKIP_AOF;
    if (!e) {
        addReplyLongLong(c, 0);
        return;
    }
    if (!absolute) {
        if (ttl <= 0) {
            dbDelete(currentDb(c), c->argv[1]);
            aofFeedPexpireat(c->db_id, c->argv[1], mstime() - 1);
            addReplyLongLong(c, 1);
            return;
        }
        when = mstime() + (milliseconds ? ttl : ttl * 1000);
    } else {
        when = milliseconds ? ttl : ttl * 1000;
        if (when <= mstime()) {
            dbDelete(currentDb(c), c->argv[1]);
            aofFeedPexpireat(c->db_id, c->argv[1], when);
            addReplyLongLong(c, 1);
            return;
        }
    }
    dbSetExpire(currentDb(c), e, when);
    aofFeedPexpireat(c->db_id, c->argv[1], when);
    addReplyLongLong(c, 1);
}

void expireCommand(client *c) {
    expireGeneric(c, 0, 0);
}
void pexpireCommand(client *c) {
    expireGeneric(c, 0, 1);
}
void expireatCommand(client *c) {
    expireGeneric(c, 1, 0);
}
void pexpireatCommand(client *c) {
    expireGeneric(c, 1, 1);
}

static void ttlGeneric(client *c, int milliseconds) {
    kvEntry *e = lookupKeyRead(currentDb(c), c->argv[1]);
    long long left;

    if (!e) {
        addReplyLongLong(c, -2);
        return;
    }
    if (e->expire_at_ms == -1) {
        addReplyLongLong(c, -1);
        return;
    }
    left = e->expire_at_ms - mstime();
    if (left < 0)
        left = 0;
    if (!milliseconds)
        left /= 1000;
    addReplyLongLong(c, left);
}

void ttlCommand(client *c) {
    ttlGeneric(c, 0);
}
void pttlCommand(client *c) {
    ttlGeneric(c, 1);
}

void persistCommand(client *c) {
    kvEntry *e = lookupKeyRead(currentDb(c), c->argv[1]);

    if (!e || e->expire_at_ms == -1) {
        addReplyLongLong(c, 0);
        return;
    }
    dbPersist(currentDb(c), e);
    addReplyLongLong(c, 1);
}

void selectCommand(client *c) {
    long long id;

    if (!getLongLongFromObject(c->argv[1], &id) || id < 0 || id >= server.databases) {
        addReplyError(c, "ERR DB index is out of range");
        return;
    }
    c->db_id = (int)id;
    addReplySimple(c, "OK");
}

void dbsizeCommand(client *c) {
    addReplyLongLong(c, dbSizeLive(currentDb(c)));
}

void flushdbCommand(client *c) {
    emptyDb(currentDb(c));
    addReplySimple(c, "OK");
}

void flushallCommand(client *c) {
    flushAll();
    addReplySimple(c, "OK");
}

static int scanSyntax(client *c) {
    (void)c;
    return 0;
}

void initStringCommands(void) {
    registerCommand("set", setCommand, setSyntax, -3, CMD_WRITE | CMD_DENYOOM, 1, 1, 1);
    registerCommand("setnx", setnxCommand, NULL, 3, CMD_WRITE | CMD_DENYOOM, 1, 1, 1);
    registerCommand("setex", setexCommand, setexSyntax, 4, CMD_WRITE | CMD_DENYOOM, 1, 1, 1);
    registerCommand("get", getCommand, NULL, 2, CMD_READONLY, 1, 1, 1);
    registerCommand("mget", mgetCommand, NULL, -2, CMD_READONLY, 1, -1, 1);
    registerCommand("mset", msetCommand, msetSyntax, -3, CMD_WRITE | CMD_DENYOOM, 1, -1, 2);
    registerCommand("incr", incrCommand, NULL, 2, CMD_WRITE | CMD_DENYOOM, 1, 1, 1);
    registerCommand("decr", decrCommand, NULL, 2, CMD_WRITE | CMD_DENYOOM, 1, 1, 1);
    registerCommand("incrby", incrbyCommand, NULL, 3, CMD_WRITE | CMD_DENYOOM, 1, 1, 1);
    registerCommand("decrby", decrbyCommand, NULL, 3, CMD_WRITE | CMD_DENYOOM, 1, 1, 1);
    registerCommand("append", appendCommand, NULL, 3, CMD_WRITE | CMD_DENYOOM, 1, 1, 1);
    registerCommand("strlen", strlenCommand, NULL, 2, CMD_READONLY, 1, 1, 1);
    registerCommand("del", delCommand, NULL, -2, CMD_WRITE, 1, -1, 1);
    registerCommand("exists", existsCommand, NULL, -2, CMD_READONLY, 1, -1, 1);
    registerCommand("type", typeCommand, NULL, 2, CMD_READONLY, 1, 1, 1);
    registerCommand("keys", keysCommand, NULL, 2, CMD_READONLY, 0, 0, 0);
    registerCommand("scan", scanCommand, scanSyntax, -2, CMD_READONLY, 0, 0, 0);
    registerCommand("expire", expireCommand, NULL, 3, CMD_WRITE, 1, 1, 1);
    registerCommand("pexpire", pexpireCommand, NULL, 3, CMD_WRITE, 1, 1, 1);
    registerCommand("expireat", expireatCommand, NULL, 3, CMD_WRITE, 1, 1, 1);
    registerCommand("pexpireat", pexpireatCommand, NULL, 3, CMD_WRITE, 1, 1, 1);
    registerCommand("ttl", ttlCommand, NULL, 2, CMD_READONLY, 1, 1, 1);
    registerCommand("pttl", pttlCommand, NULL, 2, CMD_READONLY, 1, 1, 1);
    registerCommand("persist", persistCommand, NULL, 2, CMD_WRITE, 1, 1, 1);
    registerCommand("select", selectCommand, NULL, 2, CMD_READONLY, 0, 0, 0);
    registerCommand("dbsize", dbsizeCommand, NULL, 1, CMD_READONLY, 0, 0, 0);
    registerCommand("flushdb", flushdbCommand, NULL, 1, CMD_WRITE, 0, 0, 0);
    registerCommand("flushall", flushallCommand, NULL, 1, CMD_WRITE, 0, 0, 0);
}
