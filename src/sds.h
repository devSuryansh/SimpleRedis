#ifndef SDS_H
#define SDS_H

#include <stddef.h>
#include <stdint.h>

typedef char *sds;

typedef struct sdshdr {
    uint32_t len;
    uint32_t alloc;
    char buf[];
} sdshdr;

static inline size_t sdslen(const sds s) {
    sdshdr *h = (sdshdr *)((char *)s - sizeof(sdshdr));
    return h->len;
}

static inline size_t sdsavail(const sds s) {
    sdshdr *h = (sdshdr *)((char *)s - sizeof(sdshdr));
    return h->alloc - h->len;
}

sds sdsnewlen(const void *init, size_t len);
sds sdsnew(const char *init);
sds sdsempty(void);
sds sdsdup(const sds s);
void sdsfree(sds s);
sds sdscatlen(sds s, const void *t, size_t len);
sds sdscpy(sds s, const char *t);
int sdscmp(const sds a, const sds b);
sds sdsfromlonglong(long long value);

#endif
