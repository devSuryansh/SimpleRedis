#include "server.h"
#include "anet.h"
#include "siphash.h"
#include "util.h"
#include "zmalloc.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

redisServer server;

volatile sig_atomic_t shutdown_requested;

void serverSignal(int sig) {
    (void)sig;
    shutdown_requested = 1;
}

unsigned int keyHash(const char *s, size_t len) {
    return (unsigned int)siphash24(server.hash_seed, (const unsigned char *)s, len);
}

static unsigned int cmdHash(dictNode *node) {
    redisCommand *cmd = (redisCommand *)node;
    return keyHash(cmd->name, strlen(cmd->name));
}

static int cmdCmp(dictNode *node, const void *key, size_t keylen) {
    redisCommand *cmd = (redisCommand *)node;

    if (strlen(cmd->name) != keylen)
        return 0;
    return memcmp(cmd->name, key, keylen) == 0;
}

static void cmdFree(dictNode *node) {
    zfree(node);
}

void registerCommand(const char *name, redisCommandProc proc, redisSyntaxProc syntax,
                     int arity, int flags, int firstkey, int lastkey, int keystep) {
    redisCommand *cmd = zmalloc(sizeof(*cmd));
    size_t n = strlen(name);

    if (n >= sizeof(cmd->name))
        n = sizeof(cmd->name) - 1;
    memcpy(cmd->name, name, n);
    cmd->name[n] = '\0';
    cmd->proc = proc;
    cmd->syntax = syntax;
    cmd->arity = arity;
    cmd->flags = flags;
    cmd->firstkey = firstkey;
    cmd->lastkey = lastkey;
    cmd->keystep = keystep;
    dictAdd(server.commands, &cmd->node);
}

redisCommand *lookupCommand(const char *name, size_t len) {
    char buf[64];
    size_t i;
    unsigned int h;

    if (len >= sizeof(buf))
        return NULL;
    for (i = 0; i < len; i++) {
        char ch = name[i];
        if (ch >= 'A' && ch <= 'Z')
            ch = (char)(ch - 'A' + 'a');
        buf[i] = ch;
    }
    buf[len] = '\0';
    h = keyHash(buf, len);
    return (redisCommand *)dictFind(server.commands, buf, len, h);
}

