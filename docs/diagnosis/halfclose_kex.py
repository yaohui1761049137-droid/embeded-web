"""KEXINIT via half-closed socket (SHUT_WR after sending) - the 'Protocol
mismatch' trick, but with a VALID KEXINIT. Expect: either the server's own
KEXINIT (kex works) or 'Protocol mismatch' (packet malformed) instead of RST.
"""
import socket
import struct
import sys

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


def main():
    s = socket.create_connection((HOST, PORT), timeout=8)
    s.settimeout(8)
    banner = b""
    while not banner.endswith(b"\n"):
        banner += s.recv(256)
    print("[banner]", banner.split(b"\r")[0].decode(errors="replace"))
    s.sendall(b"SSH-2.0-HalfClose_1.0\r\n")
    s.sendall(kexinit_raw())
    try:
        d = s.recv(8192)
        print("[recv]", repr(d[:200]) if d else "clean-close")
        return 0
    except ConnectionResetError as e:
        print("[RESULT] RST:", e)
        return 2
    except socket.timeout:
        print("[RESULT] timeout (connection alive, no reply)")
        return 3
    finally:
        try:
            s.close()
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
