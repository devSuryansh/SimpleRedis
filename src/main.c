#include "server.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, serverSignal);
    signal(SIGTERM, serverSignal);
    memset(&server, 0, sizeof(server));
    initServerConfig();
    if (argc > 2) {
        fprintf(stderr, "usage: simpleredis-server [config]\n");
        return 1;
    }
    if (argc == 2)
        loadServerConfig(argv[1]);
    initServer();
    printf("ready %d\n", server.port);
    fflush(stdout);
    aeMain(server.el);
    prepareShutdown(0, 0);
    return 0;
}
