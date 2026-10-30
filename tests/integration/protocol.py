#!/usr/bin/env python3
"""Protocol suite for simpleredis-server. Standard library only."""

import os
import socket
import subprocess
import tempfile
import textwrap


class RespError(Exception):
    pass


def encode(parts):
    out = f"*{len(parts)}\r\n".encode()
    for part in parts:
        if isinstance(part, str):
            part = part.encode()
        out += f"${len(part)}\r\n".encode() + part + b"\r\n"
    return out


class Conn:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port))
        self.buf = b""

    def close(self):
        self.sock.close()

    def send(self, *parts):
        self.sock.sendall(encode(parts))

    def send_raw(self, data):
        self.sock.sendall(data)

    def _fill(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise EOFError("connection closed")
        self.buf += chunk

    def _line(self):
        while b"\r\n" not in self.buf:
            self._fill()
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def read(self):
        while not self.buf:
            self._fill()
        prefix, self.buf = self.buf[:1], self.buf[1:]
        if prefix == b"+":
            return self._line().decode()
        if prefix == b"-":
            raise RespError(self._line().decode())
        if prefix == b":":
            return int(self._line())
        if prefix == b"_":
            self._line()
            return None
        if prefix == b"$":
            n = int(self._line())
            if n < 0:
                return None
            while len(self.buf) < n + 2:
                self._fill()
            data, self.buf = self.buf[:n], self.buf[n + 2 :]
            return data
        if prefix in (b"*", b">", b"%"):
            n = int(self._line())
            if n < 0:
                return None
            if prefix == b"%":
                result = {}
                for _ in range(n):
                    key = self.read()
                    if isinstance(key, bytes):
                        key = key.decode()
                    result[key] = self.read()
                return result
            items = [self.read() for _ in range(n)]
            if prefix == b">":
                return ("push", items)
            return items
        raise RespError(f"bad prefix {prefix!r}")


def start_server(directory, extra=""):
    conf = os.path.join(directory, "redis.conf")
    with open(conf, "w", encoding="utf-8") as handle:
        handle.write(
            textwrap.dedent(
                f"""\
                bind 127.0.0.1
                port 0
                databases 16
                timeout 0
                maxclients 128
                maxmemory 0
                appendonly yes
                appendfsync everysec
                appendfilename appendonly.aof
                dbfilename dump.srdb
                dir {directory}
                requirepass
                loglevel warning
                {extra}
                """
            )
        )
    binary = os.environ.get("SIMPLEREDIS_BIN", "src/simpleredis-server")
    proc = subprocess.Popen(
        [binary, conf],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    line = proc.stdout.readline().decode()
    if not line.startswith("ready "):
        err = proc.stderr.read().decode()
        raise SystemExit(f"server did not start: {line!r} {err}")
    return proc, int(line.split()[1])


def stop(proc, port):
    try:
        conn = Conn(port)
        conn.send("SHUTDOWN")
        try:
            conn.read()
        except (EOFError, RespError, OSError):
            pass
        conn.close()
    except OSError:
        proc.terminate()
    proc.wait(timeout=5)


def expect(cond, message):
    if not cond:
        raise SystemExit(message)


def main():
    with tempfile.TemporaryDirectory(prefix="simpleredis-") as directory:
        proc, port = start_server(directory)
        try:
            conn = Conn(port)
            conn.send_raw(b"*1\r\n$4\r\nPI")
            conn.send_raw(b"NG\r\n")
            expect(conn.read() == "PONG", "split PING")

            conn.send("SET", "a", "1")
            conn.send("GET", "a")
            conn.sock.sendall(encode(["SET", "b", "2"]) + encode(["GET", "b"]))
            expect(conn.read() == "OK", "set a")
            expect(conn.read() == b"1", "get a")
            expect(conn.read() == "OK", "set b")
            expect(conn.read() == b"2", "get b pipelined")

            conn.send("LPUSH", "list", "x")
            conn.read()
            try:
                conn.send("GET", "list")
                conn.read()
                raise SystemExit("expected WRONGTYPE")
            except RespError as exc:
                expect("WRONGTYPE" in str(exc), str(exc))

            conn.send("HSET", "hash", "f", "v")
            expect(conn.read() == 1, "hset")
            conn.send("HELLO", "3")
            hello = conn.read()
            expect(isinstance(hello, dict) and hello.get("proto") == 3, f"hello {hello}")
            conn.send("CLIENT", "SETINFO", "LIB-NAME", "py")
            expect(conn.read() == "OK", "setinfo")
            conn.send("HGETALL", "hash")
            got = conn.read()
            expect(got == {"f": b"v"}, f"resp3 hgetall {got}")

            other = Conn(port)
            other.send("WATCH", "a")
            expect(other.read() == "OK", "watch")
            conn.send("SET", "a", "changed")
            expect(conn.read() == "OK", "change watched")
            other.send("MULTI")
            other.send("SET", "a", "nope")
            other.send("EXEC")
            expect(other.read() == "OK", "multi")
            expect(other.read() == "QUEUED", "queued")
            expect(other.read() is None, "exec aborted by watch")
            conn.send("GET", "a")
            expect(conn.read() == b"changed", "watch kept the other write")

            sub = Conn(port)
            sub.send("SUBSCRIBE", "news")
            confirm = sub.read()
            expect(confirm[0] == b"subscribe", f"subscribe {confirm}")
            conn.send("PUBLISH", "news", "hello")
            expect(conn.read() == 1, "publish count")
            message = sub.read()
            expect(message[0] == b"message" and message[2] == b"hello", f"message {message}")
            sub.close()
            other.close()

            conn.send("SET", "live", "yes", "PX", "60000")
            expect(conn.read() == "OK", "set live")
            conn.send("SET", "scanme", "1")
            conn.read()
            conn.send("KEYS", "scan*")
            keys = conn.read()
            expect(b"scanme" in keys, f"keys {keys}")
            found = set()
            cursor = b"0"
            while True:
                conn.send("SCAN", cursor, "COUNT", "10")
                cursor, batch = conn.read()
                for item in batch:
                    found.add(item)
                if cursor == b"0":
                    break
            expect(b"scanme" in found, f"scan {found}")
            conn.close()
        finally:
            if proc.poll() is None:
                stop(proc, port)

        proc, port = start_server(directory)
        try:
            conn = Conn(port)
            conn.send("GET", "live")
            expect(conn.read() == b"yes", "aof restored live")
            conn.send("PTTL", "live")
            ttl = conn.read()
            expect(0 <= ttl <= 60000, f"ttl {ttl}")
            conn.send("SHUTDOWN")
            try:
                conn.read()
            except (EOFError, OSError, RespError):
                pass
            conn.close()
        finally:
            proc.wait(timeout=5)
        aof = os.path.join(directory, "appendonly.aof")
        expect(os.path.exists(aof) and os.path.getsize(aof) > 0, "shutdown left an aof")
        proc, port = start_server(directory)
        try:
            conn = Conn(port)
            conn.send("GET", "live")
            expect(conn.read() == b"yes", "aof after shutdown")
            conn.close()
        finally:
            stop(proc, port)
    print("protocol ok")


if __name__ == "__main__":
    main()
