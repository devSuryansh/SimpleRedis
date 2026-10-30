#ifndef SIPHASH_H
#define SIPHASH_H

#include <stddef.h>
#include <stdint.h>

uint64_t siphash24(const unsigned char key[16], const void *data, size_t len);

#endif
