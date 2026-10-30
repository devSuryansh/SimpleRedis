#include "server.h"
#include "zmalloc.h"

#include <string.h>

robj *createObject(int type, void *ptr) {
    robj *o = zmalloc(sizeof(*o));

    o->type = (unsigned)type;
    o->refcount = 1;
    o->ptr = ptr;
    return o;
}

robj *createStringObject(const char *ptr, size_t len) {
    return createObject(OBJ_STRING, sdsnewlen(ptr, len));
}

robj *dupStringObject(robj *o) {
    sds s = o->ptr;
    return createStringObject(s, sdslen(s));
}

void incrRefCount(robj *o) {
    o->refcount++;
}

sds objectSds(robj *o) {
    return o->ptr;
}

void decrRefCount(robj *o) {
    if (!o)
        return;
    if (o->refcount > 1) {
        o->refcount--;
        return;
    }
    if (o->type == OBJ_STRING)
        sdsfree(o->ptr);
    zfree(o);
}
