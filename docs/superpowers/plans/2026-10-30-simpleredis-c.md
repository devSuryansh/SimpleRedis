# SimpleRedis C server implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the C11 `simpleredis-server` that stock Redis clients can use for the command matrix in the spec.

**Architecture:** One command thread and an epoll loop. Intrusive dicts hold the keyspace and the nested types. A skiplist orders sorted sets. A min-heap expires keys. RESP is encoded into the client output buffer. The loop writes sockets. AOF and `SRDB` are the on-disk contract.

**Tech Stack:** C11, POSIX, epoll, pthreads for AOF fsync only. Tests are C binaries plus Python 3 sockets.

**Spec:** `docs/superpowers/specs/2026-10-30-simpleredis-design.md`

## Global Constraints

- C11, 4-space indent, K&R braces, `/* */` comments only, `camelCase` functions, `UPPER_SNAKE` macros, `lower_snake` locals, `static` for file-local functions.
- `-Wall -Wextra -Werror -std=c11`.
- Binary name `simpleredis-server`. Default bind `127.0.0.1:6379`.
- BSD-3-Clause license.
- Tests use the standard library only. No Redis install.
- Commits use `git commit -s` and a Conventional Commits title.
- Command handlers append replies. They do not read or write sockets.
- `redis_version` reported to clients is `7.2.4`. `simpleredis_version` is `0.1.0`.
- Snapshot magic is `SRDB`, version `1`, little-endian, layout in the spec.
- Queue-time `EXEC` abort: unknown command, wrong arity, syntax error.
- `SCAN` may duplicate or skip keys that move during a walk. It never returns an expired key.

## Review Focus

- A command split across two TCP reads must run once, not as a protocol error. Pinned by Task 8 and Task 16.
- A second client writing a watched key must make `EXEC` return a null array and leave the key unchanged by the queued command. Pinned by Task 16.
- Replaying `PEXPIREAT` after a restart must not extend a ttl that was already short. Pinned by Task 13.
- With `maxmemory` of 1, `SET` of a larger value is `-OOM` and `DEL` still works. Pinned by Task 12.
- `HGETALL` on a RESP3 connection is a `%` map, and on RESP2 is a flat `*` array. Pinned by Task 11 and Task 16.

---

### Task 1: Scaffold

**Files:**
- Create: `Makefile`, `LICENSE`, `README.md`, `redis.conf`, `.gitignore`, `tests/unit/test.h`, `tests/unit/test_harness.c`

**Interfaces:**
- Consumes: nothing
- Produces: `make test` runs every `tests/unit/test_*` binary. `EXPECT(cond)` counts failures. `make` builds `src/simpleredis-server`.

- [ ] **Step 1: Write the failing test**

`tests/unit/test_harness.c` calls `EXPECT(1 == 0)` so the harness reports a failure.

- [ ] **Step 2: Run test to verify it fails**

Run: `make test`
Expected: exit non-zero, output contains `failed`.

- [ ] **Step 3: Fix the assertion to `EXPECT(1 == 1)` and add the Makefile, BSD-3-Clause `LICENSE`, `redis.conf` with the spec defaults, `.gitignore` ignoring `context/`, objects, the binary, `*.aof`, `*.srdb`, and `.superpowers/`. README names the binary, the default address, and points at the spec for the command matrix.**

- [ ] **Step 4: Run test to verify it passes**

Run: `make test`
Expected: exit 0, output contains `1 tests, 0 failed`.

- [ ] **Step 5: Commit**

```bash
git add Makefile LICENSE README.md redis.conf .gitignore tests/unit/test.h tests/unit/test_harness.c docs/superpowers
git commit -s -m "$(cat <<'EOF'
chore: scaffold the SimpleRedis build

Add the C build, the unit-test harness, and the design record so later modules can land one at a time.
EOF
)"
```

### Task 2: Allocator and SDS

**Files:**
- Create: `src/zmalloc.h`, `src/zmalloc.c`, `src/sds.h`, `src/sds.c`, `tests/unit/test_sds.c`
- Modify: `Makefile`

