"""LoginGraceTime probe: connect to sshd, send NOTHING, wait.

A healthy sshd closes the pre-auth connection after LoginGraceTime (default
120s), usually announcing it. What the board does here tells us whether the
sshd child survives doing nothing at all.
"""
import socket
import time

HOST, PORT = "192.168.1.111", 22
WAIT = 150

s = socket.create_connection((HOST, PORT), timeout=10)
banner = b""
while not banner.endswith(b"\n"):
    banner += s.recv(256)
print("banner:", banner.split(b"\r")[0].decode(errors="replace"), flush=True)

s.settimeout(WAIT + 10)
start = time.time()
try:
    data = s.recv(4096)
    if data:
        print("after %.0fs got %d bytes: %r" % (time.time() - start, len(data), data[:200]))
    else:
        print("after %.0fs: clean close (EOF), %.0fs elapsed" % (time.time() - start, time.time() - start))
except ConnectionResetError:
    print("after %.0fs: RST" % (time.time() - start))
except socket.timeout:
    print("after %.0fs: still open, no data (sshd ignores LoginGraceTime)" % (time.time() - start))
s.close()
