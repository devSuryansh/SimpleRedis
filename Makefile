CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE -Isrc -Itests/unit -g -O1
LDFLAGS ?= -lpthread

.PHONY: all test clean

UNIT_TESTS := tests/unit/test_harness

all: $(UNIT_TESTS)

tests/unit/test_harness: tests/unit/test_harness.c tests/unit/test.h
	$(CC) $(CFLAGS) -o $@ tests/unit/test_harness.c

test: $(UNIT_TESTS)
	@fail=0; \
	for t in $(UNIT_TESTS); do \
		./$$t || fail=1; \
	done; \
	exit $$fail

clean:
	rm -f $(UNIT_TESTS) src/simpleredis-server src/*.o
