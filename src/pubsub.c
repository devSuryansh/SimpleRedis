#include "server.h"
#include "zmalloc.h"

#include <string.h>

unsigned int pubsubHash(dictNode *node) {
    pubsubChannel *ch = (pubsubChannel *)node;
    return keyHash(ch->name, sdslen(ch->name));
}

int pubsubCmp(dictNode *node, const void *key, size_t keylen) {
    pubsubChannel *ch = (pubsubChannel *)node;

    if (sdslen(ch->name) != keylen)
        return 0;
    return memcmp(ch->name, key, keylen) == 0;
}

void pubsubFree(dictNode *node) {
    pubsubChannel *ch = (pubsubChannel *)node;

    sdsfree(ch->name);
    listRelease(ch->clients);
    zfree(ch);
}

static pubsubChannel *findChannel(sds name) {
    if (!server.pubsub_channels)
        return NULL;
    return (pubsubChannel *)dictFind(server.pubsub_channels, name, sdslen(name),
                                     keyHash(name, sdslen(name)));
}

int clientSubscriptions(client *c) {
    if (!c->pubsub_channels)
        return 0;
    return (int)listLength(c->pubsub_channels);
}

static int clientHasChannel(client *c, sds name) {
    listIter *it = listGetIterator(c->pubsub_channels, AL_START_HEAD);
    listNode *ln;
    int found = 0;

    while ((ln = listNext(it)) != NULL) {
        sds cur = listNodeValue(ln);
        if (sdslen(cur) == sdslen(name) && memcmp(cur, name, sdslen(name)) == 0)
            found = 1;
    }
    listReleaseIterator(it);
    return found;
}

static void replySub(client *c, const char *kind, sds channel) {
    addReplyPushLen(c, 3);
    addReplyBulk(c, kind, strlen(kind));
    if (channel)
        addReplyBulkSds(c, channel);
    else
        addReplyNull(c);
    addReplyLongLong(c, clientSubscriptions(c));
}

static void subscribeOne(client *c, sds name) {
    pubsubChannel *ch = findChannel(name);

    if (!ch) {
        ch = zmalloc(sizeof(*ch));
        ch->name = sdsdup(name);
        ch->clients = listCreate();
        dictAdd(server.pubsub_channels, &ch->node);
    }
    if (!clientHasChannel(c, name)) {
        listAddNodeTail(ch->clients, c);
        listAddNodeTail(c->pubsub_channels, sdsdup(name));
    }
    c->flags |= CLIENT_PUBSUB;
    replySub(c, "subscribe", name);
}

static void unsubscribeOne(client *c, sds name) {
    sds owned = sdsdup(name);
    pubsubChannel *ch = findChannel(owned);
    listIter *it;
    listNode *ln;

    if (ch) {
        it = listGetIterator(ch->clients, AL_START_HEAD);
        while ((ln = listNext(it)) != NULL) {
            if (listNodeValue(ln) == c) {
                listDelNode(ch->clients, ln);
                break;
            }
        }
        listReleaseIterator(it);
        if (listLength(ch->clients) == 0) {
            dictDelete(server.pubsub_channels, name, sdslen(name), keyHash(name, sdslen(name)));
            sdsfree(ch->name);
            listRelease(ch->clients);
            zfree(ch);
        }
    }
    it = listGetIterator(c->pubsub_channels, AL_START_HEAD);
    while ((ln = listNext(it)) != NULL) {
        sds cur = listNodeValue(ln);
        if (sdslen(cur) == sdslen(name) && memcmp(cur, name, sdslen(name)) == 0) {
            listDelNode(c->pubsub_channels, ln);
            break;
        }
    }
    listReleaseIterator(it);
    if (listLength(c->pubsub_channels) == 0)
        c->flags &= ~CLIENT_PUBSUB;
    replySub(c, "unsubscribe", owned);
    sdsfree(owned);
}

void pubsubUnsubscribeAll(client *c) {
    if (!c->pubsub_channels)
        return;
    while (listLength(c->pubsub_channels) > 0) {
        sds name = sdsdup(listNodeValue(listFirst(c->pubsub_channels)));
        listIter *it;
        listNode *ln;
        pubsubChannel *ch = findChannel(name);

        if (ch) {
            it = listGetIterator(ch->clients, AL_START_HEAD);
            while ((ln = listNext(it)) != NULL) {
                if (listNodeValue(ln) == c) {
                    listDelNode(ch->clients, ln);
                    break;
                }
            }
            listReleaseIterator(it);
            if (listLength(ch->clients) == 0) {
                dictDelete(server.pubsub_channels, name, sdslen(name), keyHash(name, sdslen(name)));
                sdsfree(ch->name);
                listRelease(ch->clients);
                zfree(ch);
            }
        }
        it = listGetIterator(c->pubsub_channels, AL_START_HEAD);
        ln = listNext(it);
        if (ln)
            listDelNode(c->pubsub_channels, ln);
        listReleaseIterator(it);
        sdsfree(name);
    }
    c->flags &= ~CLIENT_PUBSUB;
}

void subscribeCommand(client *c) {
    int i;

    for (i = 1; i < c->argc; i++)
        subscribeOne(c, objectSds(c->argv[i]));
}

void unsubscribeCommand(client *c) {
    int i;

    if (c->argc == 1) {
        if (listLength(c->pubsub_channels) == 0) {
            replySub(c, "unsubscribe", NULL);
            return;
        }
        while (listLength(c->pubsub_channels) > 0)
            unsubscribeOne(c, listNodeValue(listFirst(c->pubsub_channels)));
        return;
    }
    for (i = 1; i < c->argc; i++)
        unsubscribeOne(c, objectSds(c->argv[i]));
}

void publishCommand(client *c) {
    sds name = objectSds(c->argv[1]);
    sds msg = objectSds(c->argv[2]);
    pubsubChannel *ch = findChannel(name);
    int n = 0;

    if (ch) {
        listIter *it = listGetIterator(ch->clients, AL_START_HEAD);
        listNode *ln;

        while ((ln = listNext(it)) != NULL) {
            client *sub = listNodeValue(ln);

            addReplyPushLen(sub, 3);
            addReplyBulk(sub, "message", 7);
            addReplyBulkSds(sub, name);
            addReplyBulkSds(sub, msg);
            n++;
            if (sub->fd >= 0 && server.el)
                aeCreateFileEvent(server.el, sub->fd, AE_WRITABLE, sendReplyToClient, sub);
        }
        listReleaseIterator(it);
    }
    addReplyLongLong(c, n);
}

void initPubsubCommands(void) {
    registerCommand("subscribe", subscribeCommand, NULL, -2, CMD_READONLY, 0, 0, 0);
    registerCommand("unsubscribe", unsubscribeCommand, NULL, -1, CMD_READONLY, 0, 0, 0);
    registerCommand("publish", publishCommand, NULL, 3, CMD_READONLY, 0, 0, 0);
}
