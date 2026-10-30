#include "skiplist.h"
#include "zmalloc.h"

#include <stdlib.h>

typedef struct zskiplistLevel {
    struct zskiplistNode *forward;
    unsigned int span;
} zskiplistLevel;

struct zskiplistNode {
    sds ele;
    double score;
    struct zskiplistNode *backward;
    zskiplistLevel level[];
};

struct zskiplist {
    zskiplistNode *header;
    zskiplistNode *tail;
    unsigned long length;
    int level;
};

static zskiplistNode *zslCreateNode(int level, double score, sds ele) {
    zskiplistNode *n = zmalloc(sizeof(*n) + (size_t)level * sizeof(zskiplistLevel));
    int i;

    n->score = score;
    n->ele = ele;
    n->backward = NULL;
    for (i = 0; i < level; i++) {
        n->level[i].forward = NULL;
        n->level[i].span = 0;
    }
    return n;
}

static void zslFreeNode(zskiplistNode *n) {
    if (n->ele)
        sdsfree(n->ele);
    zfree(n);
}

static int zslRandomLevel(void) {
    int level = 1;

    while ((rand() & 0xFFFF) < (int)(0.25 * 0xFFFF))
        level++;
    return level < ZSKIPLIST_MAXLEVEL ? level : ZSKIPLIST_MAXLEVEL;
}

zskiplist *zslCreate(void) {
    zskiplist *zsl = zmalloc(sizeof(*zsl));

    zsl->level = 1;
    zsl->length = 0;
    zsl->tail = NULL;
    zsl->header = zslCreateNode(ZSKIPLIST_MAXLEVEL, 0, NULL);
    return zsl;
}

void zslFree(zskiplist *zsl) {
    zskiplistNode *node = zsl->header->level[0].forward;
    zskiplistNode *next;

    zfree(zsl->header);
    while (node) {
        next = node->level[0].forward;
        zslFreeNode(node);
        node = next;
    }
    zfree(zsl);
}

zskiplistNode *zslInsert(zskiplist *zsl, double score, sds ele) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL];
    zskiplistNode *x;
    unsigned int rank[ZSKIPLIST_MAXLEVEL];
    int i;
    int level;

    x = zsl->header;
    for (i = zsl->level - 1; i >= 0; i--) {
        rank[i] = (i == zsl->level - 1) ? 0 : rank[i + 1];
        while (x->level[i].forward &&
               (x->level[i].forward->score < score ||
                (x->level[i].forward->score == score &&
                 sdscmp(x->level[i].forward->ele, ele) < 0))) {
            rank[i] += x->level[i].span;
            x = x->level[i].forward;
        }
        update[i] = x;
    }
    level = zslRandomLevel();
    if (level > zsl->level) {
        for (i = zsl->level; i < level; i++) {
            rank[i] = 0;
            update[i] = zsl->header;
            update[i]->level[i].span = (unsigned int)zsl->length;
        }
        zsl->level = level;
    }
    x = zslCreateNode(level, score, ele);
    for (i = 0; i < level; i++) {
        x->level[i].forward = update[i]->level[i].forward;
        update[i]->level[i].forward = x;
        x->level[i].span = update[i]->level[i].span - (rank[0] - rank[i]);
        update[i]->level[i].span = (rank[0] - rank[i]) + 1;
    }
    for (i = level; i < zsl->level; i++)
        update[i]->level[i].span++;
    x->backward = (update[0] == zsl->header) ? NULL : update[0];
    if (x->level[0].forward)
        x->level[0].forward->backward = x;
    else
        zsl->tail = x;
    zsl->length++;
    return x;
}

static void zslDeleteNode(zskiplist *zsl, zskiplistNode *x, zskiplistNode **update) {
    int i;

    for (i = 0; i < zsl->level; i++) {
        if (update[i]->level[i].forward == x) {
            update[i]->level[i].span += x->level[i].span - 1;
            update[i]->level[i].forward = x->level[i].forward;
        } else {
            update[i]->level[i].span--;
        }
    }
    if (x->level[0].forward)
        x->level[0].forward->backward = x->backward;
    else
        zsl->tail = x->backward;
    while (zsl->level > 1 && zsl->header->level[zsl->level - 1].forward == NULL)
        zsl->level--;
    zsl->length--;
}

int zslDelete(zskiplist *zsl, double score, sds ele, zskiplistNode **node) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL];
    zskiplistNode *x;
    int i;

    x = zsl->header;
    for (i = zsl->level - 1; i >= 0; i--) {
        while (x->level[i].forward &&
               (x->level[i].forward->score < score ||
                (x->level[i].forward->score == score &&
                 sdscmp(x->level[i].forward->ele, ele) < 0)))
            x = x->level[i].forward;
        update[i] = x;
    }
    x = x->level[0].forward;
    if (x && score == x->score && sdscmp(x->ele, ele) == 0) {
        zslDeleteNode(zsl, x, update);
        if (node)
            *node = x;
        else
            zslFreeNode(x);
        return 1;
    }
    return 0;
}

unsigned long zslGetRank(zskiplist *zsl, double score, sds ele) {
    zskiplistNode *x = zsl->header;
    unsigned long rank = 0;
    int i;

    for (i = zsl->level - 1; i >= 0; i--) {
        while (x->level[i].forward &&
               (x->level[i].forward->score < score ||
                (x->level[i].forward->score == score &&
                 sdscmp(x->level[i].forward->ele, ele) <= 0))) {
            rank += x->level[i].span;
            x = x->level[i].forward;
        }
        if (x->ele && sdscmp(x->ele, ele) == 0)
            return rank;
    }
    return 0;
}

zskiplistNode *zslGetElementByRank(zskiplist *zsl, unsigned long rank) {
    zskiplistNode *x = zsl->header;
    unsigned long traversed = 0;
    int i;

    for (i = zsl->level - 1; i >= 0; i--) {
        while (x->level[i].forward &&
               traversed + x->level[i].span <= rank) {
            traversed += x->level[i].span;
            x = x->level[i].forward;
        }
        if (traversed == rank && x != zsl->header)
            return x;
    }
    return NULL;
}

sds zslNodeMember(const zskiplistNode *node) {
    return node->ele;
}

double zslNodeScore(const zskiplistNode *node) {
    return node->score;
}

unsigned long zslLength(const zskiplist *zsl) {
    return zsl->length;
}
