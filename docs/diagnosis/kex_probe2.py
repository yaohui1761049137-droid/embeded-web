"""Broad-algorithm KEXINIT probe: cover legacy + modern SSH algorithms so any
restricted sshd KexAlgorithms/HostKeyAlgorithms list still finds a match.

On success, prints the server's chosen KEXINIT fields so the real client can
be configured with the negotiated algorithms.
"""
import socket
import struct

HOST, PORT = "192.168.1.111", 22

KEX = ",".join([
    "curve25519-sha256", "curve25519-sha256@libssh.org",
    "ecdh-sha2-nistp256", "ecdh-sha2-nistp384", "ecdh-sha2-nistp521",
    "diffie-hellman-group-exchange-sha256", "diffie-hellman-group16-sha512",
    "diffie-hellman-group18-sha512", "diffie-hellman-group14-sha256",
    "diffie-hellman-group14-sha1", "diffie-hellman-group1-sha1",
    "diffie-hellman-group-exchange-sha1",
])
HOSTKEYS = ",".join([
    "ssh-ed25519", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384",
    "ecdsa-sha2-nistp521", "rsa-sha2-512", "rsa-sha2-256", "ssh-rsa",
])
CIPHERS = ",".join([
    "aes128-ctr", "aes192-ctr", "aes256-ctr", "aes128-gcm@openssh.com",
    "aes256-gcm@openssh.com", "chacha20-poly1305@openssh.com", "3des-cbc",
    "aes128-cbc", "aes256-cbc",
])
MACS = ",".join([
    "hmac-sha2-256", "hmac-sha2-512", "hmac-sha1", "umac-64@openssh.com",
    "hmac-sha1-96",
])
COMPRESSION = "none,zlib@openssh.com"


def ssh_string(b: bytes) -> bytes:
    return struct.pack(">I", len(b)) + b


def build_kexinit() -> bytes:
    pkt = b"\x14" + b"\x00" * 16
    for algs in (KEX, HOSTKEYS, CIPHERS, CIPHERS, MACS, MACS,
                 COMPRESSION, COMPRESSION, "", ""):
        pkt += ssh_string(algs.encode())
    pkt += b"\x00" + struct.pack(">I", 0)
    pad = 16 - ((5 + len(pkt)) % 16)
    if pad < 4:
        pad += 16
    pkt = struct.pack(">B", pad) + pkt + b"\x00" * pad
    return struct.pack(">I", len(pkt)) + pkt


def parse_strings(data: bytes, offset: int, count: int):
    out, i = [], 0
    for _ in range(count):
        (n,) = struct.unpack_from(">I", data, i)
        out.append(data[i + 4:i + 4 + n].decode(errors="replace"))
        i += 4 + n
    return out


def main():
    s = socket.create_connection((HOST, PORT), timeout=8)
    banner = b""
    while not banner.endswith(b"\n"):
        banner += s.recv(256)
    print("banner:", banner.split(b"\r")[0].decode(errors="replace"))

    s.sendall(b"SSH-2.0-KexProbe_1.0\r\n")
    s.sendall(build_kexinit())
    try:
        reply = s.recv(8192)
        if not reply:
            print("server closed connection (no data)")
            return
        print("reply type:", hex(reply[0]), "len:", len(reply))
        if reply[0] == 20:
            fields = parse_strings(reply, 17, 10)  # skip type+cookie(17)
            names = ["kex", "hostkey", "cipher_c2s", "cipher_s2c",
                     "mac_c2s", "mac_s2c", "comp_c2s", "comp_s2c",
                     "lang_c2s", "lang_s2c"]
            for name, val in zip(names, fields):
                print("  %-10s %s" % (name, val))
    except ConnectionResetError as e:
        print("CONNECTION RESET:", e)
    except socket.timeout:
        print("timeout waiting for reply")
    s.close()


if __name__ == "__main__":
    main()
