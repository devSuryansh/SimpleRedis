#include "util.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long long timespecMs(clockid_t clock_id, int micros) {
    struct timespec ts;

    clock_gettime(clock_id, &ts);
    if (micros)
        return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

long long mstime(void) {
    return timespecMs(CLOCK_REALTIME, 0);
}

long long ustime(void) {
    return timespecMs(CLOCK_REALTIME, 1);
}

long long monotime(void) {
    return timespecMs(CLOCK_MONOTONIC, 0);
}

int string2ll(const char *s, size_t slen, long long *value) {
    size_t i = 0;
    int neg = 0;
    unsigned long long acc = 0;

    if (slen == 0)
        return 0;
    if (s[0] == '-' || s[0] == '+') {
        neg = s[0] == '-';
        i++;
        if (i == slen)
            return 0;
    }
    if (s[i] == '0' && i + 1 != slen)
        return 0;
    for (; i < slen; i++) {
        int digit;

        if (s[i] < '0' || s[i] > '9')
            return 0;
        digit = s[i] - '0';
        if (acc > (unsigned long long)LLONG_MAX / 10)
            return 0;
        acc = acc * 10 + (unsigned)digit;
    }
    if (!neg && acc > (unsigned long long)LLONG_MAX)
        return 0;
    if (neg && acc > (unsigned long long)LLONG_MAX + 1)
        return 0;
    if (neg && acc == (unsigned long long)LLONG_MAX + 1)
        *value = LLONG_MIN;
    else
        *value = neg ? -(long long)acc : (long long)acc;
    return 1;
}

int string2d(const char *s, size_t slen, double *value) {
    char buf[128];
    char *end = NULL;

    if (slen == 0 || slen >= sizeof(buf))
        return 0;
    memcpy(buf, s, slen);
    buf[slen] = '\0';
    *value = strtod(buf, &end);
    if (end != buf + slen)
        return 0;
    return 1;
}

int d2string(char *buf, size_t len, double value) {
    int n = snprintf(buf, len, "%.17g", value);

    if (n < 0 || (size_t)n >= len)
        return 0;
    return n;
}

int stringmatch(const char *pat, size_t plen, const char *s, size_t slen) {
    while (plen && slen) {
        if (pat[0] == '*') {
            while (plen && pat[0] == '*') {
                pat++;
                plen--;
            }
            if (plen == 0)
                return 1;
            while (slen) {
                if (stringmatch(pat, plen, s, slen))
                    return 1;
                s++;
                slen--;
            }
            return 0;
        }
        if (pat[0] == '?' || pat[0] == s[0]) {
            pat++;
            plen--;
            s++;
            slen--;
            continue;
        }
        return 0;
    }
    while (plen && pat[0] == '*') {
        pat++;
        plen--;
    }
    return plen == 0 && slen == 0;
}
