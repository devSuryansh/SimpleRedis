#include "test.h"
#include "server.h"

#include <string.h>
#include <unistd.h>

static void run(client *c, int argc, const char **argv, const char *expect) {
    execCommand(c, argc, argv);
    EXPECT(strcmp(c->reply, expect) == 0);
}

int main(void) {
    client *c;
    const char *set[] = {"SET", "k", "v"};
    const char *get[] = {"GET", "k"};
    const char *incr[] = {"INCR", "missing"};
    const char *setmax[] = {"SET", "big", "9223372036854775807"};
    const char *incrbig[] = {"INCR", "big"};
    const char *getbig[] = {"GET", "big"};
    const char *setpx[] = {"SET", "temp", "v", "PX", "1"};
    const char *gettemp[] = {"GET", "temp"};
    const char *setnx[] = {"SET", "k", "other", "NX"};

    initTestServer();
    c = createClient(-1);
    run(c, 3, set, "+OK\r\n");
    run(c, 2, get, "$1\r\nv\r\n");
    run(c, 2, incr, ":1\r\n");
    run(c, 3, setmax, "+OK\r\n");
    run(c, 2, incrbig, "-ERR increment or decrement would overflow\r\n");
    run(c, 2, getbig, "$19\r\n9223372036854775807\r\n");
    run(c, 5, setpx, "+OK\r\n");
    usleep(20000);
    run(c, 2, gettemp, "$-1\r\n");
    run(c, 4, setnx, "$-1\r\n");
    freeClient(c);
    return test_finish();
}
