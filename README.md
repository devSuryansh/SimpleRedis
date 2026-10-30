# SimpleRedis

`simpleredis-server` is a single-instance in-memory server for applications that speak the Redis protocol. Point a normal client at it. You do not install Redis.

Default address: `127.0.0.1:6379`.

```bash
make
./src/simpleredis-server redis.conf
```

The process stays in the foreground and logs to stderr. A service manager should run that command directly. `SIGINT` and `SIGTERM` persist the active mode, then exit.

The command matrix, reply bytes, and file formats are specified in [docs/superpowers/specs/2026-10-30-simpleredis-design.md](docs/superpowers/specs/2026-10-30-simpleredis-design.md).

## Support

Supported: strings, lists, hashes, sets, sorted sets, expiry, `MULTI`/`EXEC`/`WATCH`, pub/sub, `HELLO` 2 and 3, AOF or a snapshot (`SRDB`), optional `requirepass`, and a `maxmemory` limit that rejects growing writes.

Not supported, and rejected with `-ERR unknown command`: replication, cluster, sentinel, streams, Lua, modules, ACL user lists, TLS, bitops, geo, and HyperLogLog. This server does not load files written by Redis.

On-disk files are specific to SimpleRedis. An AOF or `SRDB` snapshot written here restarts here. Moving an existing Redis dataset is a one-time copy through a client.

## Clients

Point a normal client at `127.0.0.1:6379`. Protocol 2 and protocol 3 both work.

Python:

```python
import redis
client = redis.Redis(host="127.0.0.1", port=6379, protocol=3)
client.set("k", "v")
```

Node:

```javascript
import { createClient } from "redis";
const client = createClient({ url: "redis://127.0.0.1:6379" });
await client.connect();
await client.set("k", "v");
```

Go:

```go
rdb := redis.NewClient(&redis.Options{
    Addr:     "127.0.0.1:6379",
    Protocol: 3,
})
```

A service manager runs the binary in the foreground:

```
ExecStart=/usr/local/bin/simpleredis-server /etc/simpleredis/redis.conf
Restart=on-failure
```

`SIGINT` and `SIGTERM` persist the active mode, then exit.

## Tests

```bash
make test
python3 tests/integration/protocol.py
```

The integration suite uses only the Python standard library. Set `SIMPLEREDIS_BIN` to point it at another binary.
