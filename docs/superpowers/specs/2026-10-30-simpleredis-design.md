# SimpleRedis design

## Intent

SimpleRedis is a single-instance in-memory server that a normal Redis client can use instead of Redis. The client keeps its library. The operator stops running Redis. Supported commands, reply bytes, and expiry match what those clients already expect.

The build, the tests, and the running process do not invoke `redis-server` or `redis-cli` and do not link the Redis project. Source is original. `context/` is reference reading and stays untracked.

There are two implementations of this same contract. The C11 server is the reference. The Rust server is a safe port of the same modules. A dataset written by one loads in the other, because AOF and the snapshot format below are part of the contract.

## Goals

- Stock clients connect with RESP2 or RESP3 and run the command matrix below.
- Many clients, pipelining, and partial reads and writes.
- Strings, lists, hashes, sets, and sorted sets, with expiry a caller can trust.
- Durability in one mode at a time: AOF, or a snapshot.
- Transactions and pub/sub.
- A crash loses at most the unsynced tail of the active mode. `SHUTDOWN`, `SIGINT`, and `SIGTERM` persist, then exit.

## Non-goals

Replication, cluster, sentinel, streams, Lua, modules, ACL user lists, TLS, bitops, geo, HyperLogLog, eviction policies, a thread pool for commands, loading files produced by Redis, and more than one encoding per type. Unsupported commands return `-ERR unknown command '<name>'`, and the README lists them as unsupported.

Inline telnet commands (`PING\r\n` without a RESP array) are unsupported. Stock clients send arrays of bulk strings.

## Constraints

- C11. 4-space indent. K&R braces. Comments are `/* */` only. Functions are `camelCase`. Macros and constants are `UPPER_SNAKE`. Locals are `lower_snake`. File-local functions are `static`.
- Compiler flags: `-Wall -Wextra -Werror -std=c11`.
- Binary name `simpleredis-server`. Default bind `127.0.0.1:6379`.
- License: BSD-3-Clause.
- Rust port uses safe Rust. `libc` is allowed only for epoll. No line-by-line unsafe translation.
- Tests use the standard library only. No Redis install.
- Commits: `git commit -s` and a Conventional Commits title plus a body when the reason is not obvious.

## Calls

These are settled here so a builder does not reopen them.

- Epoll is level-triggered.
- Dict entries are intrusive: the caller embeds `dictNode` and supplies the hash. SipHash-2-4 lives outside the dict. The process seed comes from `/dev/urandom` at startup.
- One encoding per type. Strings are length-prefixed buffers. Lists are doubly linked. Hashes and sets are dicts. Sorted sets are a skiplist plus a dict from member to skiplist node.
- Expiry is lazy on access plus a min-heap. Each loop iteration deletes at most 64 due keys. If any remain, the next wait is 0.
- `DBSIZE`, `EXISTS`, `KEYS`, `SCAN`, and `TYPE` do not count or return a key that is already expired. Touching such a key deletes it.
- Pub/sub notifications use RESP2 arrays on RESP2 connections and RESP3 push (`>`) on RESP3 connections.
- `INFO` and `HELLO` report `redis_version` `7.2.4` because clients feature-detect the protocol generation. `simpleredis_version` is `0.1.0`. The binary is not Redis.
- An unknown config directive is a fatal startup error.
- `requirepass` empty means no authentication. `CONFIG GET requirepass` returns the configured value.
- Glob patterns support `*` and `?` only.
- ZSET scores are IEEE doubles. Equal scores sort by member byte order. Score bulk strings use `%.17g`.
- Snapshot bytes are the layout in Persistence, little-endian. Both languages must round-trip that layout.
- Queue-time transaction errors are unknown command, wrong arity, and syntax error. They set a dirty flag and do not run at `EXEC`. `EXEC` then replies `-EXECABORT Transaction discarded because of previous errors.` `WRONGTYPE`, `OOM`, and integer errors happen when the queued command runs, and each one is an element of the `EXEC` array.
- `maxmemory` counts bytes from the allocator wrapper in both languages (`zmalloc` in C, a counting global allocator in Rust). Tests set a tiny limit and assert that a growing write is rejected while `GET` and `DEL` still work. They do not assert equal `used_memory` across languages.
- `SCAN` is best-effort. One full walk may return a key twice and may miss a key that was rehashed or written while the cursor was moving. It returns only keys present in a visited bucket at the moment of the visit, and it never returns an expired key.

