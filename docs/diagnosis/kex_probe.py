"""Raw-socket SSH KEXINIT probe against the board.

Sends a hand-built, minimal SSH_MSG_KEXINIT to see whether sshd responds with
its own KEXINIT (transport OK) or resets the connection.
"""
import socket
import struct

HOST, PORT = "192.168.1.111", 22


def ssh_string(b: bytes) -> bytes:
    return struct.pack(">I", len(b)) + b


def build_kexinit() -> bytes:
    pkt = b"\x14" + b"\x00" * 16  # SSH_MSG_KEXINIT + cookie
    for algs in (
        b"curve25519-sha256",           # kex algorithms
        b"ssh-ed25519",                 # server host key algorithms
        b"aes128-ctr",                  # ciphers client->server
        b"aes128-ctr",                  # ciphers server->client
        b"hmac-sha2-256",               # macs client->server
        b"hmac-sha2-256",               # macs server->client
        b"none",                        # compression client->server
        b"none",                        # compression server->client
        b"",                            # languages client->server
        b"",                            # languages server->client
    ):
        pkt += ssh_string(algs)
    pkt += b"\x00"                      # first_kex_packet_follows = false
    pkt += struct.pack(">I", 0)         # reserved = 0
    # no padding needed for reading the reply; server tolerates it
    return pkt


def main():
    s = socket.create_connection((HOST, PORT), timeout=8)
    banner = b""
    while not banner.endswith(b"\n"):
        chunk = s.recv(256)
        if not chunk:
            break
        banner += chunk
    print("banner:", banner.split(b"\r")[0].decode(errors="replace"))

    s.sendall(b"SSH-2.0-KexProbe_1.0\r\n")
    try:
        s.sendall(build_kexinit())
        reply = s.recv(4096)
        if not reply:
            print("server closed connection after KEXINIT (no data)")
        else:
            print("reply type:", hex(reply[0]), "len:", len(reply))
            print("first 80 bytes:", reply[:80])
    except ConnectionResetError as e:
        print("CONNECTION RESET:", e)
    except socket.timeout:
        print("timeout waiting for reply")
    s.close()


if __name__ == "__main__":
    main()
