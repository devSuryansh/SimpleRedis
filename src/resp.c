#include "resp.h"
#include "zmalloc.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static int parseI64(const char *s, size_t n, long long *out) {
    size_t i = 0;
    int neg = 0;
    unsigned long long acc = 0;

    if (n == 0)
        return -1;
    if (s[0] == '-' || s[0] == '+') {
        neg = s[0] == '-';
        i++;
        if (i == n)
            return -1;
    }
    for (; i < n; i++) {
        int digit;

        if (s[i] < '0' || s[i] > '9')
            return -1;
        digit = s[i] - '0';
        if (acc > (unsigned long long)LLONG_MAX / 10)
            return -1;
        acc = acc * 10 + (unsigned)digit;
    }
    if (neg) {
        if (acc > (unsigned long long)LLONG_MAX + 1)
            return -1;
        if (acc == (unsigned long long)LLONG_MAX + 1)
            *out = LLONG_MIN;
        else
            *out = -(long long)acc;
    } else {
        if (acc > (unsigned long long)LLONG_MAX)
            return -1;
        *out = (long long)acc;
    }
    return 0;
}

static int findLine(const char *buf, size_t len, size_t pos, const char **start,
                    size_t *linelen, size_t *next) {
    size_t i;

    for (i = pos; i < len; i++) {
        if (buf[i] != '\n')
            continue;
        if (i == pos || buf[i - 1] != '\r')
            return -1;
        *start = buf + pos;
        *linelen = i - pos - 1;
        *next = i + 1;
        return 1;
    }
    return 0;
}

static void freePartial(respVal *items, int n) {
    int i;

    for (i = 0; i < n; i++)
        respFree(&items[i]);
    zfree(items);
}

static int parseValue(const char *buf, size_t len, size_t *pos, respVal *out) {
    const char *line;
    size_t linelen;
    size_t next;
    char kind;
    int rc;
    long long count;
    int nitems;
    int i;

    memset(out, 0, sizeof(*out));
    if (*pos >= len)
        return 0;
    kind = buf[*pos];
    rc = findLine(buf, len, *pos + 1, &line, &linelen, &next);
    if (rc != 1)
        return rc;

    if (kind == '+' || kind == '-') {
        out->type = kind == '+' ? RESP_SIMPLE : RESP_ERROR;
        out->str = sdsnewlen(line, linelen);
        *pos = next;
        return 1;
    }
    if (kind == ':') {
        if (parseI64(line, linelen, &out->ival) != 0)
            return -1;
        out->type = RESP_INTEGER;
        *pos = next;
        return 1;
    }
    if (kind == '_') {
        out->type = RESP_NULL;
        *pos = next;
        return 1;
    }
    if (kind == '#') {
        if (linelen != 1 || (line[0] != 't' && line[0] != 'f'))
            return -1;
        out->type = RESP_BOOL;
        out->boolv = line[0] == 't';
        *pos = next;
        return 1;
    }
    if (kind == ',') {
        out->type = RESP_DOUBLE;
        out->str = sdsnewlen(line, linelen);
        *pos = next;
        return 1;
    }
    if (kind == '$') {
        long long bulklen;

        if (parseI64(line, linelen, &bulklen) != 0)
            return -1;
        if (bulklen < -1)
            return -1;
        if (bulklen == -1) {
            out->type = RESP_NULL;
            *pos = next;
            return 1;
        }
        if (bulklen > 64 * 1024 * 1024)
            return -1;
        if (next + (size_t)bulklen + 2 > len)
            return 0;
        if (buf[next + (size_t)bulklen] != '\r' ||
            buf[next + (size_t)bulklen + 1] != '\n')
            return -1;
        out->type = RESP_BULK;
        out->str = sdsnewlen(buf + next, (size_t)bulklen);
        *pos = next + (size_t)bulklen + 2;
        return 1;
    }
    if (kind == '*' || kind == '%' || kind == '>') {
        if (parseI64(line, linelen, &count) != 0)
            return -1;
        if (count == -1 && kind == '*') {
            out->type = RESP_NULL;
            *pos = next;
            return 1;
        }
        if (count < 0)
            return -1;
        if (kind == '%') {
            if (count > (LLONG_MAX / 2))
                return -1;
            nitems = (int)(count * 2);
            out->type = RESP_MAP;
        } else {
            nitems = (int)count;
            out->type = kind == '>' ? RESP_PUSH : RESP_ARRAY;
        }
        if (count > 1024 * 1024)
            return -1;
        *pos = next;
        out->nitems = nitems;
        if (nitems == 0)
            return 1;
        out->items = zmalloc((size_t)nitems * sizeof(respVal));
        memset(out->items, 0, (size_t)nitems * sizeof(respVal));
        for (i = 0; i < nitems; i++) {
            rc = parseValue(buf, len, pos, &out->items[i]);
            if (rc != 1) {
                respFree(&out->items[i]);
                freePartial(out->items, i);
                out->items = NULL;
                out->nitems = 0;
                return rc;
            }
        }
        return 1;
    }
    return -1;
}

