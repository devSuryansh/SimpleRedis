#!/usr/bin/env python3
"""A snapshot written by one binary must load in the other."""

import os
import socket
import subprocess
import tempfile
import textwrap


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

    def send(self, *parts):
        self.sock.sendall(encode(parts))

    def _fill(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise EOFError
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
            raise SystemExit(self._line().decode())
        if prefix == b":":
            return int(self._line())
        if prefix == b"$":
            n = int(self._line())
            if n < 0:
                return None
            while len(self.buf) < n + 2:
                self._fill()
            data, self.buf = self.buf[:n], self.buf[n + 2 :]
            return data
        raise SystemExit(f"unexpected {prefix!r}")

    def close(self):
        self.sock.close()


def start(binary, directory):
    conf = os.path.join(directory, "redis.conf")
    with open(conf, "w", encoding="utf-8") as handle:
        handle.write(
            textwrap.dedent(
                f"""\
                bind 127.0.0.1
                port 0
                appendonly no
                dbfilename dump.srdb
                dir {directory}
                loglevel warning
                """
            )
        )
    proc = subprocess.Popen([binary, conf], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    line = proc.stdout.readline().decode()
    if not line.startswith("ready "):
        raise SystemExit(f"{binary} failed: {line!r} {proc.stderr.read().decode()}")
    return proc, int(line.split()[1])


def stop(proc, port):
    try:
        conn = Conn(port)
        conn.send("SHUTDOWN", "SAVE")
        try:
            conn.read()
        except OSError:
            pass
        conn.close()
    except OSError:
        proc.terminate()
    proc.wait(timeout=5)


def expect(cond, message):
    if not cond:
        raise SystemExit(message)


def roundtrip(writer, reader, directory):
    proc, port = start(writer, directory)
    conn = Conn(port)
    conn.send("SET", "k", "v", "PX", "60000")
    expect(conn.read() == "OK", "set")
    conn.send("SAVE")
    expect(conn.read() == "OK", "save")
    conn.close()
    stop(proc, port)
    proc, port = start(reader, directory)
    conn = Conn(port)
    conn.send("GET", "k")
    expect(conn.read() == b"v", f"{reader} did not load {writer}'s snapshot")
    conn.send("PTTL", "k")
    ttl = conn.read()
    expect(isinstance(ttl, int) and 0 <= ttl <= 60000, f"ttl {ttl}")
    conn.close()
    stop(proc, port)


def main():
    c_bin = "src/simpleredis-server"
    rust_bin = "rust/target/debug/simpleredis-server"
    with tempfile.TemporaryDirectory(prefix="sr-cross-") as directory:
        roundtrip(c_bin, rust_bin, directory)
    with tempfile.TemporaryDirectory(prefix="sr-cross-") as directory:
        roundtrip(rust_bin, c_bin, directory)
    print("cross-language snapshots ok")


if __name__ == "__main__":
    main()
