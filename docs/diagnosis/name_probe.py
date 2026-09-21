"""Identify the host at 192.168.1.111 via LLMNR (UDP 5355) and mDNS (UDP 5353).

Both are name-resolution protocols that respond with the device hostname.
LLMNR: Windows and systemd-resolved answer on 5355.
mDNS:  avahi/Android/Windows answer on 5353.
"""
import socket
import struct
import random

HOST_IP = "192.168.1.111"


def build_query(name: bytes, qtype: int, unicast: bool) -> bytes:
    tid = random.randint(1, 65534)
    flags = 0 if unicast else 0x0000
    pkt = struct.pack(">HHHHHH", tid, flags, 1, 0, 0, 0)
    for label in name.split(b"."):
        pkt += bytes([len(label)]) + label
    pkt += b"\x00" + struct.pack(">HH", qtype, 0x0001)  # IN class
    return pkt


def ask(port: int, name: bytes, qtype: int, unicast=False, label=""):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3)
    try:
        s.sendto(build_query(name, qtype, unicast), (HOST_IP, port))
        data, _ = s.recvfrom(2048)
        # crude parse: find A record or PTR in answers
        print("%s -> %d bytes: %s" % (label, len(data), data[:120]))
    except socket.timeout:
        print("%s -> no answer" % label)
    except OSError as e:
        print("%s -> error %s" % (label, e))
    finally:
        s.close()


if __name__ == "__main__":
    ask(5355, b"lubancat", 1, True, "LLMNR A lubancat")
    ask(5355, b"lubancat2n", 1, True, "LLMNR A lubancat2n")
    ask(5353, b"_workstation._tcp.local", 12, False, "mDNS PTR workstation")
    ask(5353, b"_ssh._tcp.local", 12, False, "mDNS PTR ssh")
    ask(5353, b"lubancat.local", 1, False, "mDNS A lubancat.local")
