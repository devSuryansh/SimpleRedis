#include "test.h"
#include "skiplist.h"
#include "sds.h"

int main(void) {
    zskiplist *zsl = zslCreate();
    sds a = sdsnew("a");
    sds b = sdsnew("b");
    sds c = sdsnew("c");

    zslInsert(zsl, 1, sdsnew("b"));
    zslInsert(zsl, 1, sdsnew("a"));
    zslInsert(zsl, 3, sdsnew("c"));
    EXPECT(zslGetRank(zsl, 1, a) == 1);
    EXPECT(zslGetRank(zsl, 1, b) == 2);
    EXPECT(zslGetRank(zsl, 3, c) == 3);
    EXPECT(zslDelete(zsl, 1, b, NULL) == 1);
    EXPECT(zslGetRank(zsl, 3, c) == 2);
    EXPECT(zslGetRank(zsl, 1, b) == 0);
    sdsfree(a);
    sdsfree(b);
    sdsfree(c);
    zslFree(zsl);
    return test_finish();
}
