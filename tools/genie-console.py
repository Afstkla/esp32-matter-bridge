#!/usr/bin/env python3
"""Talk to the Genie console over the LAN, so debugging survives USB unplug.

    python tools/genie-console.py <device-ip>

The secret comes from $GENIE_SECRET or a prompt; it never crosses the wire.
The device sends a nonce, both sides HMAC-SHA256 it under the secret, and the
client sends back the hex digest. Type commands at the prompt; everything the
device sends — command output and its live log stream — is printed as it
arrives. Ctrl-D or `exit` closes the session.
"""
import getpass
import hashlib
import hmac
import os
import select
import socket
import sys

PORT = 5323


def read_line(sock, buffered):
    """Blocking read of one \\n-terminated line, keeping any bytes past it."""
    while b"\n" not in buffered:
        chunk = sock.recv(512)
        if not chunk:
            raise SystemExit("device closed the connection")
        buffered += chunk
    line, _, rest = buffered.partition(b"\n")
    return line.decode("utf-8", "replace").strip(), rest


def handshake(sock, secret):
    line, rest = read_line(sock, b"")
    if line == "BUSY":
        raise SystemExit("another session already has the console")
    if not line.startswith("CHALLENGE "):
        raise SystemExit(f"unexpected greeting: {line!r}")
    nonce = line.split(" ", 1)[1]
    answer = hmac.new(secret.encode(), nonce.encode(), hashlib.sha256).hexdigest()
    sock.sendall(answer.encode() + b"\n")

    # The device grants three answers to the one nonce, three seconds apart.
    # One is enough here: a human who mistyped can rerun the command.
    line, rest = read_line(sock, rest)
    if line == "OK":
        return rest
    if line in ("RETRY", "DENIED"):
        raise SystemExit("wrong secret")
    raise SystemExit(f"unexpected answer: {line!r}")


def session(sock, pending):
    if pending:
        sys.stdout.write(pending.decode("utf-8", "replace"))
    sys.stdout.write("genie> ")
    sys.stdout.flush()
    while True:
        ready, _, _ = select.select([sock, sys.stdin], [], [])
        if sock in ready:
            data = sock.recv(4096)
            if not data:
                print("\nconnection closed")
                return
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()
        if sys.stdin in ready:
            line = sys.stdin.readline()
            if not line or line.strip() == "exit":
                return
            sock.sendall(line.encode())
            sys.stdout.write("genie> ")
            sys.stdout.flush()


def main():
    if len(sys.argv) < 2:
        raise SystemExit(f"usage: {sys.argv[0]} <ip> [port]")
    host = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else PORT
    secret = os.environ.get("GENIE_SECRET") or getpass.getpass("secret: ")

    try:
        sock = socket.create_connection((host, port), timeout=15)
    except ConnectionRefusedError:
        raise SystemExit(f"{host}:{port} refused — no secret set, or wrong address")
    except (socket.timeout, OSError) as e:
        raise SystemExit(f"cannot reach {host}:{port}: {e}")

    with sock:
        pending = handshake(sock, secret)
        sock.settimeout(None)
        session(sock, pending)


if __name__ == "__main__":
    main()
