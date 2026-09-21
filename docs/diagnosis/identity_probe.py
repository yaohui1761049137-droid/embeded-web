"""Identify the host at 192.168.1.111 via LLMNR (UDP 5355) and mDNS (UDP 5353).

Sends standard name queries and prints any answers (hostname records).
"""
import socket
import struct
import time

TARGET = "192.168.1.111"


def build_llmnr(name: str) -> bytes:
    # LLMNR standard query, single question
    qname = b"".join(bytes([len(p)]) + p.encode() for p in name.split("."))
    return (
        struct.pack(">HHHHHH", 0, 0, 1, 0, 0, 0)
        + qname + b"\x00"
        + struct.pack(">HH", 1, 1)  # A, IN
    )


def build_mdns(name: str, qtype: int) -> bytes:
    qname = b"".join(bytes([len(p)]) + p.encode() for p in name.split("."))
    return (
        struct.pack(">HHHHHH", 0, 0, 1, 0, 0, 0)
        + qname + b"\x00"
        + struct.pack(">HH", qtype, 1)
    )


def ask(sock, addr, payload, label, wait=3):
    sock.sendto(payload, (TARGET, addr[1]))
    sock.settimeout(wait)
    try:
        data, src = sock.recvfrom(2048)
        print("%s <- %s: %s" % (label, src[0], data.hex()))
        return data
    except socket.timeout:
        print("%s -> %s:%d no reply" % (label, TARGET, addr[1]))
        return None


def main():
    import sys
    if sys.platform == "win32":
        pass
    s_llmnr = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s_llmnr.bind(("192.168.1.121", 0))  # force the LAN-facing interface
    except OSError:
        pass
    for name in ("lubancat", "lubancat-2n", "localhost", "debian"):
        ask(s_llmnr, (TARGET, 5355), build_llmnr(name), "LLMNR '%s'" % name)

    s_mdns = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s_mdns.bind(("192.168.1.121", 0))
    except OSError:
        pass
    # mDNS: PTR _services._dns-sd._udp + A for lubancat.local
    ask(s_mdns, (TARGET, 5353), build_mdns("_services._dns-sd._udp", 12), "mDNS services-ptr")
    ask(s_mdns, (TARGET, 5353), build_mdns("lubancat.local", 1), "mDNS lubancat.local A")
    time.sleep(2)  # collect late replies


if __name__ == "__main__":
    main()
