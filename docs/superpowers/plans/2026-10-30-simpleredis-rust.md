# SimpleRedis Rust port implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A safe Rust `simpleredis-server` with the same command matrix, AOF text, and `SRDB` bytes as the C server.

**Architecture:** The same seams as the spec: RESP codec, intrusive-equivalent dict, list, skiplist, expiry heap, command dispatch, AOF, snapshot. Safe Rust. `libc` only for epoll. A counting global allocator feeds `maxmemory`.

**Tech Stack:** Rust edition 2021, `libc` for epoll. No tokio. No Redis crate.

**Spec:** `docs/superpowers/specs/2026-10-30-simpleredis-design.md`

## Global Constraints

- Safe Rust except the epoll calls that `libc` requires. Those calls stay inside the event-loop module.
- Same reply bytes, error strings, `SRDB` layout, and AOF rules as the spec.
- `redis_version` `7.2.4`. `simpleredis_version` `0.1.0`.
- Binary name `simpleredis-server`, built from `rust/`.
- The C tree in `src/` stays. This port does not delete it.
- Commits use `git commit -s` and a Conventional Commits title.
- Do not start until `make test` and `python3 tests/integration/protocol.py` pass against the C binary.

## Review Focus

- An `SRDB` file written by C must load in Rust and the reverse. Pinned by Task 4.
- RESP3 `HGETALL` and `HELLO` stay maps. Pinned by Task 5, the shared Python suite pointed at this binary.
- `WATCH` across two connections discards `EXEC`. Pinned by Task 5.
- `PEXPIREAT` inside an AOF written by C must expire on the Rust process. Pinned by Task 4.
- A write over `maxmemory` is `-OOM` and `DEL` still runs. Pinned by Task 3.

---

### Task 1: Crate and RESP

**Files:**
- Create: `rust/Cargo.toml`, `rust/src/main.rs`, `rust/src/resp.rs`, `rust/src/alloc.rs`

**Interfaces:**
- Produces: `resp::parse(buf) -> NeedMore | Done { val, consumed } | Error`. `resp::encode(val, proto) -> Vec<u8>`. Counting allocator `used_bytes() -> usize`.

- [ ] **Step 1: Write the failing test** in `rust/src/resp.rs` under `#[cfg(test)]`: split `*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n`, null proto 2 `$-1\r\n`, null proto 3 `_\r\n`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cargo test --manifest-path rust/Cargo.toml resp::`
Expected: failure, parse missing.

- [ ] **Step 3: Implement the codec and the counting allocator.**

- [ ] **Step 4: Run test to verify it passes**

Run: `cargo test --manifest-path rust/Cargo.toml resp::`
Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git commit -s -m "$(cat <<'EOF'
feat(rust): add the RESP codec and a counting allocator

The port starts at the wire format so later commands can be checked against the C server.
EOF
)"
```

### Task 2: Containers

**Files:**
- Create: `rust/src/sds.rs`, `rust/src/dict.rs`, `rust/src/list.rs`, `rust/src/skiplist.rs`, `rust/src/pq.rs`, `rust/src/siphash.rs`

**Interfaces:**
- Produces: the same behavior as the C modules. Dict resizes incrementally. Skiplist rank order matches Task 6 of the C plan. SipHash vector `0xa129ca6149be45e5`.

- [ ] **Step 1: Write the failing tests** next to each module. Same cases as the C unit tests.

- [ ] **Step 2: Run test to verify it fails**

Run: `cargo test --manifest-path rust/Cargo.toml`
Expected: failure before the containers exist.

- [ ] **Step 3: Implement the containers in safe Rust.**

- [ ] **Step 4: Run test to verify it passes**

Run: `cargo test --manifest-path rust/Cargo.toml`
Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git commit -s -m "$(cat <<'EOF'
feat(rust): add the core containers

Dict, skiplist, and the expiry heap follow the same rules as the C server.
EOF
)"
```

### Task 3: Server and commands

**Files:**
- Create: `rust/src/server.rs`, `rust/src/db.rs`, `rust/src/commands.rs`, `rust/src/ae.rs`
- Modify: `rust/src/main.rs`

**Interfaces:**
- Produces: the full command matrix, handshake, auth, `maxmemory`, pub/sub, and transactions. Stdout `ready <port>`. Command path is testable without a socket.

- [ ] **Step 1: Write the failing test**

A unit test runs `SET`/`GET`, `INCR` overflow, `HSET` integer reply, `HELLO 3`, and `maxmemory` rejection through the command function.

- [ ] **Step 2: Run test to verify it fails**

Run: `cargo test --manifest-path rust/Cargo.toml commands::`
Expected: failure before dispatch exists.

- [ ] **Step 3: Implement the epoll server and every command in the spec.**

- [ ] **Step 4: Run test to verify it passes**

Run: `cargo test --manifest-path rust/Cargo.toml`
Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git commit -s -m "$(cat <<'EOF'
feat(rust): serve the core command matrix

Clients can point at this binary for the same commands the C server answers.
EOF
)"
```

### Task 4: Durability shared with C

**Files:**
- Create: `rust/src/aof.rs`, `rust/src/snapshot.rs`, `tests/integration/cross_lang.py`

**Interfaces:**
- Produces: AOF and `SRDB` readers and writers. `cross_lang.py` saves with one binary and loads with the other.

- [ ] **Step 1: Write the failing test**

C `SAVE` of `SET k v` plus `PEXPIREAT`, Rust load, `GET` and `PTTL` match. The reverse direction too.

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tests/integration/cross_lang.py`
Expected: non-zero until the Rust loader exists.

- [ ] **Step 3: Implement AOF and SRDB.**

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tests/integration/cross_lang.py`
Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git commit -s -m "$(cat <<'EOF'
feat(rust): load and write the shared durability files

A snapshot from either binary restarts on the other without a Redis install.
EOF
)"
```

### Task 5: Shared protocol suite

**Files:**
- Modify: `tests/integration/protocol.py`, `README.md`

**Interfaces:**
- Consumes: `SIMPLEREDIS_BIN` env var selecting the binary. Default remains the C binary.

- [ ] **Step 1: Write the failing test** by running the existing suite against the Rust binary.

Run: `SIMPLEREDIS_BIN=rust/target/debug/simpleredis-server python3 tests/integration/protocol.py`
Expected: failure until Rust matches every assertion.

- [ ] **Step 2: Fix Rust until that command exits 0. Do not weaken the assertions.**

- [ ] **Step 3: Document both binaries in the README.**

- [ ] **Step 4: Run test to verify it passes**

Run: `cargo test --manifest-path rust/Cargo.toml && SIMPLEREDIS_BIN=rust/target/debug/simpleredis-server python3 tests/integration/protocol.py && python3 tests/integration/cross_lang.py`
Expected: all exit 0.

- [ ] **Step 5: Commit**

```bash
git commit -s -m "$(cat <<'EOF'
test: run the protocol suite against the Rust server

One client contract now covers both binaries.
EOF
)"
```
