#include "anet.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void anetSetError(char *err, const char *msg) {
    if (!err)
        return;
    snprintf(err, 256, "%s: %s", msg, strerror(errno));
}

int anetNonBlock(char *err, int fd) {
    int flags = fcntl(fd, F_GETFL);

    if (flags == -1) {
        anetSetError(err, "fcntl(F_GETFL)");
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        anetSetError(err, "fcntl(F_SETFL)");
        return -1;
    }
    return 0;
}

int anetEnableTcpNoDelay(char *err, int fd) {
    int yes = 1;

    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == -1) {
        anetSetError(err, "setsockopt TCP_NODELAY");
        return -1;
    }
    return 0;
}

int anetTcpServer(char *err, int port, const char *bindaddr, int backlog) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    struct sockaddr_in sa;

    if (fd == -1) {
        anetSetError(err, "socket");
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
        anetSetError(err, "setsockopt");
        close(fd);
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (!bindaddr || inet_pton(AF_INET, bindaddr, &sa.sin_addr) != 1) {
        if (err)
            snprintf(err, 256, "invalid bind address");
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == -1) {
        anetSetError(err, "bind");
        close(fd);
        return -1;
    }
    if (listen(fd, backlog) == -1) {
        anetSetError(err, "listen");
        close(fd);
        return -1;
    }
    return fd;
}

int anetTcpAccept(char *err, int fd, char *ip, size_t ip_len, int *port) {
    struct sockaddr_in sa;
    socklen_t slen = sizeof(sa);
    int cfd = accept(fd, (struct sockaddr *)&sa, &slen);

    if (cfd == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            anetSetError(err, "accept");
        return -1;
    }
    if (ip && ip_len)
        inet_ntop(AF_INET, &sa.sin_addr, ip, (socklen_t)ip_len);
    if (port)
        *port = ntohs(sa.sin_port);
    return cfd;
}

int anetTcpPort(int fd) {
    struct sockaddr_in sa;
    socklen_t slen = sizeof(sa);

    if (getsockname(fd, (struct sockaddr *)&sa, &slen) == -1)
        return -1;
    return ntohs(sa.sin_port);
}
