#!/usr/bin/env python3
"""test_fake_board.py — scripted fake Remote Board for offline tests.

Creates a pty pair; prints the slave device path on stdout (one line);
then speaks the line protocol on the master side per the given scenario.
The module under test opens the slave path (via
REMOTE_SERIAL_DEVICE_OVERRIDE) and talks through it.

Received commands are logged to <logfile> (one per line) so tests can
assert the exact command sequence — catches accidental wire changes.

Usage: test_fake_board.py <scenario> <logfile>
  ok       — PING→OK, REMOTE_GET_IPV4→OK 10.0.0.1 255.255.255.0 10.0.0.254,
             REMOTE_GET_IPV6→OK fd00::1/64, REMOTE_SET_*→OK
  no_ping  — PING never answered (warmup result must be discarded),
             data commands answered normally
  retry    — first data command unanswered (module must retry), then normal
  garbled  — every data command answered with "XYZZY" (module must
             exhaust its retries)
  silent   — data commands never answered
  err      — REMOTE_SET_IPV4 → "ERR Invalid address" (others OK)
  err6     — REMOTE_SET_IPV6 → "ERR Invalid prefix" (others OK)
"""
import os
import pty
import sys


def main():
    scenario = sys.argv[1]
    logfile = sys.argv[2]

    master, slave = pty.openpty()
    slave_path = os.ttyname(slave)
    print(slave_path, flush=True)

    saw_data = False
    with open(logfile, "w") as log:
        while True:
            try:
                chunk = os.read(master, 1024)
            except OSError:
                break
            if not chunk:
                break
            for line in chunk.decode("ascii", "replace").split("\n"):
                line = line.strip("\r")
                if not line:
                    continue
                log.write(line + "\n")
                log.flush()
                if line.startswith("PING"):
                    if scenario != "no_ping":
                        os.write(master, b"OK\n")
                    continue
                if scenario == "silent":
                    continue
                if scenario == "retry" and not saw_data:
                    saw_data = True
                    continue  # first data command unanswered → module retries
                if scenario == "garbled":
                    os.write(master, b"XYZZY\n")
                elif line.startswith("REMOTE_GET_IPV4"):
                    os.write(master, b"OK 10.0.0.1 255.255.255.0 10.0.0.254\n")
                elif line.startswith("REMOTE_GET_IPV6"):
                    os.write(master, b"OK fd00::1/64\n")
                elif line.startswith("REMOTE_SET_IPV4"):
                    os.write(master,
                             b"ERR Invalid address\n" if scenario == "err"
                             else b"OK\n")
                elif line.startswith("REMOTE_SET_IPV6"):
                    os.write(master,
                             b"ERR Invalid prefix\n" if scenario == "err6"
                             else b"OK\n")
                else:
                    os.write(master, b"ERR Unknown command\n")


if __name__ == "__main__":
    main()
