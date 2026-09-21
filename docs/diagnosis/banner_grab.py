"""Grab protocol banners from the board's open ports to identify the host."""
import socket

HOST = "192.168.1.111"
PORTS = [22, 25, 110, 143, 5355]


def grab(port):
    try:
        s = socket.create_connection((HOST, port), timeout=4)
        s.settimeout(3)
        try:
            data = s.recv(512)
        except socket.timeout:
            data = b""
        if port == 22:
            s.sendall(b"SSH-2.0-Probe\r\n")
            try:
                data += s.recv(512)
            except (ConnectionResetError, socket.timeout):
                pass
        s.close()
        return data[:300]
    except OSError as e:
        return "ERR %s" % e


for p in PORTS:
    out = grab(p)
    if isinstance(out, bytes):
        print("port %-4d %r" % (p, out))
    else:
        print("port %-4d %s" % (p, out))
