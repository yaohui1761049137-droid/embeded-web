"""Pin down the exact RST trigger on the board's sshd.

Case A: send KEXINIT as TWO TCP segments (len header, then rest).
Case B: send KEXINIT as ONE segment, but SHUT_WR after (half-close, no RST).
Case C: send KEXINIT as ONE segment with SO_LINGER RST close (control).
Case D: two-segment split at 4 bytes, then wait (banner only).
"""
import socket
import struct

HOST, PORT = "192.168.1.111", 22


def ssh_string(b: bytes) -> bytes:
    return struct.pack(">I", len(b)) + b


def kexinit_raw() -> bytes:
    pkt = b"\x14" + b"\x00" * 16
    for algs in (b"curve25519-sha256", b"ssh-ed25519", b"aes128-ctr",
                 b"aes128-ctr", b"hmac-sha2-256", b"hmac-sha2-256",
                 b"none", b"none", b"", b""):
        pkt += ssh_string(algs)
    pkt += b"\x00" + struct.pack(">I", 0)
    pad = 16 - ((5 + len(pkt)) % 16)
    if pad < 4:
        pad += 16
    pkt = struct.pack(">B", pad) + pkt + b"\x00" * pad
    return struct.pack(">I", len(pkt)) + pkt


def banner_and_recv(s, label):
    banner = b""
    s.settimeout(6)
    while not banner.endswith(b"\n"):
        banner += s.recv(256)
    print("%s banner: %r" % (label, banner[:48]))


def main():
    raw = kexinit_raw()
    first = raw[:4]
    rest = raw[4:]

    # Case A: two segments
    try:
        s = socket.create_connection((HOST, PORT), timeout=6)
        banner_and_recv(s, "A(two-seg)")
        s.sendall(first)
        s.sendall(rest)
        try:
            d = s.recv(4096)
            print("A(two-seg) recv:", repr(d[:120]) if d else "clean-close")
        except ConnectionResetError:
            print("A(two-seg): RST")
        except socket.timeout:
            print("A(two-seg): timeout(alive)")
        s.close()
    except OSError as e:
        print("A error:", e)

    # Case B: one segment + half-close
    try:
        s = socket.create_connection((HOST, PORT), timeout=6)
        banner_and_recv(s, "B(halfclose)")
        s.sendall(raw)
        s.shutdown(socket.SHUT_WR)
        try:
            d = s.recv(4096)
            print("B(halfclose) recv:", repr(d[:120]) if d else "clean-close")
        except ConnectionResetError:
            print("B(halfclose): RST")
        except socket.timeout:
            print("B(halfclose): timeout(alive)")
        s.close()
    except OSError as e:
        print("B error:", e)

    # Case C: RST close control
    try:
        s = socket.create_connection((HOST, PORT), timeout=6)
        banner_and_recv(s, "C(rst-ctrl)")
        s.sendall(raw)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                     struct.pack("ii", 1, 0))
        s.close()  # sends RST immediately after data
        print("C(rst-ctrl): done (RST sent by us)")
    except OSError as e:
        print("C error:", e)


if __name__ == "__main__":
    main()
