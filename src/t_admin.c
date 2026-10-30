#include "server.h"
#include "util.h"
#include "zmalloc.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static int eq(sds s, const char *lit) {
    size_t n = strlen(lit);
    size_t i;

    if (sdslen(s) != n)
        return 0;
    for (i = 0; i < n; i++) {
        char a = s[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (a != lit[i])
            return 0;
    }
    return 1;
}

static void addCommandSpec(client *c, redisCommand *cmd) {
    int nflags = 0;

    if (cmd->flags & CMD_WRITE)
        nflags++;
    if (cmd->flags & CMD_READONLY)
        nflags++;
    if (cmd->flags & CMD_DENYOOM)
        nflags++;
    addReplyArrayLen(c, 6);
    addReplyBulk(c, cmd->name, strlen(cmd->name));
    addReplyLongLong(c, cmd->arity);
    addReplyArrayLen(c, nflags);
    if (cmd->flags & CMD_WRITE)
        addReplyBulk(c, "write", 5);
    if (cmd->flags & CMD_READONLY)
        addReplyBulk(c, "readonly", 8);
    if (cmd->flags & CMD_DENYOOM)
        addReplyBulk(c, "denyoom", 7);
    addReplyLongLong(c, cmd->firstkey);
    addReplyLongLong(c, cmd->lastkey);
    addReplyLongLong(c, cmd->keystep);
}

static void replyHello(client *c, int proto) {
    if (c->proto >= 3)
        addReplyMapLen(c, 7);
    else
        addReplyArrayLen(c, 14);
    addReplyBulk(c, "server", 6);
    addReplyBulk(c, "redis", 5);
    addReplyBulk(c, "version", 7);
    addReplyBulk(c, "7.2.4", 5);
    addReplyBulk(c, "proto", 5);
    addReplyLongLong(c, proto);
    addReplyBulk(c, "id", 2);
    addReplyLongLong(c, (long long)c->id);
    addReplyBulk(c, "mode", 4);
    addReplyBulk(c, "standalone", 10);
    addReplyBulk(c, "role", 4);
    addReplyBulk(c, "master", 6);
    addReplyBulk(c, "modules", 7);
    addReplyArrayLen(c, 0);
}

void helloCommand(client *c) {
    long long proto = c->proto;
    int i = 2;
    sds pass = NULL;
    sds newname = NULL;

    if (c->argc >= 2) {
        if (!getLongLongFromObject(c->argv[1], &proto) || (proto != 2 && proto != 3)) {
            addReplyError(c, "NOPROTO unsupported protocol version");
            return;
        }
        while (i < c->argc) {
            sds opt = objectSds(c->argv[i]);

            if (eq(opt, "auth")) {
                if (i + 1 >= c->argc) {
                    addReplyError(c, "ERR syntax error");
                    return;
                }
                pass = objectSds(c->argv[++i]);
            } else if (eq(opt, "setname")) {
                if (i + 1 >= c->argc) {
                    addReplyError(c, "ERR syntax error");
                    return;
                }
                newname = objectSds(c->argv[++i]);
            } else {
                addReplyError(c, "ERR syntax error");
                return;
            }
            i++;
        }
        if (pass) {
            if (sdslen(server.requirepass) == 0) {
                addReplyError(c, "ERR AUTH called without any password configured for the default user");
                return;
            }
            if (sdslen(pass) != sdslen(server.requirepass) ||
                memcmp(pass, server.requirepass, sdslen(pass)) != 0) {
                addReplyError(c, "WRONGPASS invalid username-password pair or user is disabled");
                return;
            }
            c->authenticated = 1;
        }
        if (newname) {
            sdsfree(c->name);
            c->name = sdsdup(newname);
        }
        c->proto = (int)proto;
    }
    replyHello(c, (int)proto);
}

void authCommand(client *c) {
    sds pass = objectSds(c->argv[1]);

    if (sdslen(server.requirepass) == 0) {
        addReplyError(c, "ERR AUTH called without any password configured for the default user");
        return;
    }
    if (sdslen(pass) == sdslen(server.requirepass) &&
        memcmp(pass, server.requirepass, sdslen(pass)) == 0) {
        c->authenticated = 1;
        addReplySimple(c, "OK");
        return;
    }
    addReplyError(c, "WRONGPASS invalid username-password pair or user is disabled");
}

