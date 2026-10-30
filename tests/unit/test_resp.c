#include "test.h"
#include "resp.h"
#include "sds.h"
#include "zmalloc.h"

#include <string.h>

static void expectNull(int proto, const char *bytes) {
    respVal v;
    sds enc;

    memset(&v, 0, sizeof(v));
    v.type = RESP_NULL;
    enc = respEncode(&v, proto);
    EXPECT(strcmp(enc, bytes) == 0);
    sdsfree(enc);
}

int main(void) {
    const char *full = "*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n";
    size_t n = strlen(full);
    char part[64];
    respVal v;
    size_t consumed = 99;
    int rc;
    sds enc;

    memcpy(part, full, 8);
    rc = respParse(part, 8, &consumed, &v);
    EXPECT(rc == 0);
    EXPECT(consumed == 0);

    consumed = 0;
    rc = respParse(full, n, &consumed, &v);
    EXPECT(rc == 1);
    EXPECT(consumed == n);
    EXPECT(v.type == RESP_ARRAY);
    EXPECT(v.nitems == 2);
    EXPECT(v.items[0].type == RESP_BULK);
    EXPECT(sdslen(v.items[0].str) == 3);
    EXPECT(memcmp(v.items[0].str, "GET", 3) == 0);
    EXPECT(memcmp(v.items[1].str, "key", 3) == 0);
    respFree(&v);

    expectNull(2, "$-1\r\n");
    expectNull(3, "_\r\n");

    memset(&v, 0, sizeof(v));
    v.type = RESP_MAP;
    v.nitems = 2;
    v.items = zmalloc(2 * sizeof(respVal));
    memset(v.items, 0, 2 * sizeof(respVal));
    v.items[0].type = RESP_BULK;
    v.items[0].str = sdsnew("a");
    v.items[1].type = RESP_BULK;
    v.items[1].str = sdsnew("b");
    enc = respEncode(&v, 3);
    EXPECT(strncmp(enc, "%1\r\n", 4) == 0);
    sdsfree(enc);
    respFree(&v);

    rc = respParse("$5\r\nab", 6, &consumed, &v);
    EXPECT(rc == 0);
    rc = respParse("$1\r\nX\n\n", 7, &consumed, &v);
    EXPECT(rc == -1);
    return test_finish();
}
