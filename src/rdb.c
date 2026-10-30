#include "server.h"
#include "util.h"
#include "zmalloc.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static sds dataPath(const char *name) {
    sds path = sdsnew(server.dir);
    size_t n = sdslen(path);

    if (n == 0 || path[n - 1] != '/')
        path = sdscatlen(path, "/", 1);
    return sdscatlen(path, name, strlen(name));
}

static int wr(FILE *fp, const void *buf, size_t n) {
    return fwrite(buf, 1, n, fp) == n;
}

static int wrU8(FILE *fp, unsigned char v) {
    return wr(fp, &v, 1);
}

static int wrU32(FILE *fp, uint32_t v) {
    unsigned char b[4];

    b[0] = (unsigned char)(v & 255);
    b[1] = (unsigned char)((v >> 8) & 255);
    b[2] = (unsigned char)((v >> 16) & 255);
    b[3] = (unsigned char)((v >> 24) & 255);
    return wr(fp, b, 4);
}

static int wrI64(FILE *fp, long long v) {
    uint64_t u = (uint64_t)v;
    unsigned char b[8];
    int i;

    for (i = 0; i < 8; i++)
        b[i] = (unsigned char)((u >> (8 * i)) & 255);
    return wr(fp, b, 8);
}

static int wrF64(FILE *fp, double v) {
    uint64_t u;
    unsigned char b[8];
    int i;

    memcpy(&u, &v, 8);
    for (i = 0; i < 8; i++)
        b[i] = (unsigned char)((u >> (8 * i)) & 255);
    return wr(fp, b, 8);
}

static int wrStr(FILE *fp, const char *s, size_t n) {
    return wrU32(fp, (uint32_t)n) && wr(fp, s, n);
}

static int rd(FILE *fp, void *buf, size_t n) {
    return fread(buf, 1, n, fp) == n;
}

static int rdU8(FILE *fp, unsigned char *v) {
    return rd(fp, v, 1);
}

static int rdU32(FILE *fp, uint32_t *v) {
    unsigned char b[4];

    if (!rd(fp, b, 4))
        return 0;
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 1;
}

static int rdI64(FILE *fp, long long *v) {
    unsigned char b[8];
    uint64_t u = 0;
    int i;

    if (!rd(fp, b, 8))
        return 0;
    for (i = 0; i < 8; i++)
        u |= (uint64_t)b[i] << (8 * i);
    *v = (long long)u;
    return 1;
}

static int rdF64(FILE *fp, double *v) {
    unsigned char b[8];
    uint64_t u = 0;
    int i;

    if (!rd(fp, b, 8))
        return 0;
    for (i = 0; i < 8; i++)
        u |= (uint64_t)b[i] << (8 * i);
    memcpy(v, &u, 8);
    return 1;
}

static sds rdStr(FILE *fp) {
    uint32_t n;
    sds s;

    if (!rdU32(fp, &n))
        return NULL;
    s = sdsnewlen(NULL, n);
    if (n && !rd(fp, s, n)) {
        sdsfree(s);
        return NULL;
    }
    return s;
}

