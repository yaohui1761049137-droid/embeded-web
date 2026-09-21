"""TCP payload size sweep: does the RST correlate with client->server bytes?

For each size: connect to sshd, read banner, send banner line + N bytes of
'SSH-2.0-KexProbe' banner-shaped data, then watch for RST/timeout/reply.
"""
import socket

HOST, PORT = "192.168.1.111", 22


def probe(total: int) -> str:
    try:
        s = socket.create_connection((HOST, PORT), timeout=6)
        s.settimeout(6)
        banner = b""
        while not banner.endswith(b"\n"):
            banner += s.recv(256)
        payload = b"A" * total
        s.sendall(payload)
        verdict = "no-data(closed)"
        try:
            data = s.recv(4096)
            if data:
                verdict = "reply %r" % data[:120]
        except ConnectionResetError:
            verdict = "RST"
        except socket.timeout:
            verdict = "timeout(alive)"
        s.close()
        return verdict
    except OSError as e:
        return "conn-err %s" % e


for n in (32, 64, 80, 96, 112, 127, 128, 129, 145, 160, 192, 256, 512, 900):
    print("send %-4d -> %s" % (n, probe(n)))