**Interfaces:**
- Consumes: harness `EXPECT`
- Produces: `zmalloc`, `zrealloc`, `zfree`, `zstrdup`, `zmallocUsed`. `sds` is a `char *` to a buffer whose header sits immediately before it. `sdsnew`, `sdsnewlen`, `sdsempty`, `sdsdup`, `sdsfree`, `sdslen`, `sdsavail`, `sdscatlen`, `sdscpy`, `sdscmp`, `sdsfromlonglong`.

- [ ] **Step 1: Write the failing test**

Assert `sdsnew("hi")` has length 2 and compares equal to itself, `sdscatlen` grows it, `sdsfromlonglong(-12)` is `"-12"`, and `zmallocUsed()` drops back after `sdsfree`.

- [ ] **Step 2: Run test to verify it fails**

Run: `make tests/unit/test_sds && ./tests/unit/test_sds`
Expected: compile fails, `sdsnew` undeclared.

- [ ] **Step 3: Implement zmalloc (size header, running total) and SDS (uint32 len and alloc, binary safe, always NUL terminated).**

- [ ] **Step 4: Run test to verify it passes**

Run: `make tests/unit/test_sds && ./tests/unit/test_sds`
Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git commit -s -m "$(cat <<'EOF'
feat: add a counted allocator and length-prefixed strings

SDS keeps binary keys and values addressable without scanning for a terminator.
EOF
)"
```

### Task 3: SipHash

**Files:**
- Create: `src/siphash.h`, `src/siphash.c`, `tests/unit/test_siphash.c`

**Interfaces:**
- Produces: `uint64_t siphash24(const unsigned char key[16], const void *data, size_t len)`.

- [ ] **Step 1: Write the failing test**

Key bytes `0x00` through `0x0f`, message bytes `0x00` through `0x0e`. Expect `0xa129ca6149be45e5`.

- [ ] **Step 2: Run test to verify it fails**

Run: `make tests/unit/test_siphash && ./tests/unit/test_siphash`
Expected: compile fails, `siphash24` undeclared.

- [ ] **Step 3: Implement SipHash-2-4.**

- [ ] **Step 4: Run test to verify it passes**

Run: `make tests/unit/test_siphash && ./tests/unit/test_siphash`
Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git commit -s -m "$(cat <<'EOF'
feat: add SipHash-2-4 for untrusted keys

A secret seed stops a client from packing every key into one hash chain.
EOF
)"
```

### Task 4: Dict

**Files:**
- Create: `src/dict.h`, `src/dict.c`, `tests/unit/test_dict.c`

**Interfaces:**
- Produces: the `dict` interface in the spec, plus `dictGetIterator`, `dictNext`, `dictReleaseIterator`. `dictAdd` and `dictFind` move one occupied bucket when a resize is in progress. Resize when `used / slots > 1`. Initial slots 4. Slots come from `calloc`.

- [ ] **Step 1: Write the failing test**

Embed `dictNode` in a struct with an `int key`. Insert 100 keys, find each, delete half, find the rest, and iterate the survivors. Force growth past the load factor.

- [ ] **Step 2: Run test to verify it fails**

Run: `make tests/unit/test_dict && ./tests/unit/test_dict`
Expected: compile fails, `dictCreate` undeclared.

- [ ] **Step 3: Implement the two-table incremental dict.**

- [ ] **Step 4: Run test to verify it passes**

Run: `make tests/unit/test_dict && ./tests/unit/test_dict`
Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git commit -s -m "$(cat <<'EOF'
feat: add an incremental chaining dict

Resizes move a few buckets per call so a large keyspace does not stall the loop.
EOF
)"
```

### Task 5: Lists

**Files:**
- Create: `src/adlist.h`, `src/adlist.c`, `tests/unit/test_adlist.c`

**Interfaces:**
- Produces: `listCreate`, `listRelease`, `listAddNodeHead`, `listAddNodeTail`, `listDelNode`, `listLength`, `listGetIterator`, `listNext`, `listReleaseIterator`, `listSetFreeMethod`. Directions `AL_START_HEAD` and `AL_START_TAIL`.

- [ ] **Step 1: Write the failing test**

Push head `1` then tail `2`. Length is 2. Forward walk is `1`, `2`. Delete the head. Length is 1.

- [ ] **Step 2: Run test to verify it fails**

Run: `make tests/unit/test_adlist && ./tests/unit/test_adlist`
Expected: compile fails, `listCreate` undeclared.

- [ ] **Step 3: Implement the doubly linked list.**

- [ ] **Step 4: Run test to verify it passes**

Run: `make tests/unit/test_adlist && ./tests/unit/test_adlist`
Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git commit -s -m "$(cat <<'EOF'
feat: add a doubly linked list

List values and client subscriptions share one list module.
EOF
)"
```

