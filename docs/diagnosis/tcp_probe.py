"""TCP-level diagnostics against the board.

1. Scan a handful of common ports.
2. On port 22: banner handshake, then push 900 bytes of garbage and observe
   whether sshd answers with an SSH error text (TCP fine) or RST (transport
   kills big data).
"""
import socket

HOST = "192.168.1.111"
PORTS = [22, 80, 443, 5555, 8080, 8443, 9090, 9999, 5000, 5566, 3333, 1883]


def scan():
    for p in PORTS:
        try:
            s = socket.create_connection((HOST, p), timeout=2)
            print("OPEN  port %d" % p)
            s.close()
        except (ConnectionRefusedError, socket.timeout):
            pass
        except OSError as e:
            print("port %-5d error: %s" % (p, e))


def garbage_22():
    s = socket.create_connection((HOST, 22), timeout=8)
    banner = b""
    while not banner.endswith(b"\n"):
        banner += s.recv(256)
    print("banner:", banner.split(b"\r")[0].decode(errors="replace"))
    s.sendall(b"SSH-2.0-Probe\r\n")
    s.sendall(b"A" * 900)
    try:
        data = s.recv(4096)
        if not data:
            print("port 22: server closed cleanly after 900B garbage")
        else:
            print("port 22 reply after garbage:", data[:200])
    except ConnectionResetError:
        print("port 22: RST after 900B garbage")
    except socket.timeout:
        print("port 22: timeout after 900B garbage")
    s.close()


def http_post(port=80):
    body = b"B" * 900
    req = (
        b"POST / HTTP/1.1\r\nHost: 192.168.1.111\r\n"
        b"Content-Type: application/octet-stream\r\n"
        + ("Content-Length: %d\r\n" % len(body)).encode()
        + b"Connection: close\r\n\r\n"
    ) + body
    s = socket.create_connection((HOST, port), timeout=8)
    s.sendall(req)
    try:
        data = s.recv(2048)
        print("port %d reply:" % port, data[:160])
    except (ConnectionResetError, socket.timeout) as e:
        print("port %d: %s" % (port, e))
    s.close()


if __name__ == "__main__":
    scan()
    garbage_22()
    try:
        http_post(80)
    except OSError as e:
        print("port 80 not usable:", e)
