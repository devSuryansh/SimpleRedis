#include "server.h"
#include "anet.h"
#include "resp.h"
#include "util.h"
#include "zmalloc.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void addReplyRaw(client *c, const char *s, size_t len) {
    if (c->flags & CLIENT_CLOSE_AFTER_REPLY && sdslen(c->reply) > PROTO_MAX_REPLY)
        return;
    c->reply = sdscatlen(c->reply, s, len);
    if (sdslen(c->reply) > PROTO_MAX_REPLY)
        c->flags |= CLIENT_CLOSE_AFTER_REPLY;
}

void addReplySimple(client *c, const char *s) {
    addReplyRaw(c, "+", 1);
    addReplyRaw(c, s, strlen(s));
    addReplyRaw(c, "\r\n", 2);
}

void addReplyError(client *c, const char *s) {
    if (s[0] != '-')
        addReplyRaw(c, "-", 1);
    addReplyRaw(c, s, strlen(s));
    addReplyRaw(c, "\r\n", 2);
}

void addReplyErrorFmt(client *c, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        n = 0;
    if ((size_t)n >= sizeof(buf))
        n = (int)sizeof(buf) - 1;
    addReplyError(c, buf);
}

void addReplyLongLong(client *c, long long n) {
    char buf[64];
    int len = snprintf(buf, sizeof(buf), ":%lld\r\n", n);

    addReplyRaw(c, buf, (size_t)len);
}

void addReplyBulk(client *c, const char *s, size_t len) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "$%zu\r\n", len);

    addReplyRaw(c, buf, (size_t)n);
    addReplyRaw(c, s, len);
    addReplyRaw(c, "\r\n", 2);
}

void addReplyBulkSds(client *c, sds s) {
    addReplyBulk(c, s, sdslen(s));
}

void addReplyNull(client *c) {
    if (c->proto >= 3)
        addReplyRaw(c, "_\r\n", 3);
    else
        addReplyRaw(c, "$-1\r\n", 5);
}

void addReplyNullArray(client *c) {
    if (c->proto >= 3)
        addReplyRaw(c, "_\r\n", 3);
    else
        addReplyRaw(c, "*-1\r\n", 5);
}

static void addReplyPrefixedLen(client *c, char prefix, long long n) {
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%c%lld\r\n", prefix, n);

    addReplyRaw(c, buf, (size_t)len);
}

void addReplyArrayLen(client *c, long long n) {
    addReplyPrefixedLen(c, '*', n);
}

void addReplyMapLen(client *c, long long n) {
    if (c->proto >= 3)
        addReplyPrefixedLen(c, '%', n);
    else
        addReplyPrefixedLen(c, '*', n * 2);
}

void addReplyPushLen(client *c, long long n) {
    if (c->proto >= 3)
        addReplyPrefixedLen(c, '>', n);
    else
        addReplyPrefixedLen(c, '*', n);
}

void addReplyBool(client *c, int b) {
    if (c->proto >= 3)
        addReplyRaw(c, b ? "#t\r\n" : "#f\r\n", 4);
    else
        addReplyLongLong(c, b ? 1 : 0);
}

void addReplyDouble(client *c, double d) {
    char buf[64];
    int n = d2string(buf, sizeof(buf), d);

    if (c->proto >= 3) {
        addReplyRaw(c, ",", 1);
        addReplyRaw(c, buf, (size_t)n);
        addReplyRaw(c, "\r\n", 2);
    } else {
        addReplyBulk(c, buf, (size_t)n);
    }
}

static void installWriteHandler(client *c) {
    if (c->fd < 0 || !server.el)
        return;
    if (sdslen(c->reply) == 0)
        return;
    aeCreateFileEvent(server.el, c->fd, AE_WRITABLE, sendReplyToClient, c);
}

