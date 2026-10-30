#include "zmalloc.h"

#include <stdlib.h>
#include <string.h>

static size_t used_memory;

void *zmalloc(size_t size) {
    size_t *p;

    if (size == 0)
        size = 1;
    p = malloc(sizeof(size_t) + size);
    if (!p)
        return NULL;
    *p = size;
    used_memory += size;
    return p + 1;
}

void *zrealloc(void *ptr, size_t size) {
    size_t *p;
    size_t old;

    if (!ptr)
        return zmalloc(size);
    if (size == 0)
        size = 1;
    p = (size_t *)ptr - 1;
    old = *p;
    p = realloc(p, sizeof(size_t) + size);
    if (!p)
        return NULL;
    *p = size;
    used_memory = used_memory - old + size;
    return p + 1;
}

void zfree(void *ptr) {
    size_t *p;

    if (!ptr)
        return;
    p = (size_t *)ptr - 1;
    used_memory -= *p;
    free(p);
}

char *zstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = zmalloc(n);

    if (d)
        memcpy(d, s, n);
    return d;
}

size_t zmallocUsed(void) {
    return used_memory;
}
