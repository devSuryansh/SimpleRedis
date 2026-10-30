#ifndef SIMPLEREDIS_TEST_H
#define SIMPLEREDIS_TEST_H

#include <stdio.h>

static int tests_run;
static int tests_failed;

static void test_fail(const char *file, int line, const char *msg) {
    fprintf(stderr, "FAIL %s:%d %s\n", file, line, msg);
    tests_failed++;
}

#define EXPECT(cond) do { \
    tests_run++; \
    if (!(cond)) \
        test_fail(__FILE__, __LINE__, #cond); \
} while (0)

static int test_finish(void) {
    printf("%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}

#endif