void sendReplyToClient(aeEventLoop *el, int fd, void *priv, int mask) {
    client *c = priv;
    ssize_t n;

    (void)el;
    (void)mask;
    if (sdslen(c->reply) == 0) {
        aeDeleteFileEvent(server.el, fd, AE_WRITABLE);
        return;
    }
    n = write(fd, c->reply, sdslen(c->reply));
    if (n == -1) {
        if (errno == EAGAIN || errno == EINTR)
            return;
        freeClient(c);
        return;
    }
    sdsDiscard(c->reply, (size_t)n);
    if (sdslen(c->reply) == 0) {
        aeDeleteFileEvent(server.el, fd, AE_WRITABLE);
        if (c->flags & CLIENT_CLOSE_AFTER_REPLY)
            freeClient(c);
    }
}

static void shiftQuery(client *c, size_t n) {
    sdsDiscard(c->query, n);
}

static int argvFromResp(client *c, respVal *v) {
    int i;

    if (v->type != RESP_ARRAY || v->nitems < 1)
        return -1;
    for (i = 0; i < v->nitems; i++) {
        if (v->items[i].type != RESP_BULK)
            return -1;
    }
    c->argc = v->nitems;
    c->argv = zmalloc((size_t)c->argc * sizeof(robj *));
    for (i = 0; i < c->argc; i++) {
        sds s = v->items[i].str;
        c->argv[i] = createStringObject(s, sdslen(s));
    }
    return 0;
}

void processInputBuffer(client *c) {
    while (sdslen(c->query) > 0) {
        respVal v;
        size_t consumed = 0;
        int rc;

        if (sdslen(c->query) == 0)
            break;
        if (c->flags & CLIENT_CLOSE_AFTER_REPLY)
            break;
        rc = respParse(c->query, sdslen(c->query), &consumed, &v);
        if (rc == 0)
            return;
        if (rc == -1) {
            addReplyError(c, "ERR Protocol error");
            c->flags |= CLIENT_CLOSE_AFTER_REPLY;
            return;
        }
        if (argvFromResp(c, &v) != 0) {
            addReplyError(c, "ERR Protocol error");
            c->flags |= CLIENT_CLOSE_AFTER_REPLY;
            respFree(&v);
            shiftQuery(c, consumed);
            return;
        }
        respFree(&v);
        shiftQuery(c, consumed);
        processCommand(c);
        freeClientArgv(c);
        if (c->flags & CLIENT_CLOSE_AFTER_REPLY && sdslen(c->query) > 0)
            break;
    }
    installWriteHandler(c);
}

void readQueryFromClient(aeEventLoop *el, int fd, void *priv, int mask) {
    client *c = priv;
    char buf[16384];
    ssize_t n;

    (void)el;
    (void)mask;
    n = read(fd, buf, sizeof(buf));
    if (n == -1) {
        if (errno == EAGAIN || errno == EINTR)
            return;
        freeClient(c);
        return;
    }
    if (n == 0) {
        freeClient(c);
        return;
    }
    c->last_interaction_ms = monotime();
    c->query = sdscatlen(c->query, buf, (size_t)n);
    if (sdslen(c->query) > PROTO_MAX_QUERY) {
        freeClient(c);
        return;
    }
    processInputBuffer(c);
}

void acceptTcpHandler(aeEventLoop *el, int fd, void *priv, int mask) {
    char ip[64];
    int port = 0;
    int cfd;

    (void)el;
    (void)priv;
    (void)mask;
    cfd = anetTcpAccept(NULL, fd, ip, sizeof(ip), &port);
    if (cfd == -1)
        return;
    if (listLength(server.clients) >= (unsigned long)server.maxclients) {
        const char *err = "-ERR max number of clients reached\r\n";
        ssize_t w = write(cfd, err, strlen(err));
        (void)w;
        close(cfd);
        return;
    }
    if (anetNonBlock(NULL, cfd) == -1 || anetEnableTcpNoDelay(NULL, cfd) == -1) {
        close(cfd);
        return;
    }
    createClient(cfd);
}
