#include "test.h"
#include "server.h"
#include "zmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void run(client *c, int argc, const char **argv, const char *expect) {
    execCommand(c, argc, argv);
    if (strcmp(c->reply, expect) != 0) {
        int i;

        fprintf(stderr, "cmd");
        for (i = 0; i < argc; i++)
            fprintf(stderr, " %s", argv[i]);
        fprintf(stderr, " got [%s]\n", c->reply);
    }
    EXPECT(strcmp(c->reply, expect) == 0);
}

static int fileContains(const char *path, const char *needle) {
    FILE *fp = fopen(path, "rb");
    char buf[8192];
    size_t n;
    int found = 0;

    if (!fp)
        return 0;
    n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    found = strstr(buf, needle) != NULL;
    return found;
}

int main(void) {
    char tmpl[] = "/tmp/srtestXXXXXX";
    char *dir = mkdtemp(tmpl);
    char aofpath[256];
    client *c;
    const char *set[] = {"SET", "k", "v", "PX", "60000"};
    const char *get[] = {"GET", "k"};
    const char *pttl[] = {"PTTL", "k"};
    const char *set2[] = {"SET", "k2", "v2"};
    const char *get2[] = {"GET", "k2"};
    long long ttl = -1;

    EXPECT(dir != NULL);
    initTestServer();
    zfree(server.dir);
    server.dir = zstrdup(dir);
    server.appendonly = 1;
    aofOpen();
    c = createClient(-1);
    run(c, 5, set, "+OK\r\n");
    snprintf(aofpath, sizeof(aofpath), "%s/appendonly.aof", dir);
    EXPECT(fileContains(aofpath, "PEXPIREAT"));
    server.loading = 1;
    emptyDb(currentDb(c));
    server.loading = 0;
    aofLoad();
    run(c, 2, get, "$1\r\nv\r\n");
    execCommand(c, 2, pttl);
    sscanf(c->reply, ":%lld", &ttl);
    EXPECT(ttl >= 0 && ttl <= 60000);

    run(c, 3, set2, "+OK\r\n");
    {
        robj *key = createStringObject("k2", 2);
        kvEntry *e = lookupKeyWrite(currentDb(c), key);

        EXPECT(e != NULL);
        dbSetExpire(currentDb(c), e, 1);
        decrRefCount(key);
    }
    EXPECT(saveSnapshot() == 0);
    server.loading = 1;
    emptyDb(&server.db[0]);
    server.loading = 0;
    EXPECT(loadSnapshot() == 0);
    run(c, 2, get, "$1\r\nv\r\n");
    run(c, 2, get2, "$-1\r\n");
    {
        const char *p1[] = {"RPUSH", "lst", "a"};
        const char *p2[] = {"RPUSH", "lst", "b"};
        const char *llen[] = {"LLEN", "lst"};
        const char *ex[] = {"SET", "ttl", "v", "EX", "100"};
        const char *again[] = {"SET", "ttl", "v2"};
        const char *ttl[] = {"TTL", "ttl"};
        const char *lpush[] = {"LPUSH", "zz", "q"};
        const char *setget[] = {"SET", "zz", "v", "GET"};
        const char *type[] = {"TYPE", "zz"};

        run(c, 3, p1, ":1\r\n");
        run(c, 3, p2, ":2\r\n");
        server.loading = 1;
        emptyDb(currentDb(c));
        server.loading = 0;
        aofLoad();
        run(c, 2, llen, ":2\r\n");
        sdsfree(server.requirepass);
        server.requirepass = sdsnew("pw");
        server.loading = 1;
        emptyDb(currentDb(c));
        server.loading = 0;
        aofLoad();
        c->authenticated = 1;
        run(c, 2, llen, ":2\r\n");
        run(c, 5, ex, "+OK\r\n");
        run(c, 3, again, "+OK\r\n");
        run(c, 2, ttl, ":-1\r\n");
        run(c, 3, lpush, ":1\r\n");
        run(c, 4, setget,
            "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n");
        run(c, 2, type, "+list\r\n");
    }
    freeClient(c);
    return test_finish();
}
