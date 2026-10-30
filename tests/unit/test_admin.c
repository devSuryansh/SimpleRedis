#include "test.h"
#include "server.h"

#include <string.h>

static void run(client *c, int argc, const char **argv, const char *expect) {
    execCommand(c, argc, argv);
    EXPECT(strcmp(c->reply, expect) == 0);
}

int main(void) {
    client *c;
    const char *hello[] = {"HELLO", "3"};
    const char *setinfo[] = {"CLIENT", "SETINFO", "LIB-NAME", "py"};
    const char *count[] = {"COMMAND", "COUNT"};
    const char *auth[] = {"AUTH", "secret"};
    const char *setbig[] = {"SET", "big", "12345678"};

    initTestServer();
    c = createClient(-1);
    execCommand(c, 2, hello);
    EXPECT(strstr(c->reply, "proto") != NULL);
    EXPECT(strstr(c->reply, ":3\r\n") != NULL);
    c->proto = 2;
    run(c, 4, setinfo, "+OK\r\n");
    execCommand(c, 2, count);
    EXPECT(c->reply[0] == ':');
    EXPECT(strcmp(c->reply, ":0\r\n") != 0);
    run(c, 2, auth, "-ERR AUTH called without any password configured for the default user\r\n");
    server.maxmemory = 1;
    run(c, 3, setbig, "-OOM command not allowed when used memory > 'maxmemory'\r\n");
    freeClient(c);
    return test_finish();
}
