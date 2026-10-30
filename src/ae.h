#ifndef AE_H
#define AE_H

#define AE_OK 0
#define AE_ERR -1
#define AE_NONE 0
#define AE_READABLE 1
#define AE_WRITABLE 2
#define AE_NOMORE -1

struct aeEventLoop;
typedef void (*aeFileProc)(struct aeEventLoop *el, int fd, void *clientData, int mask);
typedef int (*aeTimeProc)(struct aeEventLoop *el, long long id, void *clientData);

typedef struct aeEventLoop aeEventLoop;

aeEventLoop *aeCreateEventLoop(int setsize);
void aeDeleteEventLoop(aeEventLoop *el);
int aeCreateFileEvent(aeEventLoop *el, int fd, int mask, aeFileProc proc, void *clientData);
void aeDeleteFileEvent(aeEventLoop *el, int fd, int mask);
long long aeCreateTimeEvent(aeEventLoop *el, long long milliseconds, aeTimeProc proc,
                            void *clientData);
int aeProcessEvents(aeEventLoop *el);
void aeMain(aeEventLoop *el);
void aeStop(aeEventLoop *el);

#endif
