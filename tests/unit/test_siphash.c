#include "test.h"
#include "siphash.h"

int main(void) {
    unsigned char key[16];
    unsigned char msg[15];
    size_t i;

    for (i = 0; i < 16; i++)
        key[i] = (unsigned char)i;
    for (i = 0; i < 15; i++)
        msg[i] = (unsigned char)i;
    EXPECT(siphash24(key, msg, 15) == 0xa129ca6149be45e5ULL);
    return test_finish();
}
