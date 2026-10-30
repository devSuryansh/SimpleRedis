#include "ae.h"
#include "util.h"
#include "zmalloc.h"

#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

typedef struct aeFileEvent {
    int mask;
    aeFileProc rfileProc;
    aeFileProc wfileProc;
    void *clientData;
} aeFileEvent;

typedef struct aeTimeEvent {
    long long id;
    long long when_ms;
    aeTimeProc timeProc;
    void *clientData;
    struct aeTimeEvent *next;
} aeTimeEvent;

struct aeEventLoop {
    int setsize;
    int stop;
    int epfd;
    long long timeEventNextId;
    aeFileEvent *events;
    aeTimeEvent *timeEventHead;
};

int aeApiCreate(aeEventLoop *el) {
    el->epfd = epoll_create1(EPOLL_CLOEXEC);
    return el->epfd < 0 ? AE_ERR : AE_OK;
}

static int aeApiAdd(aeEventLoop *el, int fd, int mask, int oldmask) {
    struct epoll_event ee;
    int op = oldmask == AE_NONE ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;

    ee.events = 0;
    ee.data.fd = fd;
    if (mask & AE_READABLE)
        ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE)
        ee.events |= EPOLLOUT;
    if (epoll_ctl(el->epfd, op, fd, &ee) == -1)
        return AE_ERR;
    return AE_OK;
}

static void aeApiDel(aeEventLoop *el, int fd) {
    epoll_ctl(el->epfd, EPOLL_CTL_DEL, fd, NULL);
}

aeEventLoop *aeCreateEventLoop(int setsize) {
    aeEventLoop *el = zmalloc(sizeof(*el));

    el->setsize = setsize;
    el->stop = 0;
    el->timeEventNextId = 0;
    el->timeEventHead = NULL;
    el->events = zmalloc((size_t)setsize * sizeof(aeFileEvent));
    memset(el->events, 0, (size_t)setsize * sizeof(aeFileEvent));
    if (aeApiCreate(el) == AE_ERR) {
        zfree(el->events);
        zfree(el);
        return NULL;
    }
    return el;
}

void aeDeleteEventLoop(aeEventLoop *el) {
    aeTimeEvent *te = el->timeEventHead;

    while (te) {
        aeTimeEvent *next = te->next;
        zfree(te);
        te = next;
    }
    close(el->epfd);
    zfree(el->events);
    zfree(el);
}

int aeCreateFileEvent(aeEventLoop *el, int fd, int mask, aeFileProc proc, void *clientData) {
    int oldmask;

    if (fd < 0 || fd >= el->setsize)
        return AE_ERR;
    oldmask = el->events[fd].mask;
    if (aeApiAdd(el, fd, oldmask | mask, oldmask) == AE_ERR)
        return AE_ERR;
    el->events[fd].mask |= mask;
    if (mask & AE_READABLE)
        el->events[fd].rfileProc = proc;
    if (mask & AE_WRITABLE)
        el->events[fd].wfileProc = proc;
    el->events[fd].clientData = clientData;
    return AE_OK;
}

void aeDeleteFileEvent(aeEventLoop *el, int fd, int mask) {
    int oldmask;

    if (fd < 0 || fd >= el->setsize)
        return;
    oldmask = el->events[fd].mask;
    if (!(oldmask & mask))
        return;
    el->events[fd].mask = oldmask & ~mask;
    if (el->events[fd].mask == AE_NONE)
        aeApiDel(el, fd);
    else
        aeApiAdd(el, fd, el->events[fd].mask, oldmask);
}

long long aeCreateTimeEvent(aeEventLoop *el, long long milliseconds, aeTimeProc proc,
                            void *clientData) {
    aeTimeEvent *te = zmalloc(sizeof(*te));

    te->id = el->timeEventNextId++;
    te->when_ms = monotime() + milliseconds;
    te->timeProc = proc;
    te->clientData = clientData;
    te->next = el->timeEventHead;
    el->timeEventHead = te;
    return te->id;
}

static int aeSoonestTimer(aeEventLoop *el, long long now) {
    aeTimeEvent *te = el->timeEventHead;
    long long soonest = -1;

    while (te) {
        if (soonest == -1 || te->when_ms < soonest)
            soonest = te->when_ms;
        te = te->next;
    }
    if (soonest == -1)
        return -1;
    if (soonest <= now)
        return 0;
    if (soonest - now > 1000000)
        return 1000000;
    return (int)(soonest - now);
}

static void aeProcessTimeEvents(aeEventLoop *el) {
    long long now = monotime();
    aeTimeEvent *te = el->timeEventHead;
    aeTimeEvent *prev = NULL;

    while (te) {
        aeTimeEvent *next = te->next;
        int retval;

        if (te->when_ms > now) {
            prev = te;
            te = next;
            continue;
        }
        retval = te->timeProc(el, te->id, te->clientData);
        if (retval == AE_NOMORE) {
            if (prev)
                prev->next = next;
            else
                el->timeEventHead = next;
            zfree(te);
        } else {
            te->when_ms = monotime() + retval;
            prev = te;
        }
        te = next;
    }
}

int aeProcessEvents(aeEventLoop *el) {
    struct epoll_event fired[128];
    int timeout = aeSoonestTimer(el, monotime());
    int n;
    int i;

    n = epoll_wait(el->epfd, fired, 128, timeout);
    for (i = 0; i < n; i++) {
        int fd = fired[i].data.fd;
        int mask = el->events[fd].mask;
        int ev = 0;

        if (fd < 0 || fd >= el->setsize)
            continue;
        if ((fired[i].events & EPOLLIN) && (mask & AE_READABLE))
            ev |= AE_READABLE;
        if ((fired[i].events & EPOLLOUT) && (mask & AE_WRITABLE))
            ev |= AE_WRITABLE;
        if ((fired[i].events & (EPOLLERR | EPOLLHUP)) && ev == 0)
            ev |= AE_READABLE;
        if ((ev & AE_READABLE) && el->events[fd].rfileProc)
            el->events[fd].rfileProc(el, fd, el->events[fd].clientData, ev);
        if ((ev & AE_WRITABLE) && (el->events[fd].mask & AE_WRITABLE) &&
            el->events[fd].wfileProc)
            el->events[fd].wfileProc(el, fd, el->events[fd].clientData, ev);
    }
    aeProcessTimeEvents(el);
    return n;
}

void aeMain(aeEventLoop *el) {
    el->stop = 0;
    while (!el->stop)
        aeProcessEvents(el);
}

void aeStop(aeEventLoop *el) {
    el->stop = 1;
}
