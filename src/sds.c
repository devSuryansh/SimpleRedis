#include "sds.h"
#include "zmalloc.h"

#include <stdio.h>
#include <string.h>

static sdshdr *sdsHdr(sds s) {
    return (sdshdr *)(s - sizeof(sdshdr));
}

sds sdsnewlen(const void *init, size_t len) {
    sdshdr *h;

    if (len > UINT32_MAX)
        return NULL;
    h = zmalloc(sizeof(sdshdr) + len + 1);
    if (!h)
        return NULL;
    h->len = (uint32_t)len;
    h->alloc = (uint32_t)len;
    if (len && init)
        memcpy(h->buf, init, len);
    else if (len)
        memset(h->buf, 0, len);
    h->buf[len] = '\0';
    return h->buf;
}

sds sdsnew(const char *init) {
    if (!init)
        return sdsempty();
    return sdsnewlen(init, strlen(init));
}

sds sdsempty(void) {
    return sdsnewlen("", 0);
}

sds sdsdup(const sds s) {
    return sdsnewlen(s, sdslen(s));
}

void sdsfree(sds s) {
    if (s)
        zfree(sdsHdr(s));
}

static sds sdsMakeRoom(sds s, size_t add) {
    sdshdr *h = sdsHdr(s);
    size_t avail = h->alloc - h->len;
    size_t need;
    size_t neu;

    if (avail >= add)
        return s;
    need = (size_t)h->len + add;
    if (need > UINT32_MAX)
        return NULL;
    neu = h->alloc < (UINT32_MAX / 2) ? (size_t)h->alloc * 2 : UINT32_MAX;
    if (neu < need)
        neu = need;
    h = zrealloc(h, sizeof(sdshdr) + neu + 1);
    if (!h)
        return NULL;
    h->alloc = (uint32_t)neu;
    return h->buf;
}

sds sdscatlen(sds s, const void *t, size_t len) {
    size_t cur;

    s = sdsMakeRoom(s, len);
    if (!s)
        return NULL;
    cur = sdslen(s);
    memcpy(s + cur, t, len);
    sdsHdr(s)->len = (uint32_t)(cur + len);
    s[cur + len] = '\0';
    return s;
}

sds sdscpy(sds s, const char *t) {
    size_t len = strlen(t);
    sdshdr *h;

    s = sdsMakeRoom(s, len > sdslen(s) ? len - sdslen(s) : 0);
    if (!s)
        return NULL;
    memcpy(s, t, len);
    h = sdsHdr(s);
    h->len = (uint32_t)len;
    s[len] = '\0';
    return s;
}

int sdscmp(const sds a, const sds b) {
    size_t la = sdslen(a);
    size_t lb = sdslen(b);
    size_t n = la < lb ? la : lb;
    int cmp = memcmp(a, b, n);

    if (cmp)
        return cmp;
    if (la < lb)
        return -1;
    if (la > lb)
        return 1;
    return 0;
}

void sdsDiscard(sds s, size_t n) {
    size_t len = sdslen(s);

    if (n > len)
        n = len;
    memmove(s, s + n, len - n + 1);
    sdsHdr(s)->len = (uint32_t)(len - n);
}

sds sdsfromlonglong(long long value) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", value);

    if (n < 0)
        return NULL;
    return sdsnewlen(buf, (size_t)n);
}