### Task 6: Skiplist

**Files:**
- Create: `src/skiplist.h`, `src/skiplist.c`, `tests/unit/test_skiplist.c`

**Interfaces:**
- Produces: `zslCreate`, `zslFree`, `zslInsert`, `zslDelete`, `zslGetRank` (1-based, 0 if missing), `zslGetElementByRank`. Max level 32. Equal scores order by member byte order. The node owns its `sds` member.

- [ ] **Step 1: Write the failing test**

Insert scores `(1, "b")`, `(1, "a")`, `(3, "c")`. Rank of `"a"` is 1, `"b"` is 2, `"c"` is 3. Delete `"b"`. Rank of `"c"` is 2.

- [ ] **Step 2: Run test to verify it fails**

Run: `make tests/unit/test_skiplist && ./tests/unit/test_skiplist`
Expected: compile fails, `zslCreate` undeclared.

- [ ] **Step 3: Implement the skiplist.**

- [ ] **Step 4: Run test to verify it passes**

Run: `make tests/unit/test_skiplist && ./tests/unit/test_skiplist`
Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git commit -s -m "$(cat <<'EOF'
feat: add a skiplist for ordered scores

Sorted-set ranges walk score order, then member order, without a full sort.
EOF
)"
```

### Task 7: Expiry heap

**Files:**
- Create: `src/pq.h`, `src/pq.c`, `tests/unit/test_pq.c`

**Interfaces:**
- Produces: `pqCreate(setIndex)`, `pqRelease`, `pqUpsert(entry, expire_at_ms)`, `pqRemove(entry)`, `pqPeek`. `setIndex(entry, idx)` stores the slot. `(size_t)-1` means absent. `pqUpsert` on an existing entry moves it.

- [ ] **Step 1: Write the failing test**

Upsert deadlines 5, 1, and 3. Peek is 1. Upsert the 5 entry down to 0. Peek is 0. Remove it. Peek is 1.

- [ ] **Step 2: Run test to verify it fails**

Run: `make tests/unit/test_pq && ./tests/unit/test_pq`
Expected: compile fails, `pqCreate` undeclared.

- [ ] **Step 3: Implement the binary heap.**

- [ ] **Step 4: Run test to verify it passes**

Run: `make tests/unit/test_pq && ./tests/unit/test_pq`
Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git commit -s -m "$(cat <<'EOF'
feat: add a min-heap for expiry deadlines

The loop wakes for the next deadline instead of scanning every key.
EOF
)"
```

### Task 8: RESP codec

**Files:**
- Create: `src/resp.h`, `src/resp.c`, `tests/unit/test_resp.c`

**Interfaces:**
- Produces: `int respParse(const char *buf, size_t len, size_t *consumed, respVal *out)`. Return 1, 0, or -1. On 0, `*consumed` is 0. `sds respEncode(const respVal *v, int proto)`. `respFree(respVal *v)`. Types cover simple, error, integer, bulk, null, array, map, bool, double, push.

- [ ] **Step 1: Write the failing test**

Feed `*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n` in two halves. The first call returns 0 and consumes 0. The second returns an array of two bulks. Encode a null at proto 2 as `$-1\r\n` and at proto 3 as `_\r\n`. Encode a one-pair map at proto 3 starting with `%1\r\n`. A truncated bulk returns -1 only when the declared length is followed by a bad CRLF. Incomplete input returns 0.

- [ ] **Step 2: Run test to verify it fails**

Run: `make tests/unit/test_resp && ./tests/unit/test_resp`
Expected: compile fails, `respParse` undeclared.

- [ ] **Step 3: Implement the codec.**

- [ ] **Step 4: Run test to verify it passes**

Run: `make tests/unit/test_resp && ./tests/unit/test_resp`
Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git commit -s -m "$(cat <<'EOF'
feat: add an incremental RESP2 and RESP3 codec

