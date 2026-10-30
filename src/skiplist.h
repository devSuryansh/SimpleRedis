#ifndef SKIPLIST_H
#define SKIPLIST_H

#include "sds.h"

#define ZSKIPLIST_MAXLEVEL 32

typedef struct zskiplistNode zskiplistNode;
typedef struct zskiplist zskiplist;

zskiplist *zslCreate(void);
void zslFree(zskiplist *zsl);
zskiplistNode *zslInsert(zskiplist *zsl, double score, sds member);
int zslDelete(zskiplist *zsl, double score, sds member, zskiplistNode **node);
unsigned long zslGetRank(zskiplist *zsl, double score, sds member);
zskiplistNode *zslGetElementByRank(zskiplist *zsl, unsigned long rank);
sds zslNodeMember(const zskiplistNode *node);
double zslNodeScore(const zskiplistNode *node);
unsigned long zslLength(const zskiplist *zsl);

#endif