## Modules

A command handler receives the client state and appends a reply value. It does not read or write a socket. The event loop does not inspect key types. Persistence appends a command array or walks the keyspace. It does not see client buffers.

| Module | Interface the caller learns | What stays inside |
| --- | --- | --- |
| zmalloc | `zmalloc`, `zrealloc`, `zfree`, `zstrdup`, `zmallocUsed` | size header, running total |
| sds | create, free, length, cat, copy, compare | header before the buffer |
| siphash | `siphash24(key16, data, len) -> uint64` | compression rounds |
| dict | create, add node, find, delete, iterate, release | two tables, incremental move of a few buckets |
| adlist | create, push head/tail, delete node, iterate, length | node allocation |
| skiplist | create, insert, delete, rank, element by rank, free | levels, spans |
| pq | upsert, remove, peek, pop | index repair |
| resp | feed bytes, pull one value, write one value for a protocol version | parser state |
| ae | create loop, file event, time event, process once, stop | epoll |
| anet | listen, nonblock, accept | socket calls |
| db | lookup, store, delete, expire, scan, type check | lazy expiry, heap index |
| commands | `argv` on the client, replies appended | per-type operations |
| aof / snapshot | append command, load, rewrite, save | file format |
| server | accept, dispatch, write buffers | client list, cron |

Dict is the shared seam: keyspace, hash fields, set members, sorted-set member index, pub/sub channels, watched keys. The skiplist is private to sorted sets. The heap is private to expiry.

### Dict

```c
typedef struct dictNode {
    struct dictNode *next;
} dictNode;

typedef int (*dictKeyCompare)(dictNode *node, const void *key, size_t keylen);
typedef void (*dictNodeFree)(dictNode *node);
typedef unsigned int (*dictNodeHash)(dictNode *node);

dict *dictCreate(dictKeyCompare cmp, dictNodeHash hashFn, dictNodeFree freeFn);
void dictAdd(dict *d, dictNode *node);
dictNode *dictFind(dict *d, const void *key, size_t keylen, unsigned int hash);
dictNode *dictDelete(dict *d, const void *key, size_t keylen, unsigned int hash);
unsigned long dictSize(dict *d);
void dictRelease(dict *d);
```

`dictFind` and `dictAdd` move one bucket when a resize is in progress. Resize starts when `used / slots > 1`, to the next power of two, slots allocated with `calloc`. Initial slots: 4. Hash is `siphash24` reduced to the slot count. Compare sees the embedded node.

### Keyspace entry

```c
typedef struct kvEntry {
    dictNode node;
    sds key;
    robj *value;
    long long expire_at_ms; /* unix ms, -1 if none */
    size_t heap_idx;         /* (size_t)-1 if not in the heap */
} kvEntry;
```

`robj` is `type`, `refcount`, `ptr`. Types: string, list, hash, set, zset. Deleting a key frees the value, removes the heap node, and marks watchers dirty.

### Command dispatch

```c
typedef void (*redisCommandProc)(client *c);

void addReplySimple(client *c, const char *s);
void addReplyError(client *c, const char *s);
void addReplyLongLong(client *c, long long n);
void addReplyBulk(client *c, const char *s, size_t len);
void addReplyNull(client *c);
void addReplyArrayLen(client *c, long long n);
void addReplyMapLen(client *c, long long n);
void addReplyPushLen(client *c, long long n);
void addReplyBool(client *c, int b);
void addReplyDouble(client *c, double d);
```