static int writeValue(FILE *fp, robj *o) {
    if (o->type == OBJ_STRING) {
        return wrStr(fp, objectSds(o), sdslen(objectSds(o)));
    }
    if (o->type == OBJ_LIST) {
        list *l = o->ptr;
        listIter *it = listGetIterator(l, AL_START_HEAD);
        listNode *ln;

        if (!wrU32(fp, (uint32_t)listLength(l))) {
            listReleaseIterator(it);
            return 0;
        }
        while ((ln = listNext(it)) != NULL) {
            sds s = listNodeValue(ln);
            if (!wrStr(fp, s, sdslen(s))) {
                listReleaseIterator(it);
                return 0;
            }
        }
        listReleaseIterator(it);
        return 1;
    }
    if (o->type == OBJ_HASH || o->type == OBJ_SET) {
        dict *d = o->ptr;
        dictIterator *it = dictGetIterator(d);
        dictNode *n;
        int ishash = o->type == OBJ_HASH;

        if (!wrU32(fp, (uint32_t)dictSize(d))) {
            dictReleaseIterator(it);
            return 0;
        }
        while ((n = dictNext(it)) != NULL) {
            if (ishash) {
                hashField *f = (hashField *)n;
                if (!wrStr(fp, f->field, sdslen(f->field)) || !wrStr(fp, f->value, sdslen(f->value))) {
                    dictReleaseIterator(it);
                    return 0;
                }
            } else {
                setMember *m = (setMember *)n;
                if (!wrStr(fp, m->member, sdslen(m->member))) {
                    dictReleaseIterator(it);
                    return 0;
                }
            }
        }
        dictReleaseIterator(it);
        return 1;
    }
    if (o->type == OBJ_ZSET) {
        zset *z = o->ptr;
        unsigned long len = zslLength(z->zsl);
        unsigned long rank;

        if (!wrU32(fp, (uint32_t)len))
            return 0;
        for (rank = 1; rank <= len; rank++) {
            zskiplistNode *node = zslGetElementByRank(z->zsl, rank);
            sds member = zslNodeMember(node);

            if (!wrStr(fp, member, sdslen(member)) || !wrF64(fp, zslNodeScore(node)))
                return 0;
        }
        return 1;
    }
    return 0;
}

int saveSnapshot(void) {
    sds path = dataPath(server.dbfilename);
    sds tmp = sdscatlen(sdsdup(path), ".tmp", 4);
    FILE *fp = fopen(tmp, "wb");
    int dbn;
    uint32_t dbcount = 0;
    long pos_count;

    if (!fp) {
        sdsfree(path);
        sdsfree(tmp);
        return -1;
    }
    if (!wr(fp, "SRDB", 4) || !wrU32(fp, 1))
        goto fail;
    pos_count = ftell(fp);
    if (!wrU32(fp, 0))
        goto fail;
    for (dbn = 0; dbn < server.databases; dbn++) {
        redisDb *db = &server.db[dbn];
        dictIterator *it;
        dictNode *node;
        long long now = mstime();
        long pos_keys;
        uint32_t keys = 0;

        if (dictSize(db->dict) == 0)
            continue;
        dbcount++;
        if (!wrU32(fp, (uint32_t)dbn))
            goto fail;
        pos_keys = ftell(fp);
        if (!wrU32(fp, 0))
            goto fail;
        it = dictGetIterator(db->dict);
        while ((node = dictNext(it)) != NULL) {
            kvEntry *e = (kvEntry *)node;

            if (e->expire_at_ms != -1 && e->expire_at_ms <= now)
                continue;
            if (!wrI64(fp, e->expire_at_ms) || !wrU8(fp, (unsigned char)e->value->type) ||
                !wrStr(fp, e->key, sdslen(e->key)) || !writeValue(fp, e->value)) {
                dictReleaseIterator(it);
                goto fail;
            }
            keys++;
        }
        dictReleaseIterator(it);
        if (fseek(fp, pos_keys, SEEK_SET) != 0 || !wrU32(fp, keys) || fseek(fp, 0, SEEK_END) != 0)
            goto fail;
    }
    if (fseek(fp, pos_count, SEEK_SET) != 0 || !wrU32(fp, dbcount))
        goto fail;
    if (fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
        fclose(fp);
        goto fail_nofile;
    }
    fclose(fp);
    if (rename(tmp, path) != 0)
        goto fail_nofile;
    sdsfree(path);
    sdsfree(tmp);
    return 0;
fail:
    fclose(fp);
fail_nofile:
    unlink(tmp);
    sdsfree(path);
    sdsfree(tmp);
    return -1;
}