int respParse(const char *buf, size_t len, size_t *consumed, respVal *out) {
    size_t pos = 0;
    int rc = parseValue(buf, len, &pos, out);

    if (rc == 1)
        *consumed = pos;
    else {
        *consumed = 0;
        if (rc == -1)
            respFree(out);
        else
            respFree(out);
    }
    return rc;
}

void respFree(respVal *v) {
    int i;

    if (!v)
        return;
    sdsfree(v->str);
    v->str = NULL;
    if (v->items) {
        for (i = 0; i < v->nitems; i++)
            respFree(&v->items[i]);
        zfree(v->items);
        v->items = NULL;
    }
    v->nitems = 0;
}

static sds catLen(sds s, char prefix, long long n) {
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%c%lld\r\n", prefix, n);

    return sdscatlen(s, buf, (size_t)len);
}

static sds encodeInto(sds s, const respVal *v, int proto) {
    int i;

    switch (v->type) {
    case RESP_SIMPLE:
        s = sdscatlen(s, "+", 1);
        s = sdscatlen(s, v->str, sdslen(v->str));
        return sdscatlen(s, "\r\n", 2);
    case RESP_ERROR:
        s = sdscatlen(s, "-", 1);
        s = sdscatlen(s, v->str, sdslen(v->str));
        return sdscatlen(s, "\r\n", 2);
    case RESP_INTEGER:
        return catLen(s, ':', v->ival);
    case RESP_NULL:
        if (proto >= 3)
            return sdscatlen(s, "_\r\n", 3);
        return sdscatlen(s, "$-1\r\n", 5);
    case RESP_BULK:
        s = catLen(s, '$', (long long)sdslen(v->str));
        s = sdscatlen(s, v->str, sdslen(v->str));
        return sdscatlen(s, "\r\n", 2);
    case RESP_BOOL:
        if (proto >= 3)
            return sdscatlen(s, v->boolv ? "#t\r\n" : "#f\r\n", 4);
        return catLen(s, ':', v->boolv ? 1 : 0);
    case RESP_DOUBLE:
        if (proto >= 3) {
            s = sdscatlen(s, ",", 1);
            s = sdscatlen(s, v->str, sdslen(v->str));
            return sdscatlen(s, "\r\n", 2);
        }
        s = catLen(s, '$', (long long)sdslen(v->str));
        s = sdscatlen(s, v->str, sdslen(v->str));
        return sdscatlen(s, "\r\n", 2);
    case RESP_ARRAY:
    case RESP_PUSH:
    case RESP_MAP:
        if (v->type == RESP_MAP && proto >= 3)
            s = catLen(s, '%', v->nitems / 2);
        else if (v->type == RESP_PUSH && proto >= 3)
            s = catLen(s, '>', v->nitems);
        else
            s = catLen(s, '*', v->nitems);
        for (i = 0; i < v->nitems; i++)
            s = encodeInto(s, &v->items[i], proto);
        return s;
    default:
        return s;
    }
}

sds respEncode(const respVal *v, int proto) {
    return encodeInto(sdsempty(), v, proto);
}