`addReply*` only appends to the client output buffer. The loop writes the buffer when the socket is writable. A client whose output buffer exceeds 64MB, or whose query buffer exceeds 64MB, is closed.

Tests at the command seam build a client with `fd == -1`, set `argc` and `argv`, call the proc, and read the output buffer.

## Wire protocol

Incremental parser. A short read returns "need more data" and consumes nothing of the incomplete value. A protocol error produces `-ERR Protocol error` and closes the client.

RESP2: `+` simple, `-` error, `:` integer, `$` bulk, `$-1` null, `*` array.

RESP3 adds `_` null, `#t` / `#f` bool, `,` double, `%` map, and `>` push. Negotiated per connection. Default is RESP2 until `HELLO 2` or `HELLO 3`.

Map replies, and only these, change shape with the protocol:

- `HELLO`
- `HGETALL`
- `CONFIG GET`

In RESP2 those are flat arrays of alternating field and value. In RESP3 they are maps. Every other aggregate is an array in both protocols. `ZSCORE` stays a bulk string.

Null bulk in RESP2 is `$-1\r\n`. Null in RESP3 is `_\r\n`.

Command names are case-insensitive.

## Client handshake and admin

`HELLO` with no argument reports the current protocol and does not switch. `HELLO 2` and `HELLO 3` switch and reply with a map (or flat array) of: `server` = `redis`, `version` = `7.2.4`, `proto`, `id`, `mode` = `standalone`, `role` = `master`, `modules` = empty array. Any other version: `-NOPROTO unsupported protocol version`.

`HELLO` may include `AUTH password` and `SETNAME name` after the version. `AUTH` inside `HELLO` uses the same password check as `AUTH`.

`CLIENT ID` returns the connection id (monotonic from 1). `CLIENT SETNAME` / `GETNAME` store a name (null if unset). `CLIENT SETINFO LIB-NAME <s>` and `CLIENT SETINFO LIB-VER <s>` return `+OK` and ignore the value. Any other `CLIENT` form: `-ERR unknown subcommand`.

`COMMAND` returns an array of command specs. Each spec is an array: name (bulk), arity (integer), flags (array of bulks), first key position, last key position, step. Arity is Redis-style: positive means exact `argc`, negative `-N` means at least `N` arguments including the command name. `COMMAND COUNT` is an integer. `COMMAND INFO name` is an array of one spec, or a null if the name is unknown.

`ROLE` is the array `master`, integer `0`, empty array.

`TIME` is two bulks: unix seconds and microseconds.

`AUTH password`: if `requirepass` is empty, `-ERR AUTH called without any password configured for the default user`. If it matches, `+OK` and the client is authenticated. If it does not, `-WRONGPASS invalid username-password pair or user is disabled`.

When `requirepass` is set, a new client may only run `AUTH`, `PING`, `HELLO`, and `QUIT`. Anything else: `-NOAUTH Authentication required.`.

`CONFIG GET` accepts one glob and returns the matching keys among: `port`, `bind`, `databases`, `timeout`, `maxclients`, `maxmemory`, `appendonly`, `appendfsync`, `appendfilename`, `dir`, `dbfilename`, `requirepass`. `CONFIG SET` accepts only `timeout <seconds>` and `appendfsync always|everysec|no`. Anything else: `-ERR Unsupported CONFIG parameter: <name>`.

`INFO` is a bulk string with sections `Server`, `Clients`, `Memory`, `Persistence`, `Stats`, `Keyspace`. It includes `redis_version:7.2.4`, `simpleredis_version:0.1.0`, `used_memory`, `connected_clients`, `total_commands_processed`, `role:master`, `tcp_port`, `aof_enabled`.

`PING` with no argument is `+PONG`. With one argument it is a bulk copy. `ECHO` is a bulk copy. `QUIT` is `+OK` and the connection closes after the write. `SELECT n` switches the client database. Out of range: `-ERR DB index is out of range`. `DBSIZE` counts live keys in the current database. `FLUSHDB` and `FLUSHALL` delete live keys and return `+OK`.

