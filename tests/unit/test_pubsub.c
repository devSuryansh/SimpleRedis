#include "test.h"
#include "server.h"

#include <string.h>

int main(void) {
    client *a;
    client *b;
    client *pub;
    const char *sub[] = {"SUBSCRIBE", "ch"};
    const char *publish[] = {"PUBLISH", "ch", "hi"};
    const char *get[] = {"GET", "x"};

    initTestServer();
    a = createClient(-1);
    b = createClient(-1);
    pub = createClient(-1);
    a->proto = 3;
    execCommand(a, 2, sub);
    EXPECT(a->reply[0] == '>');
    EXPECT(strstr(a->reply, "subscribe") != NULL);
    b->proto = 2;
    execCommand(b, 2, sub);
    EXPECT(b->reply[0] == '*');
    execCommand(pub, 3, publish);
    EXPECT(strcmp(pub->reply, ":2\r\n") == 0);
    EXPECT(strstr(a->reply, "message") != NULL);
    EXPECT(strstr(a->reply, "hi") != NULL);
    EXPECT(strstr(b->reply, "message") != NULL);
    EXPECT(strstr(b->reply, "hi") != NULL);
    execCommand(a, 2, get);
    EXPECT(strstr(a->reply, "only SUBSCRIBE / UNSUBSCRIBE / PING / QUIT") != NULL);
    freeClient(a);
    freeClient(b);
    freeClient(pub);
    return test_finish();
}