Partial socket reads wait for the rest of the value instead of failing the client.
EOF
)"
```

### Task 9: Event loop and PING

**Files:**
- Create: `src/ae.h`, `src/ae.c`, `src/ae_epoll.c`, `src/anet.h`, `src/anet.c`, `src/server.h`, `src/server.c`, `src/networking.c`, `src/config.c`
- Modify: `Makefile`

**Interfaces:**
- Produces: `aeCreateEventLoop`, `aeCreateFileEvent`, `aeDeleteFileEvent`, `aeCreateTimeEvent`, `aeProcessEvents`, `aeStop`. Masks `AE_READABLE` and `AE_WRITABLE`. `anetTcpServer`, `anetNonBlock`, `anetTcpAccept`. `initServer`, `main`. Global `redisServer server`. Client output helpers from the spec. `PING` and `ECHO`.

- [ ] **Step 1: Write the failing test**

`tests/unit/test_ping.c` starts nothing on the socket. It calls `initTestServer()`, builds a client with `fd == -1` and argv `PING`, calls `processCommand`, and expects the reply buffer `+PONG\r\n`.

- [ ] **Step 2: Run test to verify it fails**

Run: `make tests/unit/test_ping && ./tests/unit/test_ping`
Expected: compile fails, `processCommand` undeclared.

- [ ] **Step 3: Implement config defaults, the epoll loop, accept/read/write, and PING/ECHO. `port 0` binds an ephemeral port and logs the real port on stdout as `ready <port>`.**

- [ ] **Step 4: Run test to verify it passes**

Run: `make tests/unit/test_ping && ./tests/unit/test_ping`
Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git commit -s -m "$(cat <<'EOF'
feat: accept clients and answer PING

The process is a single epoll loop so later commands share one thread.
EOF
)"
```

### Task 10: Keyspace, strings, and expiry

**Files:**
- Create: `src/object.c`, `src/db.c`, `src/expire.c`, `src/t_string.c`, `tests/unit/test_strings.c`

**Interfaces:**
- Produces: `dbLookup`, `dbSet`, `dbDelete`, `dbSetExpire`, `lookupKeyRead`. Commands: `GET`, `SET` (EX, PX, NX, XX, GET), `SETNX`, `SETEX`, `MGET`, `MSET`, `INCR`, `INCRBY`, `DECR`, `DECRBY`, `APPEND`, `STRLEN`, `DEL`, `EXISTS`, `TYPE`, `KEYS`, `SCAN`, `EXPIRE`, `PEXPIRE`, `EXPIREAT`, `PEXPIREAT`, `TTL`, `PTTL`, `PERSIST`, `DBSIZE`, `FLUSHDB`, `FLUSHALL`, `SELECT`.

- [ ] **Step 1: Write the failing test**

`SET k v` then `GET k` yields `$1\r\nv\r\n`. `INCR missing` yields `:1\r\n`. `INCR` of `9223372036854775807` yields `-ERR increment or decrement would overflow` and `GET` is unchanged. `SET k v PX 1`, sleep 20ms, `GET k` is `$-1\r\n`. `SET` with `NX` on an existing key is `$-1\r\n`.

- [ ] **Step 2: Run test to verify it fails**

Run: `make tests/unit/test_strings && ./tests/unit/test_strings`
Expected: FAIL or compile failure before `SET` exists.

- [ ] **Step 3: Implement objects, 16 databases, lazy expiry, the heap drain of at most 64 keys per cron, and the commands above. Wrong type and wrong arity use the spec strings.**

- [ ] **Step 4: Run test to verify it passes**

Run: `make tests/unit/test_strings && ./tests/unit/test_strings`
Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git commit -s -m "$(cat <<'EOF'
feat: store strings with expiry

