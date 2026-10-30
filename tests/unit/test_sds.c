#include "test.h"
#include "sds.h"
#include "zmalloc.h"

#include <string.h>

int main(void) {
    size_t before;
    sds s, other, num, bin;

    before = zmallocUsed();
    s = sdsnew("hi");
    EXPECT(sdslen(s) == 2);
    other = sdsnew("hi");
    EXPECT(sdscmp(s, other) == 0);
    sdsfree(other);
    s = sdscatlen(s, "!", 1);
    EXPECT(sdslen(s) == 3);
    EXPECT(memcmp(s, "hi!", 3) == 0);
    num = sdsfromlonglong(-12);
    EXPECT(sdscmp(num, other = sdsnew("-12")) == 0);
    sdsfree(other);
    bin = sdsnewlen("a\0b", 3);
    EXPECT(sdslen(bin) == 3);
    EXPECT(bin[1] == '\0');
    sdsfree(s);
    sdsfree(num);
    sdsfree(bin);
    EXPECT(zmallocUsed() == before);
    return test_finish();
}
