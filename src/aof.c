#include "server.h"
#include "resp.h"
#include "util.h"
#include "zmalloc.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t aof_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t aof_cv = PTHREAD_COND_INITIALIZER;
static int aof_bio_stop;
static int aof_bio_fsync;
static int aof_bio_started;
static pthread_t aof_bio_thread;
static long long aof_last_fsync;

static sds dataPath(const char *name) {
    sds path = sdsnew(server.dir);
    size_t n = sdslen(path);

    if (n == 0 || path[n - 1] != '/')
        path = sdscatlen(path, "/", 1);
    return sdscatlen(path, name, strlen(name));
}

static int writeAll(int fd, const char *buf, size_t len) {
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static sds encodeArgv(int argc, robj **argv) {
    sds out = sdsempty();
    char hdr[64];
    int i;
    int n = snprintf(hdr, sizeof(hdr), "*%d\r\n", argc);

    out = sdscatlen(out, hdr, (size_t)n);
    for (i = 0; i < argc; i++) {
        sds s = objectSds(argv[i]);
        n = snprintf(hdr, sizeof(hdr), "$%zu\r\n", sdslen(s));
        out = sdscatlen(out, hdr, (size_t)n);
        out = sdscatlen(out, s, sdslen(s));
        out = sdscatlen(out, "\r\n", 2);
    }
    return out;
}

static int aofWriteFd(int fd, int argc, robj **argv) {
    sds buf = encodeArgv(argc, argv);
    int rc = writeAll(fd, buf, sdslen(buf));

    sdsfree(buf);
    return rc;
}

static void aofSelect(int fd, int dbid) {
    robj *argv[2];
    char num[32];
    int n;

    if (server.aof_selected_db == dbid)
        return;
    n = snprintf(num, sizeof(num), "%d", dbid);
    argv[0] = createStringObject("SELECT", 6);
    argv[1] = createStringObject(num, (size_t)n);
    aofWriteFd(fd, 2, argv);
    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
    server.aof_selected_db = dbid;
}

void aofFeedArgv(int dbid, int argc, robj **argv) {
    if (!server.appendonly || server.loading || server.aof_fd < 0)
        return;
    aofSelect(server.aof_fd, dbid);
    if (aofWriteFd(server.aof_fd, argc, argv) != 0)
        serverLog(LL_WARNING, "AOF write failed");
    if (server.appendfsync == AOF_FSYNC_ALWAYS)
        fsync(server.aof_fd);
}

void aofFeed(client *c) {
    aofFeedArgv(c->db_id, c->argc, c->argv);
}

void aofFeedPlainSet(int dbid, robj *key, robj *val) {
    robj *argv[3];

    argv[0] = createStringObject("SET", 3);
    argv[1] = key;
    argv[2] = val;
    aofFeedArgv(dbid, 3, argv);
    decrRefCount(argv[0]);
}

void aofFeedPexpireat(int dbid, robj *key, long long when_ms) {
    char num[32];
    int n = snprintf(num, sizeof(num), "%lld", when_ms);
    robj *argv[3];

    argv[0] = createStringObject("PEXPIREAT", 9);
    argv[1] = key;
    argv[2] = createStringObject(num, (size_t)n);
    aofFeedArgv(dbid, 3, argv);
    decrRefCount(argv[0]);
    decrRefCount(argv[2]);
}

static void *aofBio(void *arg) {
    (void)arg;
    for (;;) {
        int stop;
        int do_fsync;
        int fd;

        pthread_mutex_lock(&aof_mu);
        while (!aof_bio_stop && !aof_bio_fsync)
            pthread_cond_wait(&aof_cv, &aof_mu);
        stop = aof_bio_stop;
        do_fsync = aof_bio_fsync;
        aof_bio_fsync = 0;
        fd = server.aof_fd;
        pthread_mutex_unlock(&aof_mu);
        if (do_fsync && fd >= 0)
            fsync(fd);
        if (stop)
            break;
    }
    return NULL;
}

static void aofStartBio(void) {
    if (aof_bio_started)
        return;
    aof_bio_stop = 0;
    if (pthread_create(&aof_bio_thread, NULL, aofBio, NULL) == 0)
        aof_bio_started = 1;
}

void aofFsync(int sync_now) {
    (void)sync_now;
    if (server.aof_fd >= 0)
        fsync(server.aof_fd);
}

void aofCron(void) {
    long long now;

    if (!server.appendonly || server.appendfsync != AOF_FSYNC_EVERYSEC || server.aof_fd < 0)
        return;
    now = monotime();
    if (now - aof_last_fsync < 1000)
        return;
    aof_last_fsync = now;
    if (!aof_bio_started) {
        fsync(server.aof_fd);
        return;
    }
    pthread_mutex_lock(&aof_mu);
    aof_bio_fsync = 1;
    pthread_cond_signal(&aof_cv);
    pthread_mutex_unlock(&aof_mu);
}

void aofShutdown(void) {
    if (aof_bio_started) {
        pthread_mutex_lock(&aof_mu);
        aof_bio_stop = 1;
        pthread_cond_signal(&aof_cv);
        pthread_mutex_unlock(&aof_mu);
        pthread_join(aof_bio_thread, NULL);
        aof_bio_started = 0;
    }
    if (server.aof_fd >= 0) {
        fsync(server.aof_fd);
        close(server.aof_fd);
        server.aof_fd = -1;
    }
}

void aofOpen(void) {
    sds path = dataPath(server.appendfilename);

    server.aof_fd = open(path, O_RDWR | O_CREAT, 0644);
    sdsfree(path);
    if (server.aof_fd < 0) {
        serverLog(LL_WARNING, "cannot open AOF");
        return;
    }
    if (server.appendfsync == AOF_FSYNC_EVERYSEC)
        aofStartBio();
}

static void replayBuffer(client *c, sds buf) {
    size_t off = 0;

    while (off < sdslen(buf)) {
        respVal v;
        size_t consumed = 0;
        int rc = respParse(buf + off, sdslen(buf) - off, &consumed, &v);
        int i;

        if (rc == 0)
            break;
        if (rc != 1 || v.type != RESP_ARRAY) {
            respFree(&v);
            break;
        }
        c->argc = v.nitems;
        c->argv = zmalloc((size_t)c->argc * sizeof(robj *));
        for (i = 0; i < c->argc; i++) {
            sds s = v.items[i].str ? v.items[i].str : sdsempty();
            c->argv[i] = createStringObject(s, sdslen(s));
        }
        respFree(&v);
        processCommand(c);
        freeClientArgv(c);
        sdsfree(c->reply);
        c->reply = sdsempty();
        off += consumed;
    }
}

void aofLoad(void) {
    sds path;
    int fd;
    sds buf;
    client *c;
    char tmp[4096];
    ssize_t n;

    if (server.aof_fd < 0)
        return;
    path = dataPath(server.appendfilename);
    fd = server.aof_fd;
    if (lseek(fd, 0, SEEK_SET) < 0) {
        sdsfree(path);
        return;
    }
    buf = sdsempty();
    while ((n = read(fd, tmp, sizeof(tmp))) > 0)
        buf = sdscatlen(buf, tmp, (size_t)n);
    lseek(fd, 0, SEEK_END);
    sdsfree(path);
    server.loading = 1;
    c = createClient(-1);
    c->authenticated = 1;
    replayBuffer(c, buf);
    freeClient(c);
    server.loading = 0;
    sdsfree(buf);
}

static int rewriteToFd(int fd) {
    int dbn;

    server.aof_selected_db = -1;
    for (dbn = 0; dbn < server.databases; dbn++) {
        redisDb *db = &server.db[dbn];
        dictIterator *it = dictGetIterator(db->dict);
        dictNode *node;
        long long now = mstime();
        int wrote_select = 0;

        while ((node = dictNext(it)) != NULL) {
            kvEntry *e = (kvEntry *)node;
            robj *keyobj;

            if (e->expire_at_ms != -1 && e->expire_at_ms <= now)
                continue;
            if (!wrote_select) {
                aofSelect(fd, dbn);
                wrote_select = 1;
            }
            keyobj = createStringObject(e->key, sdslen(e->key));
            if (e->value->type == OBJ_STRING) {
                robj *argv[3];
                argv[0] = createStringObject("SET", 3);
                argv[1] = keyobj;
                argv[2] = e->value;
                aofWriteFd(fd, 3, argv);
                decrRefCount(argv[0]);
            } else if (e->value->type == OBJ_LIST) {
                list *l = e->value->ptr;
                listIter *li = listGetIterator(l, AL_START_HEAD);
                listNode *ln;
                int n = (int)listLength(l);
                robj **argv = zmalloc((size_t)(n + 2) * sizeof(robj *));
                int i = 2;

                argv[0] = createStringObject("RPUSH", 5);
                argv[1] = keyobj;
                while ((ln = listNext(li)) != NULL)
                    argv[i++] = createStringObject(listNodeValue(ln), sdslen(listNodeValue(ln)));
                listReleaseIterator(li);
                aofWriteFd(fd, i, argv);
                decrRefCount(argv[0]);
                for (i = 2; i < n + 2; i++)
                    decrRefCount(argv[i]);
                zfree(argv);
            } else if (e->value->type == OBJ_HASH || e->value->type == OBJ_SET) {
                dict *d = e->value->ptr;
                dictIterator *fit = dictGetIterator(d);
                dictNode *fn;
                int count = (int)dictSize(d);
                robj **argv = zmalloc((size_t)(count * 2 + 2) * sizeof(robj *));
                int i = 2;
                int ishash = e->value->type == OBJ_HASH;

                argv[0] = createStringObject(ishash ? "HSET" : "SADD", ishash ? 4 : 4);
                argv[1] = keyobj;
                while ((fn = dictNext(fit)) != NULL) {
                    if (ishash) {
                        hashField *f = (hashField *)fn;
                        argv[i++] = createStringObject(f->field, sdslen(f->field));
                        argv[i++] = createStringObject(f->value, sdslen(f->value));
                    } else {
                        setMember *m = (setMember *)fn;
                        argv[i++] = createStringObject(m->member, sdslen(m->member));
                    }
                }
                dictReleaseIterator(fit);
                aofWriteFd(fd, i, argv);
                decrRefCount(argv[0]);
                {
                    int j;
                    for (j = 2; j < i; j++)
                        decrRefCount(argv[j]);
                }
                zfree(argv);
            } else if (e->value->type == OBJ_ZSET) {
                zset *z = e->value->ptr;
                unsigned long len = zslLength(z->zsl);
                robj **argv = zmalloc((len * 2 + 2) * sizeof(robj *));
                unsigned long rank;
                int i = 2;

                argv[0] = createStringObject("ZADD", 4);
                argv[1] = keyobj;
                for (rank = 1; rank <= len; rank++) {
                    zskiplistNode *zn = zslGetElementByRank(z->zsl, rank);
                    char score[64];
                    int ns = d2string(score, sizeof(score), zslNodeScore(zn));

                    argv[i++] = createStringObject(score, (size_t)ns);
                    argv[i++] = createStringObject(zslNodeMember(zn), sdslen(zslNodeMember(zn)));
                }
                aofWriteFd(fd, i, argv);
                decrRefCount(argv[0]);
                {
                    int j;
                    for (j = 2; j < i; j++)
                        decrRefCount(argv[j]);
                }
                zfree(argv);
            }
            if (e->expire_at_ms != -1) {
                char num[32];
                int ns = snprintf(num, sizeof(num), "%lld", e->expire_at_ms);
                robj *argv[3];

                argv[0] = createStringObject("PEXPIREAT", 9);
                argv[1] = keyobj;
                argv[2] = createStringObject(num, (size_t)ns);
                aofWriteFd(fd, 3, argv);
                decrRefCount(argv[0]);
                decrRefCount(argv[2]);
            }
            decrRefCount(keyobj);
        }
        dictReleaseIterator(it);
    }
    return 0;
}

int aofRewrite(void) {
    sds path = dataPath(server.appendfilename);
    sds tmp = sdscatlen(sdsdup(path), ".tmp", 4);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int old;

    if (fd < 0) {
        sdsfree(path);
        sdsfree(tmp);
        return -1;
    }
    rewriteToFd(fd);
    fsync(fd);
    close(fd);
    if (rename(tmp, path) != 0) {
        sdsfree(path);
        sdsfree(tmp);
        return -1;
    }
    old = server.aof_fd;
    server.aof_fd = open(path, O_RDWR | O_APPEND, 0644);
    if (old >= 0)
        close(old);
    server.aof_selected_db = -1;
    sdsfree(path);
    sdsfree(tmp);
    return server.aof_fd < 0 ? -1 : 0;
}

void persistForShutdown(int force_save, int nosave) {
    if (!nosave) {
        if (force_save || !server.appendonly)
            saveSnapshot();
        if (server.appendonly && server.aof_fd >= 0)
            fsync(server.aof_fd);
    }
    aofShutdown();
}

void rewriteaofCommand(client *c) {
    if (!server.appendonly) {
        addReplyError(c, "ERR AOF is disabled");
        return;
    }
    if (aofRewrite() != 0) {
        addReplyError(c, "ERR AOF rewrite failed");
        return;
    }
    addReplySimple(c, "OK");
}

void initPersistCommands(void) {
    registerCommand("rewriteaof", rewriteaofCommand, NULL, 1, CMD_READONLY, 0, 0, 0);
    registerCommand("save", saveCommand, NULL, 1, CMD_READONLY, 0, 0, 0);
}