`SHUTDOWN` persists the active mode, then exits. `SHUTDOWN NOSAVE` exits without persisting. `SHUTDOWN SAVE` writes the snapshot, and also fsyncs the AOF when AOF is enabled. `SIGINT` and `SIGTERM` behave as `SHUTDOWN`.

## Data commands

Wrong type, for every command that inspects a value: `-WRONGTYPE Operation against a key holding the wrong kind of value`.

Wrong arity: `-ERR wrong number of arguments for '<cmd>' command`.

Not an integer, or out of signed 64-bit range: `-ERR value is not an integer or out of range`.

`INCR` overflow: `-ERR increment or decrement would overflow`. The key is unchanged.

A missing key is not a wrong type.

`maxmemory` of 0 means unlimited. Above a non-zero limit, commands that can grow memory return `-OOM command not allowed when used memory > 'maxmemory'` and do not run. `DEL`, `FLUSHDB`, `FLUSHALL`, expiry commands, and reads stay allowed.

### Keys

- `DEL` one or more keys. Integer count deleted.
- `EXISTS` one or more keys. Integer count present.
- `TYPE` returns a simple string: `string`, `list`, `hash`, `set`, `zset`, or `none`.
- `KEYS pattern` returns an array of matching keys in the current database.
- `SCAN cursor [MATCH pattern] [COUNT n]`. Reply is an array of two items: the next cursor as a bulk string, and an array of keys. Cursor `0` starts and ends a full walk. `COUNT` defaults to 10 and is a hint. The cursor is a decimal bucket index.
- `EXPIRE` / `PEXPIRE` set a relative ttl. Return 1 if the key exists, 0 if it does not. A non-positive ttl deletes the key and returns 1 if it existed.
- `EXPIREAT` / `PEXPIREAT` take an absolute unix time in seconds or milliseconds.
- `TTL` returns seconds, `PTTL` returns milliseconds. `-2` if missing, `-1` if there is no expiry.
- `PERSIST` returns 1 if an expiry was removed, else 0.

### Strings

Values are binary-safe.

- `GET` bulk, or null if missing.
- `SET key value` options, in any order: `EX seconds`, `PX milliseconds`, `NX`, `XX`, `GET`. `EX` with `PX`, or `NX` with `XX`, is an arity/syntax error: `-ERR syntax error`. `NX` fails (null, no write) when the key exists. `XX` fails when it is missing. `GET` returns the previous bulk (or null) instead of `+OK`. Otherwise `+OK`.
- `SETNX` returns 1 or 0. `SETEX key seconds value` returns `+OK`.
- `MGET` returns an array of bulks and nulls. `MSET` requires an even number of key/value arguments and returns `+OK`.
- `INCR`, `DECR`, `INCRBY`, `DECRBY`. A missing key becomes 0 before the operation. The stored form is the decimal representation.
- `APPEND` returns the new length. `STRLEN` returns the length, or 0 if missing.

### Lists

- `LPUSH`, `RPUSH` one or more values. Return the length.
- `LPOP`, `RPOP` return a bulk, or null if missing or empty.
- `LLEN` returns 0 if missing.
- `LINDEX` supports negative indexes. Out of range is null.
- `LRANGE start stop` is inclusive and supports negative indexes. Missing key is an empty array.
- A list that becomes empty is removed.

### Hashes

- `HSET key field value [field value ...]` returns the integer count of fields that were new.
- `HGET` is a bulk or null.
- `HDEL` returns the count removed.
- `HEXISTS` returns 0 or 1.
- `HLEN` returns 0 if missing.
- `HGETALL`, `HKEYS`, and `HVALS` on a missing key return an empty array (an empty map for `HGETALL` in RESP3), not a null.
- An empty hash is removed.

### Sets

