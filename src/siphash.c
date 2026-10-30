#include "siphash.h"

static uint64_t rotl(uint64_t x, int b) {
    return (x << b) | (x >> (64 - b));
}

static uint64_t read64(const unsigned char *p) {
    uint64_t v = 0;
    int i;

    for (i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static void sipround(uint64_t *v0, uint64_t *v1, uint64_t *v2, uint64_t *v3) {
    *v0 += *v1;
    *v1 = rotl(*v1, 13);
    *v1 ^= *v0;
    *v0 = rotl(*v0, 32);
    *v2 += *v3;
    *v3 = rotl(*v3, 16);
    *v3 ^= *v2;
    *v0 += *v3;
    *v3 = rotl(*v3, 21);
    *v3 ^= *v0;
    *v2 += *v1;
    *v1 = rotl(*v1, 17);
    *v1 ^= *v2;
    *v2 = rotl(*v2, 32);
}

uint64_t siphash24(const unsigned char key[16], const void *data, size_t len) {
    const unsigned char *p = data;
    uint64_t k0 = read64(key);
    uint64_t k1 = read64(key + 8);
    uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
    uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
    uint64_t v3 = 0x7465646279746573ULL ^ k1;
    uint64_t b = ((uint64_t)len) << 56;
    size_t left = len;

    while (left >= 8) {
        uint64_t m = read64(p);

        v3 ^= m;
        sipround(&v0, &v1, &v2, &v3);
        sipround(&v0, &v1, &v2, &v3);
        v0 ^= m;
        p += 8;
        left -= 8;
    }
    if (left >= 7)
        b |= (uint64_t)p[6] << 48;
    if (left >= 6)
        b |= (uint64_t)p[5] << 40;
    if (left >= 5)
        b |= (uint64_t)p[4] << 32;
    if (left >= 4)
        b |= (uint64_t)p[3] << 24;
    if (left >= 3)
        b |= (uint64_t)p[2] << 16;
    if (left >= 2)
        b |= (uint64_t)p[1] << 8;
    if (left >= 1)
        b |= (uint64_t)p[0];
    v3 ^= b;
    sipround(&v0, &v1, &v2, &v3);
    sipround(&v0, &v1, &v2, &v3);
    v0 ^= b;
    v2 ^= 0xff;
    sipround(&v0, &v1, &v2, &v3);
    sipround(&v0, &v1, &v2, &v3);
    sipround(&v0, &v1, &v2, &v3);
    sipround(&v0, &v1, &v2, &v3);
    return v0 ^ v1 ^ v2 ^ v3;
}
