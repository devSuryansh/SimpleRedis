#include "test.h"
#include "server.h"

#include <string.h>

int main(void) {
    client *c;
    const char *ping[] = {"PING"};
    const char *pingarg[] = {"PING", "hi"};
    const char *echo[] = {"ECHO", "hi"};

    initTestServer();
    c = createClient(-1);
    execCommand(c, 1, ping);
    EXPECT(strcmp(c->reply, "+PONG\r\n") == 0);
    execCommand(c, 2, pingarg);
    EXPECT(strcmp(c->reply, "$2\r\nhi\r\n") == 0);
    execCommand(c, 2, echo);
    EXPECT(strcmp(c->reply, "$2\r\nhi\r\n") == 0);
    freeClient(c);
    return test_finish();
}