- `SADD` and `SREM` return the count of members actually added or removed.
- `SISMEMBER` returns 0 or 1.
- `SMEMBERS` on a missing key is an empty array.
- `SCARD` returns 0 if missing.
- An empty set is removed.

### Sorted sets

- `ZADD key score member [score member ...]` returns the count of new members. A member that already exists has its score updated and does not count as new.
- `ZREM` returns the count removed.
- `ZSCORE` returns a bulk, or null.
- `ZCARD` returns 0 if missing.
- `ZRANGE start stop [WITHSCORES]` is inclusive, supports negative indexes, and walks in score order then member order. `WITHSCORES` appends the score bulk after each member.
- `ZRANK` returns a 0-based integer, or null.
- An empty sorted set is removed.

## Transactions

`MULTI` replies `+OK` and queues later commands. A queued command replies `+QUEUED` and is not applied. `EXEC` replies an array of the queued replies. `DISCARD` drops the queue and replies `+OK`.

`EXEC` without `MULTI`: `-ERR EXEC without MULTI`. `DISCARD` without `MULTI`: `-ERR DISCARD without MULTI`. Nested `MULTI`: `-ERR MULTI calls can not be nested`.

If any queued command had the wrong arity or a syntax error, `EXEC` runs nothing and replies `-EXECABORT Transaction discarded because of previous errors.`.

`WATCH key [key ...]` replies `+OK`. It is an error inside `MULTI`: `-ERR WATCH inside MULTI is not allowed`. `UNWATCH` replies `+OK` and clears the client's watches. `EXEC` and `DISCARD` also clear them.

The database keeps a dict from key name to the clients watching it, including keys that do not exist yet. Any successful modification or deletion of that key marks those clients dirty. `EXEC` on a dirty client replies null (`*-1\r\n` in RESP2, `_\r\n` in RESP3) and does not run the queue. The same client's own writes dirty its watch.

## Pub/sub

`SUBSCRIBE` and `UNSUBSCRIBE` may name one or more channels. `UNSUBSCRIBE` with no channel unsubscribes all. Each channel produces one confirmation array (or push): `subscribe` or `unsubscribe`, the channel, and the client's remaining subscription count.

`PUBLISH channel message` returns the integer number of subscribers that received it. It is not persisted.

A subscribed client may only run `SUBSCRIBE`, `UNSUBSCRIBE`, `PING`, and `QUIT`. Anything else: `-ERR Can't execute '<cmd>': only SUBSCRIBE / UNSUBSCRIBE / PING / QUIT are allowed in this context`.

`PING` while subscribed replies a two-element array (or push): `pong`, and the optional argument or an empty bulk.

A published message is `message`, channel, payload. RESP3 uses push. Delivery is synchronous, on the publishing client's turn, to subscribers on this process.

## Persistence

One mode is active, chosen by `appendonly`.

Startup when `appendonly yes`: replay `appendfilename` if it exists. Ignore the snapshot. Startup when `appendonly no`: load `dbfilename` if it exists.

### AOF

The file is a sequence of RESP2 arrays of bulk strings. Relative expiry is never replayed as a relative ttl. The server writes:

- `SET` with `EX` or `PX` as `SET key value` plus `PEXPIREAT key <unix ms>`.
- `EXPIRE`, `PEXPIRE`, `EXPIREAT`, and `PEXPIREAT` as `PEXPIREAT` with absolute unix milliseconds.
- `SELECT` when the selected database changes, before the following write.
- Each successful mutating command inside `EXEC`, without the `MULTI` wrapper.
- `FLUSHDB` and `FLUSHALL`.

Failed commands, reads, `PUBLISH`, `WATCH`, and `AUTH` are not logged.

`appendfsync always` fsyncs on the main thread before the reply. `everysec` asks a single side thread to fsync about once a second. That thread only fsyncs the AOF fd. `no` leaves flushing to the OS until shutdown. Shutdown fsyncs.

