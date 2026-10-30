#ifndef ANET_H
#define ANET_H

#include <stddef.h>

int anetTcpServer(char *err, int port, const char *bindaddr, int backlog);
int anetTcpAccept(char *err, int fd, char *ip, size_t ip_len, int *port);
int anetNonBlock(char *err, int fd);
int anetEnableTcpNoDelay(char *err, int fd);
int anetTcpPort(int fd);

#endif
