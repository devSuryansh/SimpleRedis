#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

long long mstime(void);
long long ustime(void);
long long monotime(void);
int string2ll(const char *s, size_t slen, long long *value);
int string2d(const char *s, size_t slen, double *value);
int d2string(char *buf, size_t len, double value);
int stringmatch(const char *pat, size_t plen, const char *s, size_t slen);

#endif