`REWRITEAOF` walks the keyspace and writes canonical commands (`SET`, `RPUSH`, `HSET`, `SADD`, `ZADD`, and `PEXPIREAT`) to a temp file, fsyncs, and renames it over the AOF. It runs on the main thread. It is an error when `appendonly` is `no`: `-ERR AOF is disabled`.

### Snapshot

`SAVE` writes `dir/dbfilename` on the main thread and replies `+OK`.

```
magic            4 bytes  "SRDB"
version          uint32   1
db_count         uint32
repeated db_count times:
  db_index       uint32
  key_count      uint32
  repeated key_count times:
    expire_at_ms int64    unix ms, or -1
    type         uint8    0 string, 1 list, 2 hash, 3 set, 4 zset
    key_len      uint32
    key          bytes
    payload      as below
```

All integers are little-endian. Payload:

- string: `uint32` length, bytes
- list: `uint32` count, then that many strings, head to tail
- hash: `uint32` count, then field string and value string, repeated
- set: `uint32` count, then member strings
- zset: `uint32` count, then member string and `float64` score, in rank order

Strings inside the payload use the same `uint32` length prefix. Expired keys are omitted. `SAVE` writes a temp file, fsyncs, and renames.

## Config

`simpleredis-server [path]`. If the path is omitted, built-in defaults are used and no file is required. A provided path that cannot be read is fatal.

Lines are `keyword value`. Blank lines and `#` comments are ignored.

| Key | Default |
| --- | --- |
| bind | 127.0.0.1 |
| port | 6379 |
| databases | 16 |
| timeout | 0 |
| maxclients | 10000 |
| maxmemory | 0 |
| appendonly | no |
| appendfsync | everysec |
| appendfilename | appendonly.aof |
| dbfilename | dump.srdb |
| dir | ./ |
| requirepass | empty |
| loglevel | notice |

`timeout` is idle seconds. 0 disables it. `databases` is fixed at startup. `dir` is created if missing.

## Process shape

One command thread. The side thread only fsyncs. `SIGPIPE` is ignored. The listen socket and client sockets are non-blocking. Cron runs every 100ms: expire up to 64 keys, close idle clients, kick `appendfsync everysec`.

Logging goes to stderr. Levels: `debug`, `verbose`, `notice`, `warning`.

Over `maxclients`, the server accepts and immediately closes, after writing `-ERR max number of clients reached`.

## Repository

```
Makefile
LICENSE
README.md
redis.conf
src/        C server
tests/unit  C seam tests
tests/integration/protocol.py
rust/       port, after the C suite is green
```

`README.md` states the support matrix, the non-goals, a Python, Node, and Go client pointed at `127.0.0.1:6379` for protocol 2 and 3, and a foreground service-manager example. Those steps do not install Redis.

## Test seams

Tests call these interfaces and do not reach past them:

- SDS, dict, list, skiplist, expiry heap, SipHash
- RESP codec: split buffers, RESP2 null, RESP3 null, map, bool, double, push
- Commands: `argv` in, reply buffer out, including wrong type, wrong arity, and `OOM`
- Database: store, expire, lazy delete
- Persistence: AOF replay and snapshot bytes, including absolute expiry
- A live process on an ephemeral port: pipelining, `HELLO`, `CLIENT SETINFO`, two pub/sub clients, `MULTI` / `WATCH`, restart, `SHUTDOWN` leaving a loadable AOF

## Builder check

A builder read this document without further conversation. Three questions came back. Each is settled here as a call, because the client contract already decides them.

- Queue-time `EXEC` abort covers an unknown command, a wrong arity, and a syntax error. `WRONGTYPE`, `OOM`, and a bad integer are runtime replies inside the `EXEC` array.
- Both languages count allocator bytes. The OOM test is behavioral.
- `SCAN` during incremental rehash is best-effort, as written under Calls.

## Rust port

Same seams, same command matrix, same AOF text, same `SRDB` bytes. Safe Rust. The C suite stays. The Python suite runs against both binaries.
