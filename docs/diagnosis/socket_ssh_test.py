"""Full-duplex socket SSH test against the board.

Runs KEXINIT exchange on a fully-duplex, explicitly-managed socket and
reports whether sshd responds with its own KEXINIT.
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
    try:
        banner = b""
        while not banner.endswith(b"\n"):
            banner += s.recv(256)
        print("[banner]", banner.split(b"\r")[0].decode(errors="replace"))

        raw = kexinit_raw()
        s.sendall(b"SSH-2.0-SocketSSH_1.0\r\n")
        s.sendall(raw)
        try:
            reply = s.recv(8192)
            print("[kex reply] type=0x%02x len=%d %r"
                  % (reply[0], len(reply), reply[:64]))
            print("[RESULT] OK — sshd responded to KEXINIT")
            return 0
        except ConnectionResetError as e:
            print("[RESULT] RST:", e)
            return 2
        except socket.timeout:
            print("[RESULT] timeout waiting for KEX reply")
            return 3
    finally:
        try:
            s.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        s.close()


if __name__ == "__main__":
    sys.exit(main())
