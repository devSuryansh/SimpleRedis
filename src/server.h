#ifndef SERVER_H
#define SERVER_H

#include "adlist.h"
#include "ae.h"
#include "dict.h"
#include "pq.h"
#include "sds.h"
#include "skiplist.h"

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#define OBJ_STRING 0
#define OBJ_LIST 1
#define OBJ_HASH 2
#define OBJ_SET 3
#define OBJ_ZSET 4

#define CLIENT_MULTI (1 << 0)
#define CLIENT_DIRTY_CAS (1 << 1)
#define CLIENT_DIRTY_EXEC (1 << 2)
#define CLIENT_CLOSE_AFTER_REPLY (1 << 3)
#define CLIENT_PUBSUB (1 << 4)

#define CMD_WRITE (1 << 0)
#define CMD_DENYOOM (1 << 1)
#define CMD_READONLY (1 << 2)

#define AOF_FSYNC_NO 0
#define AOF_FSYNC_EVERYSEC 1
#define AOF_FSYNC_ALWAYS 2

#define LL_DEBUG 0
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3

#define PROTO_MAX_QUERY (64 * 1024 * 1024)
#define PROTO_MAX_REPLY (64 * 1024 * 1024)
#define ACTIVE_EXPIRE_KEYS 64

typedef struct client client;

typedef struct robj {
    unsigned type;
    unsigned refcount;
    void *ptr;
} robj;

typedef struct kvEntry {
    dictNode node;
    sds key;
    robj *value;
    long long expire_at_ms;
    size_t heap_idx;
} kvEntry;

typedef struct redisDb {
    dict *dict;
    dict *watched_keys;
    pq *expires;
    int id;
} redisDb;

typedef struct watchKey {
    dictNode node;
    sds key;
    list *clients;
} watchKey;

typedef struct pubsubChannel {
    dictNode node;
    sds name;
    list *clients;
} pubsubChannel;

typedef void (*redisCommandProc)(client *c);
typedef int (*redisSyntaxProc)(client *c);

typedef struct redisCommand {
    dictNode node;
    char name[32];
    redisCommandProc proc;
    redisSyntaxProc syntax;
    int arity;
    int flags;
    int firstkey;
    int lastkey;
    int keystep;
} redisCommand;

typedef struct multiCmd {
    redisCommand *cmd;
    int argc;
    robj **argv;
} multiCmd;

typedef struct hashField {
    dictNode node;
    sds field;
    sds value;
} hashField;

typedef struct setMember {
    dictNode node;
    sds member;
} setMember;

typedef struct zmember {
    dictNode node;
    zskiplistNode *sl;
} zmember;

typedef struct zset {
    dict *dict;
    zskiplist *zsl;
} zset;

struct client {
    uint64_t id;
    int fd;
    int db_id;
    int proto;
    int authenticated;
    int flags;
    sds query;
    sds reply;
    sds name;
    int argc;
    robj **argv;
    list *watched_keys;
    list *pubsub_channels;
    list *multi_queue;
    listNode *ln;
    long long last_interaction_ms;
};

typedef struct redisServer {
    char *bindaddr;
    int port;
    int tcp_backlog;
    int databases;
    int timeout;
    int maxclients;
    unsigned long long maxmemory;
    int appendonly;
    int appendfsync;
    char *appendfilename;
    char *dbfilename;
    char *dir;
    sds requirepass;
    int loglevel;
    int aof_fd;
    int aof_selected_db;
    int loading;
    int shutdown_asap;
    uint64_t next_client_id;
    long long total_commands;
    unsigned char hash_seed[16];
    redisDb *db;
    list *clients;
    dict *pubsub_channels;
    dict *commands;
    aeEventLoop *el;
    int ipfd;
    int el_setsize;
} redisServer;

extern redisServer server;

void initServerConfig(void);
int loadServerConfig(const char *filename);
void initServer(void);
void initTestServer(void);
client *createClient(int fd);
void freeClient(client *c);
void freeClientArgv(client *c);
int processCommand(client *c);
void execCommand(client *c, int argc, const char **argv);
void addReplyRaw(client *c, const char *s, size_t len);
void addReplySimple(client *c, const char *s);
void addReplyError(client *c, const char *s);
void addReplyErrorFmt(client *c, const char *fmt, ...);
void addReplyLongLong(client *c, long long n);
void addReplyBulk(client *c, const char *s, size_t len);
void addReplyBulkSds(client *c, sds s);
void addReplyNull(client *c);
void addReplyNullArray(client *c);
void addReplyArrayLen(client *c, long long n);
void addReplyMapLen(client *c, long long n);
void addReplyPushLen(client *c, long long n);
void addReplyBool(client *c, int b);
void addReplyDouble(client *c, double d);
void serverLog(int level, const char *fmt, ...);
void registerCommand(const char *name, redisCommandProc proc, redisSyntaxProc syntax,
                     int arity, int flags, int firstkey, int lastkey, int keystep);
redisCommand *lookupCommand(const char *name, size_t len);
redisDb *currentDb(client *c);
robj *createObject(int type, void *ptr);
robj *createStringObject(const char *ptr, size_t len);
robj *dupStringObject(robj *o);
void incrRefCount(robj *o);
void decrRefCount(robj *o);
unsigned int keyHash(const char *s, size_t len);
int getLongLongFromObject(robj *o, long long *target);
int getDoubleFromObject(robj *o, double *target);
sds objectSds(robj *o);
void dbSet(redisDb *db, robj *key, robj *val);
int dbDelete(redisDb *db, robj *key);
kvEntry *lookupKeyRead(redisDb *db, robj *key);
kvEntry *lookupKeyWrite(redisDb *db, robj *key);
int checkType(client *c, kvEntry *e, int type);
void dbSetExpire(redisDb *db, kvEntry *e, long long when_ms);
void dbPersist(redisDb *db, kvEntry *e);
int expireIfNeeded(redisDb *db, kvEntry *e);
void signalModifiedKey(redisDb *db, sds key);
void touchWatchedKey(redisDb *db, sds key);
void activeExpireCycle(int max_keys);
int dbSizeLive(redisDb *db);
void emptyDb(redisDb *db);
void flushAll(void);
int overMaxmemory(void);
void aofFeed(client *c);
void aofFeedArgv(int dbid, int argc, robj **argv);
void aofLoad(void);
int aofRewrite(void);
void aofFsync(int sync);
int saveSnapshot(void);
int loadSnapshot(void);
void persistForShutdown(int force_save, int nosave);
void prepareShutdown(int force_save, int nosave);
void serverSignal(int sig);
void initDatabases(void);
void initStringCommands(void);
void initListCommands(void);
void initHashCommands(void);
void initSetCommands(void);
void initZsetCommands(void);
void freeHashObject(robj *o);
void freeSetObject(robj *o);
void freeZsetObject(robj *o);
void freeListObject(robj *o);
void unwatchAll(client *c);
void pubsubUnsubscribeAll(client *c);
int clientSubscriptions(client *c);

void pingCommand(client *c);
void echoCommand(client *c);
void readQueryFromClient(aeEventLoop *el, int fd, void *priv, int mask);
void sendReplyToClient(aeEventLoop *el, int fd, void *priv, int mask);
void acceptTcpHandler(aeEventLoop *el, int fd, void *priv, int mask);
void processInputBuffer(client *c);

#endif