GET hides a key as soon as its deadline has passed, and the heap deletes what is due.
EOF
)"
```

### Task 11: Lists, hashes, sets, sorted sets

**Files:**
- Create: `src/t_list.c`, `src/t_hash.c`, `src/t_set.c`, `src/t_zset.c`, `tests/unit/test_types.c`

**Interfaces:**
- Produces: the list, hash, set, and zset commands in the spec. Empty containers are removed. `HGETALL` encodes a map when `client->proto == 3`.

- [ ] **Step 1: Write the failing test**

`LPUSH l a b`, `LRANGE l 0 -1` is `b`, `a`. `HSET h f v` is `:1\r\n`. Second `HSET` of the same field is `:0\r\n`. `HGETALL` at proto 2 starts with `*2\r\n`. At proto 3 it starts with `%1\r\n`. `SADD` duplicate is `:0\r\n`. `ZADD z 1 b 1 a`, `ZRANGE z 0 -1` is `a` then `b`. `GET l` is `-WRONGTYPE Operation against a key holding the wrong kind of value`.

- [ ] **Step 2: Run test to verify it fails**

Run: `make tests/unit/test_types && ./tests/unit/test_types`
Expected: failure before the list commands exist.

- [ ] **Step 3: Implement the four types.**

- [ ] **Step 4: Run test to verify it passes**

Run: `make tests/unit/test_types && ./tests/unit/test_types`
Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git commit -s -m "$(cat <<'EOF'
feat: add list, hash, set, and sorted-set commands

One encoding per type keeps the keyspace typed the way Redis clients expect.
EOF
)"
```

### Task 12: Handshake, limits, and auth

**Files:**
- Create: `tests/unit/test_admin.c`
- Modify: `src/server.c`, `src/networking.c`, `src/config.c`

**Interfaces:**
- Produces: `HELLO`, `CLIENT`, `COMMAND`, `COMMAND COUNT`, `COMMAND INFO`, `ROLE`, `TIME`, `AUTH`, `CONFIG GET`, `CONFIG SET`, `INFO`, `QUIT`, `SHUTDOWN`. `requirepass` gate. `maxmemory` denial for growing writes.

- [ ] **Step 1: Write the failing test**

`HELLO 3` reply contains `proto` and `:3`. `CLIENT SETINFO LIB-NAME py` is `+OK\r\n`. `COMMAND COUNT` is a positive integer. With `maxmemory` 1, `SET big` of 8 bytes is `-OOM command not allowed when used memory > 'maxmemory'`. `AUTH` without a password is `-ERR AUTH called without any password configured for the default user`.

- [ ] **Step 2: Run test to verify it fails**

Run: `make tests/unit/test_admin && ./tests/unit/test_admin`
Expected: failure before `HELLO` exists.

- [ ] **Step 3: Implement the handshake, the config get/set allow-list, auth, and the OOM check.**

- [ ] **Step 4: Run test to verify it passes**

Run: `make tests/unit/test_admin && ./tests/unit/test_admin`
Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git commit -s -m "$(cat <<'EOF'
feat: speak the client handshake and enforce limits

HELLO, CLIENT, and COMMAND let current libraries connect without a Redis-specific fork.
EOF
)"
```

### Task 13: AOF and snapshot

**Files:**
- Create: `src/aof.c`, `src/rdb.c`, `tests/unit/test_persist.c`

**Interfaces:**
- Produces: `aofAppendCommand`, `aofLoad`, `aofRewrite`, `saveSnapshot`, `loadSnapshot`. Formats from the spec. Loading does not append. `SET` with `PX` is logged as `SET` plus `PEXPIREAT`.

- [ ] **Step 1: Write the failing test**

`SET k v PX 60000` writes an AOF that contains `PEXPIREAT`. A new test server loading that file returns the value, and `PTTL` is at most 60000. `SAVE` then `loadSnapshot` into an empty server restores `k`. Expired keys are absent from the snapshot.

- [ ] **Step 2: Run test to verify it fails**

Run: `make tests/unit/test_persist && ./tests/unit/test_persist`
Expected: failure before `saveSnapshot` exists.

- [ ] **Step 3: Implement AOF, the fsync thread, rewrite, and SRDB. Wire `SAVE` and `REWRITEAOF`.**

- [ ] **Step 4: Run test to verify it passes**

Run: `make tests/unit/test_persist && ./tests/unit/test_persist`
Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git commit -s -m "$(cat <<'EOF'
feat: persist with AOF and a snapshot

Expiry is stored as an absolute deadline so a restart does not extend a ttl.
EOF
)"
```

