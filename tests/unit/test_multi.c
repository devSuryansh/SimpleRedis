#include "test.h"
#include "server.h"
#include "zmalloc.h"

#include <string.h>

static void run(client *c, int argc, const char **argv, const char *expect) {
    execCommand(c, argc, argv);
    EXPECT(strcmp(c->reply, expect) == 0);
}

int main(void) {
    client *c;
    const char *multi[] = {"MULTI"};
    const char *set[] = {"SET", "a", "1"};
    const char *exec[] = {"EXEC"};
    const char *get[] = {"GET", "a"};
    const char *watch[] = {"WATCH", "a"};
    const char *set2[] = {"SET", "a", "2"};
    const char *lpush[] = {"LPUSH", "l", "x"};
    const char *incr[] = {"INCR", "l"};
    const char *nope[] = {"NOPE"};
    robj *key;
    robj *val;

    initTestServer();
    c = createClient(-1);
    run(c, 1, multi, "+OK\r\n");
    run(c, 3, set, "+QUEUED\r\n");
    run(c, 1, exec, "*1\r\n+OK\r\n");
    run(c, 2, get, "$1\r\n1\r\n");

    run(c, 2, watch, "+OK\r\n");
    key = createStringObject("a", 1);
    val = createStringObject("ext", 3);
    dbSet(currentDb(c), key, val);
    decrRefCount(key);
    run(c, 1, multi, "+OK\r\n");
    run(c, 3, set2, "+QUEUED\r\n");
    run(c, 1, exec, "*-1\r\n");
    run(c, 2, get, "$3\r\next\r\n");

    run(c, 3, lpush, ":1\r\n");
    run(c, 1, multi, "+OK\r\n");
    run(c, 2, incr, "+QUEUED\r\n");
    execCommand(c, 1, exec);
    EXPECT(strstr(c->reply, "-WRONGTYPE Operation against a key holding the wrong kind of value") !=
           NULL);

    run(c, 1, multi, "+OK\r\n");
    execCommand(c, 1, nope);
    EXPECT(strstr(c->reply, "unknown command") != NULL);
    run(c, 1, exec, "-EXECABORT Transaction discarded because of previous errors.\r\n");
    freeClient(c);
    return test_finish();
}