void clientCommand(client *c) {
    sds sub = objectSds(c->argv[1]);

    if (eq(sub, "id") && c->argc == 2) {
        addReplyLongLong(c, (long long)c->id);
        return;
    }
    if (eq(sub, "getname") && c->argc == 2) {
        if (!c->name || sdslen(c->name) == 0)
            addReplyNull(c);
        else
            addReplyBulkSds(c, c->name);
        return;
    }
    if (eq(sub, "setname") && c->argc == 3) {
        sdsfree(c->name);
        c->name = sdsdup(objectSds(c->argv[2]));
        addReplySimple(c, "OK");
        return;
    }
    if (eq(sub, "setinfo") && c->argc == 4) {
        sds which = objectSds(c->argv[2]);

        if (eq(which, "lib-name") || eq(which, "lib-ver")) {
            addReplySimple(c, "OK");
            return;
        }
    }
    addReplyError(c, "ERR unknown subcommand");
}

void commandCommand(client *c) {
    dictIterator *it;
    dictNode *n;

    if (c->argc == 1) {
        addReplyArrayLen(c, (long long)dictSize(server.commands));
        it = dictGetIterator(server.commands);
        while ((n = dictNext(it)) != NULL)
            addCommandSpec(c, (redisCommand *)n);
        dictReleaseIterator(it);
        return;
    }
    if (eq(objectSds(c->argv[1]), "count") && c->argc == 2) {
        addReplyLongLong(c, (long long)dictSize(server.commands));
        return;
    }
    if (eq(objectSds(c->argv[1]), "info") && c->argc >= 3) {
        int i;

        addReplyArrayLen(c, c->argc - 2);
        for (i = 2; i < c->argc; i++) {
            sds name = objectSds(c->argv[i]);
            redisCommand *cmd = lookupCommand(name, sdslen(name));

            if (!cmd)
                addReplyNull(c);
            else
                addCommandSpec(c, cmd);
        }
        return;
    }
    addReplyError(c, "ERR unknown subcommand");
}

void roleCommand(client *c) {
    (void)c;
    addReplyArrayLen(c, 3);
    addReplyBulk(c, "master", 6);
    addReplyLongLong(c, 0);
    addReplyArrayLen(c, 0);
}

void timeCommand(client *c) {
    long long us = ustime();
    char sec[32];
    char usec[32];
    int ns = snprintf(sec, sizeof(sec), "%lld", us / 1000000);
    int nu = snprintf(usec, sizeof(usec), "%lld", us % 1000000);

    addReplyArrayLen(c, 2);
    addReplyBulk(c, sec, (size_t)ns);
    addReplyBulk(c, usec, (size_t)nu);
}

static void configPair(client *c, const char *key, const char *val) {
    addReplyBulk(c, key, strlen(key));
    addReplyBulk(c, val, strlen(val));
}

void configGetCommand(client *c) {
    sds pat = objectSds(c->argv[2]);
    const char *keys[12];
    const char *vals[12];
    char num[4][32];
    int n = 0;
    int i;
    int matched = 0;

    snprintf(num[0], sizeof(num[0]), "%d", server.port);
    snprintf(num[1], sizeof(num[1]), "%d", server.databases);
    snprintf(num[2], sizeof(num[2]), "%d", server.timeout);
    snprintf(num[3], sizeof(num[3]), "%d", server.maxclients);
    keys[n] = "port";
    vals[n++] = num[0];
    keys[n] = "bind";
    vals[n++] = server.bindaddr;
    keys[n] = "databases";
    vals[n++] = num[1];
    keys[n] = "timeout";
    vals[n++] = num[2];
    keys[n] = "maxclients";
    vals[n++] = num[3];
    keys[n] = "maxmemory";
    {
        static char mem[32];
        snprintf(mem, sizeof(mem), "%llu", server.maxmemory);
        vals[n++] = mem;
    }
    keys[n] = "appendonly";
    vals[n++] = server.appendonly ? "yes" : "no";
    keys[n] = "appendfsync";
    vals[n++] = server.appendfsync == AOF_FSYNC_ALWAYS     ? "always"
                : server.appendfsync == AOF_FSYNC_NO       ? "no"
                                                           : "everysec";
    keys[n] = "appendfilename";
    vals[n++] = server.appendfilename;
    keys[n] = "dir";
    vals[n++] = server.dir;
    keys[n] = "dbfilename";
    vals[n++] = server.dbfilename;
    keys[n] = "requirepass";
    vals[n++] = server.requirepass;
    for (i = 0; i < n; i++) {
        if (stringmatch(pat, sdslen(pat), keys[i], strlen(keys[i])))
            matched++;
    }
    if (c->proto >= 3)
        addReplyMapLen(c, matched);
    else
        addReplyArrayLen(c, matched * 2);
    for (i = 0; i < n; i++) {
        if (stringmatch(pat, sdslen(pat), keys[i], strlen(keys[i])))
            configPair(c, keys[i], vals[i]);
    }
}