### Task 14: Transactions

**Files:**
- Create: `src/multi.c`, `tests/unit/test_multi.c`

**Interfaces:**
- Produces: `MULTI`, `EXEC`, `DISCARD`, `WATCH`, `UNWATCH`. Watched keys live in a per-database dict of client lists. Modifications mark those clients dirty.

- [ ] **Step 1: Write the failing test**

`MULTI`, `SET a 1`, `EXEC` yields `*1\r\n+OK\r\n` and `GET a` is `1`. `WATCH a`, external `dbSet` of `a`, then `MULTI`/`SET a 2`/`EXEC` yields `*-1\r\n` and `GET a` is still the external value. `INCR` queued against a list yields an `EXEC` array containing `-WRONGTYPE`. An unknown queued command makes `EXEC` the `EXECABORT` error.

- [ ] **Step 2: Run test to verify it fails**

Run: `make tests/unit/test_multi && ./tests/unit/test_multi`
Expected: failure before `MULTI` exists.

- [ ] **Step 3: Implement transactions and watch.**

- [ ] **Step 4: Run test to verify it passes**

Run: `make tests/unit/test_multi && ./tests/unit/test_multi`
Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git commit -s -m "$(cat <<'EOF'
feat: add watched transactions

EXEC is atomic on the single command thread, and a dirty watch discards the queue.
EOF
)"
```

### Task 15: Pub/sub

**Files:**
- Create: `src/pubsub.c`, `tests/unit/test_pubsub.c`

**Interfaces:**
- Produces: `SUBSCRIBE`, `UNSUBSCRIBE`, `PUBLISH`. RESP3 confirmations and messages use `>`. A subscribed client rejects `GET` with the spec error. `PING` while subscribed is a two-element pong.

- [ ] **Step 1: Write the failing test**

Two fake clients subscribe to `ch`. `PUBLISH ch hi` returns `:2\r\n`. Each subscriber buffer contains `message` and `hi`. At proto 3 the notification starts with `>`. `GET` on a subscribed client is the spec error.

- [ ] **Step 2: Run test to verify it fails**

Run: `make tests/unit/test_pubsub && ./tests/unit/test_pubsub`
Expected: failure before `SUBSCRIBE` exists.

- [ ] **Step 3: Implement pub/sub.**

- [ ] **Step 4: Run test to verify it passes**

Run: `make tests/unit/test_pubsub && ./tests/unit/test_pubsub`
Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git commit -s -m "$(cat <<'EOF'
feat: add publish and subscribe

Messages are delivered on the publisher's turn to every client in the channel.
EOF
)"
```

### Task 16: Protocol suite and operator README

**Files:**
- Create: `tests/integration/protocol.py`
- Modify: `Makefile`, `README.md`

**Interfaces:**
- Consumes: `simpleredis-server [conf]`, stdout line `ready <port>` when ready.

- [ ] **Step 1: Write the failing test**

Python script, standard library only. Start the server with `port 0`, `appendonly yes`, and a temp `dir`. Cover pipelined `SET`/`GET`, a split-byte `PING`, RESP3 `HGETALL` as a map, `HELLO 3`, `CLIENT SETINFO`, wrong type, `MULTI`/`WATCH` from two sockets, pub/sub from two sockets, TTL survival across a process restart under 60 seconds, `SHUTDOWN` leaving an AOF the next process loads, and `SCAN` plus `KEYS`. Exit 1 on any mismatch.

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/integration/protocol.py`
Expected: a connection or assertion failure until the server flags match the script. If it already passes, the step's expected failure is already satisfied by an earlier red run of a single assertion you temporarily invert, then restore. Do not skip the read of the output.

- [ ] **Step 3: Fill the README support matrix, the non-goals, Python, Node, and Go client snippets for protocols 2 and 3, and a foreground service example. Fix server bugs the suite exposes.**

- [ ] **Step 4: Run test to verify it passes**

Run: `make test && python3 tests/integration/protocol.py`
Expected: both exit 0.

- [ ] **Step 5: Commit**

```bash
git commit -s -m "$(cat <<'EOF'
test: cover the wire protocol without a Redis install

The suite is the contract a stock client relies on, including restart and pub/sub.
EOF
)"
```
