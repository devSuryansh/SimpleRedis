#include "test.h"
#include "server.h"

#include <string.h>

static void run(client *c, int argc, const char **argv, const char *expect) {
    execCommand(c, argc, argv);
    EXPECT(strcmp(c->reply, expect) == 0);
}

int main(void) {
    client *c;
    const char *lpush[] = {"LPUSH", "l", "a", "b"};
    const char *lrange[] = {"LRANGE", "l", "0", "-1"};
    const char *hset[] = {"HSET", "h", "f", "v"};
    const char *hset2[] = {"HSET", "h", "f", "v2"};
    const char *hgetall[] = {"HGETALL", "h"};
    const char *sadd[] = {"SADD", "s", "m"};
    const char *sadd2[] = {"SADD", "s", "m"};
    const char *zadd[] = {"ZADD", "z", "1", "b", "1", "a"};
    const char *zrange[] = {"ZRANGE", "z", "0", "-1"};
    const char *getl[] = {"GET", "l"};

    initTestServer();
    c = createClient(-1);
    run(c, 4, lpush, ":2\r\n");
    run(c, 4, lrange, "*2\r\n$1\r\nb\r\n$1\r\na\r\n");
    run(c, 4, hset, ":1\r\n");
    run(c, 4, hset2, ":0\r\n");
    run(c, 2, hgetall, "*2\r\n$1\r\nf\r\n$2\r\nv2\r\n");
    c->proto = 3;
    run(c, 2, hgetall, "%1\r\n$1\r\nf\r\n$2\r\nv2\r\n");
    c->proto = 2;
    run(c, 3, sadd, ":1\r\n");
    run(c, 3, sadd2, ":0\r\n");
    run(c, 6, zadd, ":2\r\n");
    run(c, 4, zrange, "*2\r\n$1\r\na\r\n$1\r\nb\r\n");
    run(c, 2, getl, "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n");
    freeClient(c);
    return test_finish();
}