void configCommand(client *c) {
    sds sub = objectSds(c->argv[1]);

    if (eq(sub, "get") && c->argc == 3) {
        configGetCommand(c);
        return;
    }
    if (eq(sub, "set") && c->argc == 4) {
        sds key = objectSds(c->argv[2]);
        sds val = objectSds(c->argv[3]);

        if (eq(key, "timeout")) {
            long long n;
            if (!getLongLongFromObject(c->argv[3], &n) || n < 0) {
                addReplyError(c, "ERR Invalid argument");
                return;
            }
            server.timeout = (int)n;
            addReplySimple(c, "OK");
            return;
        }
        if (eq(key, "appendfsync")) {
            if (eq(val, "always"))
                server.appendfsync = AOF_FSYNC_ALWAYS;
            else if (eq(val, "everysec"))
                server.appendfsync = AOF_FSYNC_EVERYSEC;
            else if (eq(val, "no"))
                server.appendfsync = AOF_FSYNC_NO;
            else {
                addReplyError(c, "ERR Invalid argument");
                return;
            }
            addReplySimple(c, "OK");
            return;
        }
        addReplyErrorFmt(c, "ERR Unsupported CONFIG parameter: %s", key);
        return;
    }
    addReplyError(c, "ERR unknown subcommand");
}

void infoCommand(client *c) {
    char buf[2048];
    int n = snprintf(
        buf, sizeof(buf),
        "# Server\r\nredis_version:7.2.4\r\nsimpleredis_version:0.1.0\r\n"
        "tcp_port:%d\r\nrole:master\r\n"
        "# Clients\r\nconnected_clients:%lu\r\n"
        "# Memory\r\nused_memory:%zu\r\n"
        "# Persistence\r\naof_enabled:%d\r\n"
        "# Stats\r\ntotal_commands_processed:%lld\r\n"
        "# Keyspace\r\n",
        server.port, server.clients ? listLength(server.clients) : 0, zmallocUsed(),
        server.appendonly, server.total_commands);

    if (n < 0)
        n = 0;
    addReplyBulk(c, buf, (size_t)n);
}

void shutdownCommand(client *c) {
    int nosave = 0;
    int force = 0;

    if (c->argc == 2) {
        sds opt = objectSds(c->argv[1]);

        if (eq(opt, "nosave"))
            nosave = 1;
        else if (eq(opt, "save"))
            force = 1;
        else {
            addReplyError(c, "ERR syntax error");
            return;
        }
    }
    addReplySimple(c, "OK");
    server.shutdown_nosave = nosave;
    server.shutdown_force_save = force;
    server.shutdown_asap = 1;
    if (c->fd >= 0)
        c->flags |= CLIENT_CLOSE_AFTER_REPLY;
}

void initAdminCommands(void) {
    registerCommand("hello", helloCommand, NULL, -1, CMD_READONLY, 0, 0, 0);
    registerCommand("auth", authCommand, NULL, 2, CMD_READONLY, 0, 0, 0);
    registerCommand("client", clientCommand, NULL, -2, CMD_READONLY, 0, 0, 0);
    registerCommand("command", commandCommand, NULL, -1, CMD_READONLY, 0, 0, 0);
    registerCommand("role", roleCommand, NULL, 1, CMD_READONLY, 0, 0, 0);
    registerCommand("time", timeCommand, NULL, 1, CMD_READONLY, 0, 0, 0);
    registerCommand("config", configCommand, NULL, -2, CMD_READONLY, 0, 0, 0);
    registerCommand("info", infoCommand, NULL, -1, CMD_READONLY, 0, 0, 0);
    registerCommand("shutdown", shutdownCommand, NULL, -1, CMD_READONLY, 0, 0, 0);
}
