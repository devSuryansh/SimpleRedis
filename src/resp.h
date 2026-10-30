#ifndef RESP_H
#define RESP_H

#include "sds.h"

#define RESP_SIMPLE 1
#define RESP_ERROR 2
#define RESP_INTEGER 3
#define RESP_BULK 4
#define RESP_NULL 5
#define RESP_ARRAY 6
#define RESP_MAP 7
#define RESP_BOOL 8
#define RESP_DOUBLE 9
#define RESP_PUSH 10

typedef struct respVal {
    int type;
    long long ival;
    int boolv;
    sds str;
    struct respVal *items;
    int nitems;
} respVal;

/* 1 complete, 0 need more (consumed set to 0), -1 protocol error. */
int respParse(const char *buf, size_t len, size_t *consumed, respVal *out);
sds respEncode(const respVal *v, int proto);
void respFree(respVal *v);

#endif
