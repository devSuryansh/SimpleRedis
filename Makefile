CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE -Isrc -Itests/unit -g -O1
LDFLAGS ?= -lpthread

.PHONY: all test clean

UNIT_TESTS := tests/unit/test_harness tests/unit/test_sds tests/unit/test_siphash \
	tests/unit/test_dict tests/unit/test_adlist tests/unit/test_skiplist \
	tests/unit/test_pq tests/unit/test_resp tests/unit/test_ping

all: src/simpleredis-server $(UNIT_TESTS)

tests/unit/test_harness: tests/unit/test_harness.c tests/unit/test.h
	$(CC) $(CFLAGS) -o $@ tests/unit/test_harness.c

tests/unit/test_sds: tests/unit/test_sds.c src/sds.c src/zmalloc.c tests/unit/test.h
	$(CC) $(CFLAGS) -o $@ tests/unit/test_sds.c src/sds.c src/zmalloc.c

tests/unit/test_siphash: tests/unit/test_siphash.c src/siphash.c tests/unit/test.h
	$(CC) $(CFLAGS) -o $@ tests/unit/test_siphash.c src/siphash.c

tests/unit/test_dict: tests/unit/test_dict.c src/dict.c src/zmalloc.c tests/unit/test.h
	$(CC) $(CFLAGS) -o $@ tests/unit/test_dict.c src/dict.c src/zmalloc.c

tests/unit/test_adlist: tests/unit/test_adlist.c src/adlist.c src/zmalloc.c tests/unit/test.h
	$(CC) $(CFLAGS) -o $@ tests/unit/test_adlist.c src/adlist.c src/zmalloc.c

tests/unit/test_skiplist: tests/unit/test_skiplist.c src/skiplist.c src/sds.c src/zmalloc.c tests/unit/test.h
	$(CC) $(CFLAGS) -o $@ tests/unit/test_skiplist.c src/skiplist.c src/sds.c src/zmalloc.c

tests/unit/test_pq: tests/unit/test_pq.c src/pq.c src/zmalloc.c tests/unit/test.h
	$(CC) $(CFLAGS) -o $@ tests/unit/test_pq.c src/pq.c src/zmalloc.c

tests/unit/test_resp: tests/unit/test_resp.c src/resp.c src/sds.c src/zmalloc.c tests/unit/test.h
	$(CC) $(CFLAGS) -o $@ tests/unit/test_resp.c src/resp.c src/sds.c src/zmalloc.c

CORE_SRC := src/zmalloc.c src/sds.c src/siphash.c src/dict.c src/adlist.c \
	src/skiplist.c src/pq.c src/resp.c src/ae.c src/anet.c src/util.c \
	src/object.c src/config.c src/networking.c src/server.c

src/simpleredis-server: $(CORE_SRC) src/main.c
	$(CC) $(CFLAGS) -o $@ $(CORE_SRC) src/main.c $(LDFLAGS)

tests/unit/test_ping: tests/unit/test_ping.c $(CORE_SRC) tests/unit/test.h
	$(CC) $(CFLAGS) -o $@ tests/unit/test_ping.c $(CORE_SRC) $(LDFLAGS)

test: $(UNIT_TESTS)
	@fail=0; \
	for t in $(UNIT_TESTS); do \
		./$$t || fail=1; \
	done; \
	exit $$fail

clean:
	rm -f $(UNIT_TESTS) src/simpleredis-server src/*.o