static robj *loadValue(FILE *fp, unsigned char type) {
    if (type == 0) {
        sds s = rdStr(fp);

        if (!s)
            return NULL;
        return createObject(OBJ_STRING, s);
    }
    if (type == 1) {
        uint32_t n, i;
        list *l = listCreate();

        listSetFreeMethod(l, (void (*)(void *))sdsfree);
        if (!rdU32(fp, &n)) {
            listRelease(l);
            return NULL;
        }
        for (i = 0; i < n; i++) {
            sds s = rdStr(fp);

            if (!s) {
                listRelease(l);
                return NULL;
            }
            listAddNodeTail(l, s);
        }
        return createObject(OBJ_LIST, l);
    }
    if (type == 2 || type == 3) {
        uint32_t n, i;
        dict *d = type == 2 ? createHashDict() : createSetDict();

        if (!rdU32(fp, &n)) {
            dictRelease(d);
            return NULL;
        }
        for (i = 0; i < n; i++) {
            sds a = rdStr(fp);

            if (!a) {
                dictRelease(d);
                return NULL;
            }
            if (type == 2) {
                sds b = rdStr(fp);
                hashField *f;

                if (!b) {
                    sdsfree(a);
                    dictRelease(d);
                    return NULL;
                }
                f = zmalloc(sizeof(*f));
                f->field = a;
                f->value = b;
                dictAdd(d, &f->node);
            } else {
                setMember *m = zmalloc(sizeof(*m));

                m->member = a;
                dictAdd(d, &m->node);
            }
        }
        return createObject(type == 2 ? OBJ_HASH : OBJ_SET, d);
    }
    if (type == 4) {
        uint32_t n, i;
        zset *z = createZset();

        if (!rdU32(fp, &n)) {
            zslFree(z->zsl);
            dictRelease(z->dict);
            zfree(z);
            return NULL;
        }
        for (i = 0; i < n; i++) {
            sds member = rdStr(fp);
            double score;

            if (!member || !rdF64(fp, &score)) {
                sdsfree(member);
                zslFree(z->zsl);
                dictRelease(z->dict);
                zfree(z);
                return NULL;
            }
            zsetInsertOwned(z, score, member);
        }
        return createObject(OBJ_ZSET, z);
    }
    return NULL;
}

int loadSnapshot(void) {
    sds path = dataPath(server.dbfilename);
    FILE *fp = fopen(path, "rb");
    char magic[4];
    uint32_t version, dbcount, dbi;
    int loading = server.loading;

    sdsfree(path);
    if (!fp)
        return 0;
    if (!rd(fp, magic, 4) || memcmp(magic, "SRDB", 4) != 0 || !rdU32(fp, &version) || version != 1 ||
        !rdU32(fp, &dbcount)) {
        fclose(fp);
        return -1;
    }
    server.loading = 1;
    for (dbi = 0; dbi < dbcount; dbi++) {
        uint32_t index, keys, k;
        redisDb *db;

        if (!rdU32(fp, &index) || !rdU32(fp, &keys) || (int)index >= server.databases) {
            server.loading = loading;
            fclose(fp);
            return -1;
        }
        db = &server.db[index];
        for (k = 0; k < keys; k++) {
            long long expire;
            sds key;
            unsigned char type;
            robj *keyobj;
            robj *val;

            if (!rdI64(fp, &expire) || !rdU8(fp, &type) || !(key = rdStr(fp))) {
                server.loading = loading;
                fclose(fp);
                return -1;
            }
            val = loadValue(fp, type);
            if (!val) {
                sdsfree(key);
                server.loading = loading;
                fclose(fp);
                return -1;
            }
            keyobj = createStringObject(key, sdslen(key));
            sdsfree(key);
            if (expire != -1 && expire <= mstime()) {
                decrRefCount(val);
                decrRefCount(keyobj);
                continue;
            }
            dbSet(db, keyobj, val);
            if (expire != -1) {
                kvEntry *e = lookupKeyWrite(db, keyobj);
                if (e)
                    dbSetExpire(db, e, expire);
            }
            decrRefCount(keyobj);
        }
    }
    server.loading = loading;
    fclose(fp);
    return 0;
}

void saveCommand(client *c) {
    if (saveSnapshot() != 0) {
        addReplyError(c, "ERR save failed");
        return;
    }
    addReplySimple(c, "OK");
}
