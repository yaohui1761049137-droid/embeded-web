"""KEXINIT size sweep against the board's sshd.

Sends VALID minimal KEXINIT packets of increasing size (padding the algorithm
strings with dummy names) to find whether sshd's reply/reset depends on packet
size or on packet content.
"""
import socket
import struct

HOST, PORT = "192.168.1.111", 22


def ssh_string(b: bytes) -> bytes:
    return struct.pack(">I", len(b)) + b


def build_kexinit(target_payload: int) -> bytes:
    base = []
    for algs in ("curve25519-sha256", "ssh-ed25519", "aes128-ctr",
                 "aes128-ctr", "hmac-sha2-256", "hmac-sha2-256",
                 "none", "none", "", ""):
        base.append(algs)
    pkt = b"\x14" + b"\x00" * 16
    filler = "z" * 200 + ",zz"  # a syntactically valid algorithm name
    # grow the kex field until payload reaches target
    kex = base[0]
    fields = list(base)
    while True:
        fields[0] = kex
        pkt = b"\x14" + b"\x00" * 16
        for f in fields:
            pkt += ssh_string(f.encode())
        pkt += b"\x00" + struct.pack(">I", 0)
        if len(pkt) >= target_payload or len(kex) > 20000:
            break
        kex += "," + filler
    pad = 16 - ((5 + len(pkt)) % 16)
    if pad < 4:
        pad += 16
    pkt = struct.pack(">B", pad) + pkt + b"\x00" * pad
    return struct.pack(">I", len(pkt)) + pkt


def try_size(size):
    s = socket.create_connection((HOST, PORT), timeout=8)
    banner = b""
    while not banner.endswith(b"\n"):
        banner += s.recv(256)
    s.sendall(b"SSH-2.0-KexProbe_1.0\r\n")
    s.sendall(build_kexinit(size))
    verdict = "no-data(closed)"
    try:
        reply = s.recv(8192)
        if reply:
            verdict = "reply type=0x%02x len=%d" % (reply[0], len(reply))
    except ConnectionResetError:
        verdict = "RST"
    except socket.timeout:
        verdict = "timeout(alive)"
    s.close()
    return verdict


if __name__ == "__main__":
    for size in (100, 200, 400, 700, 1000, 1400):
        try:
            print("payload>=%5d -> %s" % (size, try_size(size)))
        except OSError as e:
            print("payload>=%5d -> conn error %s" % (size, e))
