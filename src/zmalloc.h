#ifndef ZMALLOC_H
#define ZMALLOC_H

#include <stddef.h>

void *zmalloc(size_t size);
void *zrealloc(void *ptr, size_t size);
void zfree(void *ptr);
char *zstrdup(const char *s);
size_t zmallocUsed(void);

#endif
