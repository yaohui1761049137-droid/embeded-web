"""Dribble test: send the same valid KEXINIT byte-by-byte with TCP_NODELAY.

If sshd replies to dribbled data but resets bulk data, the fault is in the
PC NIC's TCP segmentation offload path (or path MSS), not the board.
"""
import socket
import struct

HOST, PORT = "192.168.1.111", 22


def ssh_string(b: bytes) -> bytes:
    return struct.pack(">I", len(b)) + b


def build_kexinit() -> bytes:
    pkt = b"\x14" + b"\x00" * 16
    for algs in ("curve25519-sha256", "ssh-ed25519", "aes128-ctr",
                 "aes128-ctr", "hmac-sha2-256", "hmac-sha2-256",
                 "none", "none", "", ""):
        pkt += ssh_string(algs.encode())
    pkt += b"\x00" + struct.pack(">I", 0)
    pad = 16 - ((5 + len(pkt)) % 16)
    if pad < 4:
        pad += 16
    pkt = struct.pack(">B", pad) + pkt + b"\x00" * pad
    return struct.pack(">I", len(pkt)) + pkt


def main():
    s = socket.create_connection((HOST, PORT), timeout=8)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    banner = b""
    while not banner.endswith(b"\n"):
        banner += s.recv(256)
    print("banner:", banner.split(b"\r")[0].decode(errors="replace"))

    s.sendall(b"SSH-2.0-KexProbe_1.0\r\n")
    payload = build_kexinit()
    try:
        for i in range(len(payload)):
            s.sendall(payload[i:i + 1])
        reply = s.recv(8192)
        if reply:
            print("dribbled -> reply type=0x%02x len=%d (server ALIVE)" % (reply[0], len(reply)))
        else:
            print("dribbled -> clean close, no data")
    except ConnectionResetError:
        print("dribbled -> RST (still resets)")
    except socket.timeout:
        print("dribbled -> timeout, no reply")
    s.close()


if __name__ == "__main__":
    main()
