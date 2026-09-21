"""Probe the silent open ports (25/110/143) with real protocol commands to
identify what is actually listening on 192.168.1.111, plus determinism check
of the sshd garbage response (20 runs).
"""
import socket
import time

HOST = "192.168.1.111"


def smtp_probe():
    s = socket.create_connection((HOST, 25), timeout=5)
    s.settimeout(4)
    hello = s.recv(256)
    s.sendall(b"EHLO probe.local\r\n")
    time.sleep(1)
    try:
        resp = s.recv(1024)
    except socket.timeout:
        resp = b""
    s.close()
    return hello, resp


def pop3_probe():
    s = socket.create_connection((HOST, 110), timeout=5)
    s.settimeout(4)
    hello = s.recv(256)
    s.sendall(b"USER probe\r\n")
    try:
        resp = s.recv(256)
    except socket.timeout:
        resp = b""
    s.close()
    return hello, resp


def imap_probe():
    s = socket.create_connection((HOST, 143), timeout=5)
    s.settimeout(4)
    hello = s.recv(256)
    s.sendall(b"a001 CAPABILITY\r\n")
    try:
        resp = s.recv(512)
    except socket.timeout:
        resp = b""
    s.close()
    return hello, resp


def garbage_reply_rate(runs=20):
    ok = 0
    outcomes = {}
    for i in range(runs):
        try:
            s = socket.create_connection((HOST, 22), timeout=6)
            s.settimeout(4)
            while not s.recv(64).endswith(b"\n"):
                pass
            s.sendall(b"A" * 300)
            d = s.recv(64)
            s.close()
            key = "reply" if d.startswith(b"Protocol") else repr(d[:20])
            outcomes[key] = outcomes.get(key, 0) + 1
            if d.startswith(b"Protocol"):
                ok += 1
        except Exception as e:
            key = type(e).__name__
            outcomes[key] = outcomes.get(key, 0) + 1
    return ok, runs, outcomes


if __name__ == "__main__":
    for name, fn in (("SMTP/25", smtp_probe), ("POP3/110", pop3_probe),
                     ("IMAP/143", imap_probe)):
        try:
            hello, resp = fn()
            print("%-9s hello=%r resp=%r" % (name, hello[:80], resp[:200]))
        except OSError as e:
            print("%-9s error: %s" % (name, e))
    ok, total, outcomes = garbage_reply_rate()
    print("sshd garbage determinism: %d/%d replied, outcomes=%s"
          % (ok, total, outcomes))
