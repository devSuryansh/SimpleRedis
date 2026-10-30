#include "server.h"
#include "zmalloc.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>

static int eq(const char *a, const char *b) {
    return strcasecmp(a, b) == 0;
}

static void fatalConfig(const char *msg) {
    fprintf(stderr, "config: %s\n", msg);
    exit(1);
}

void initServerConfig(void) {
    server.bindaddr = zstrdup("127.0.0.1");
    server.port = 6379;
    server.tcp_backlog = 511;
    server.databases = 16;
    server.timeout = 0;
    server.maxclients = 10000;
    server.maxmemory = 0;
    server.appendonly = 0;
    server.appendfsync = AOF_FSYNC_EVERYSEC;
    server.appendfilename = zstrdup("appendonly.aof");
    server.dbfilename = zstrdup("dump.srdb");
    server.dir = zstrdup("./");
    server.requirepass = sdsempty();
    server.loglevel = LL_NOTICE;
    server.aof_fd = -1;
    server.aof_selected_db = -1;
    server.ipfd = -1;
    server.el_setsize = 10240;
}

static int parseYesNo(const char *v, int *out) {
    if (eq(v, "yes")) {
        *out = 1;
        return 1;
    }
    if (eq(v, "no")) {
        *out = 0;
        return 1;
    }
    return 0;
}

int loadServerConfig(const char *filename) {
    FILE *fp = fopen(filename, "r");
    char line[1024];

    if (!fp)
        fatalConfig("cannot open config file");
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        char *keyword;
        char *value;
        long n;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\n' || *p == '\0')
            continue;
        keyword = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n')
            p++;
        if (*p) {
            *p = '\0';
            p++;
        }
        while (*p == ' ' || *p == '\t')
            p++;
        value = p;
        p = value + strlen(value);
        while (p > value && (p[-1] == '\n' || p[-1] == '\r' || p[-1] == ' ' || p[-1] == '\t'))
            *--p = '\0';
        if (eq(keyword, "bind")) {
            zfree(server.bindaddr);
            server.bindaddr = zstrdup(value);
        } else if (eq(keyword, "port")) {
            n = strtol(value, NULL, 10);
            server.port = (int)n;
        } else if (eq(keyword, "databases")) {
            server.databases = atoi(value);
        } else if (eq(keyword, "timeout")) {
            server.timeout = atoi(value);
        } else if (eq(keyword, "maxclients")) {
            server.maxclients = atoi(value);
        } else if (eq(keyword, "maxmemory")) {
            server.maxmemory = strtoull(value, NULL, 10);
        } else if (eq(keyword, "appendonly")) {
            if (!parseYesNo(value, &server.appendonly))
                fatalConfig("appendonly must be yes or no");
        } else if (eq(keyword, "appendfsync")) {
            if (eq(value, "no"))
                server.appendfsync = AOF_FSYNC_NO;
            else if (eq(value, "always"))
                server.appendfsync = AOF_FSYNC_ALWAYS;
            else if (eq(value, "everysec"))
                server.appendfsync = AOF_FSYNC_EVERYSEC;
            else
                fatalConfig("bad appendfsync");
        } else if (eq(keyword, "appendfilename")) {
            zfree(server.appendfilename);
            server.appendfilename = zstrdup(value);
        } else if (eq(keyword, "dbfilename")) {
            zfree(server.dbfilename);
            server.dbfilename = zstrdup(value);
        } else if (eq(keyword, "dir")) {
            zfree(server.dir);
            server.dir = zstrdup(value);
        } else if (eq(keyword, "requirepass")) {
            sdsfree(server.requirepass);
            server.requirepass = sdsnew(value);
        } else if (eq(keyword, "loglevel")) {
            if (eq(value, "debug"))
                server.loglevel = LL_DEBUG;
            else if (eq(value, "verbose"))
                server.loglevel = LL_VERBOSE;
            else if (eq(value, "notice"))
                server.loglevel = LL_NOTICE;
            else if (eq(value, "warning"))
                server.loglevel = LL_WARNING;
            else
                fatalConfig("bad loglevel");
        } else {
            fatalConfig("unknown directive");
        }
    }
    fclose(fp);
    if (mkdir(server.dir, 0755) == -1 && errno != EEXIST)
        fatalConfig("cannot create dir");
    return 0;
}