void serverLog(int level, const char *fmt, ...) {
    va_list ap;

    if (level < server.loglevel)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

void freeClientArgv(client *c) {
    int i;

    for (i = 0; i < c->argc; i++)
        decrRefCount(c->argv[i]);
    zfree(c->argv);
    c->argv = NULL;
    c->argc = 0;
}

void freeClient(client *c) {
    if (c->fd >= 0 && server.el) {
        aeDeleteFileEvent(server.el, c->fd, AE_READABLE | AE_WRITABLE);
        close(c->fd);
        c->fd = -1;
    }
    if (c->ln && server.clients) {
        listDelNode(server.clients, c->ln);
        c->ln = NULL;
    }
    sdsfree(c->query);
    sdsfree(c->reply);
    sdsfree(c->name);
    freeClientArgv(c);
    if (c->watched_keys)
        listRelease(c->watched_keys);
    if (c->pubsub_channels)
        listRelease(c->pubsub_channels);
    if (c->multi_queue)
        listRelease(c->multi_queue);
    zfree(c);
}

client *createClient(int fd) {
    client *c = zmalloc(sizeof(*c));

    memset(c, 0, sizeof(*c));
    c->id = ++server.next_client_id;
    c->fd = fd;
    c->proto = 2;
    c->authenticated = server.requirepass == NULL || sdslen(server.requirepass) == 0;
    c->query = sdsempty();
    c->reply = sdsempty();
    c->last_interaction_ms = monotime();
    c->watched_keys = listCreate();
    c->pubsub_channels = listCreate();
    c->multi_queue = listCreate();
    if (fd >= 0) {
        if (server.clients)
            listAddNodeTail(server.clients, c);
        if (server.clients)
            c->ln = listLast(server.clients);
        if (server.el)
            aeCreateFileEvent(server.el, fd, AE_READABLE, readQueryFromClient, c);
    }
    return c;
}

void execCommand(client *c, int argc, const char **argv) {
    int i;

    sdsfree(c->reply);
    c->reply = sdsempty();
    c->argc = argc;
    c->argv = zmalloc((size_t)argc * sizeof(robj *));
    for (i = 0; i < argc; i++)
        c->argv[i] = createStringObject(argv[i], strlen(argv[i]));
    processCommand(c);
    freeClientArgv(c);
}

static int arityOk(redisCommand *cmd, int argc) {
    if (cmd->arity > 0)
        return argc == cmd->arity;
    return argc >= -cmd->arity;
}

int processCommand(client *c) {
    sds name;
    redisCommand *cmd;

    if (c->argc <= 0)
        return -1;
    name = objectSds(c->argv[0]);
    cmd = lookupCommand(name, sdslen(name));
    server.total_commands++;
    if (!c->authenticated) {
        if (!cmd || (strcasecmp(cmd->name, "auth") != 0 && strcasecmp(cmd->name, "ping") != 0 &&
                     strcasecmp(cmd->name, "hello") != 0 && strcasecmp(cmd->name, "quit") != 0)) {
            addReplyError(c, "NOAUTH Authentication required.");
            return -1;
        }
    }
    if (!cmd) {
        addReplyErrorFmt(c, "ERR unknown command '%s'", name);
        if (c->flags & CLIENT_MULTI)
            c->flags |= CLIENT_DIRTY_EXEC;
        return -1;
    }
    if (!arityOk(cmd, c->argc)) {
        addReplyErrorFmt(c, "ERR wrong number of arguments for '%s' command", cmd->name);
        if (c->flags & CLIENT_MULTI)
            c->flags |= CLIENT_DIRTY_EXEC;
        return -1;
    }
    if (cmd->syntax && cmd->syntax(c) != 0) {
        if (c->flags & CLIENT_MULTI)
            c->flags |= CLIENT_DIRTY_EXEC;
        return -1;
    }
    if (!(c->flags & CLIENT_MULTI) && (cmd->flags & CMD_DENYOOM) && !server.loading &&
        overMaxmemory()) {
        addReplyError(c, "OOM command not allowed when used memory > 'maxmemory'");
        return -1;
    }
    cmd->proc(c);
    return 0;
}

void pingCommand(client *c) {
    if (c->argc == 1)
        addReplySimple(c, "PONG");
    else
        addReplyBulkSds(c, objectSds(c->argv[1]));
}

void echoCommand(client *c) {
    addReplyBulkSds(c, objectSds(c->argv[1]));
}

void quitCommand(client *c) {
    addReplySimple(c, "OK");
    c->flags |= CLIENT_CLOSE_AFTER_REPLY;
}

static void loadHashSeed(void) {
    FILE *fp = fopen("/dev/urandom", "rb");
    size_t n = 0;

    if (fp) {
        n = fread(server.hash_seed, 1, sizeof(server.hash_seed), fp);
        fclose(fp);
    }
    if (n != sizeof(server.hash_seed)) {
        long long t = mstime();
        memcpy(server.hash_seed, &t, sizeof(t) < sizeof(server.hash_seed) ? sizeof(t)
                                                                           : sizeof(server.hash_seed));
    }
}

static int serverCron(aeEventLoop *el, long long id, void *clientData) {
    (void)el;
    (void)id;
    (void)clientData;
    if (shutdown_requested)
        server.shutdown_asap = 1;
    activeExpireCycle(ACTIVE_EXPIRE_KEYS);
    if (server.timeout > 0 && server.clients) {
        long long now = monotime();
        listIter *it = listGetIterator(server.clients, AL_START_HEAD);
        listNode *ln;

        while ((ln = listNext(it)) != NULL) {
            client *c = listNodeValue(ln);

            if (now - c->last_interaction_ms > (long long)server.timeout * 1000)
                freeClient(c);
        }
        listReleaseIterator(it);
    }
    if (server.shutdown_asap) {
        aeStop(server.el);
        return AE_NOMORE;
    }
    return 100;
}

static void initCommandTable(void) {
    server.commands = dictCreate(cmdCmp, cmdHash, cmdFree);
    registerCommand("ping", pingCommand, NULL, -1, CMD_READONLY, 0, 0, 0);
    registerCommand("echo", echoCommand, NULL, 2, CMD_READONLY, 0, 0, 0);
    registerCommand("quit", quitCommand, NULL, 1, CMD_READONLY, 0, 0, 0);
    initStringCommands();
    initListCommands();
    initHashCommands();
    initSetCommands();
    initZsetCommands();
}

void initTestServer(void) {
    memset(&server, 0, sizeof(server));
    initServerConfig();
    server.hash_seed[0] = 1;
    server.clients = listCreate();
    server.aof_fd = -1;
    server.ipfd = -1;
    initCommandTable();
    initDatabases();
}

void initServer(void) {
    char err[256];
    int fd;

    loadHashSeed();
    server.clients = listCreate();
    server.aof_fd = -1;
    initCommandTable();
    initDatabases();
    server.el_setsize = server.maxclients + 128;
    if (server.el_setsize < 128)
        server.el_setsize = 128;
    server.el = aeCreateEventLoop(server.el_setsize);
    if (!server.el) {
        fprintf(stderr, "failed to create event loop\n");
        exit(1);
    }
    fd = anetTcpServer(err, server.port, server.bindaddr, server.tcp_backlog);
    if (fd == -1) {
        fprintf(stderr, "%s\n", err);
        exit(1);
    }
    anetNonBlock(NULL, fd);
    server.ipfd = fd;
    server.port = anetTcpPort(fd);
    aeCreateFileEvent(server.el, fd, AE_READABLE, acceptTcpHandler, NULL);
    aeCreateTimeEvent(server.el, 100, serverCron, NULL);
}

void prepareShutdown(int force_save, int nosave) {
    (void)force_save;
    (void)nosave;
    if (server.el)
        aeStop(server.el);
    server.shutdown_asap = 1;
}
