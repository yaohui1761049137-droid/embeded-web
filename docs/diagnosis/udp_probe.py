"""UDP service probe against the board: NTP (123), SNMP (161), TFTP (69),
DNS (53), SSDP (1900). NTP matters most: the project is chrony-centric.
"""
import socket
import struct

HOST = "192.168.1.111"


def ntp_client():
    # NTP mode-3 client packet, LI=0 VN=3 Mode=3
    pkt = (b"\x1b" + 47 * b"\x00")
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3)
    try:
        s.sendto(pkt, (HOST, 123))
        data, _ = s.recvfrom(512)
        if len(data) >= 48:
            li_vn_mode = data[0]
            stratum = data[1]
            print("NTP 123: reply %d bytes, LI/VN/Mode=0b%s stratum=%d"
                  % (len(data), format(li_vn_mode, "08b"), stratum))
        else:
            print("NTP 123: short reply %r" % data)
    except socket.timeout:
        print("NTP 123: no answer")
    except OSError as e:
        print("NTP 123: %s" % e)


def snmp_get():
    # SNMPv2c GET sysDescr.0 (1.3.6.1.2.1.1.1.0), community "public"
    oid = b"\x2b\x06\x01\x02\x01\x01\x01\x00"  # 1.3.6.1.2.1.1.1.0
    varbind = b"\x30" + _len(b"\x06" + _len(oid) + b"\x05" + _len(b"\x00"))
    varbind += b"\x06" + _len(oid) + b"\x05" + _len(b"\x00")
    pdus = b"\xa0" + _len(struct.pack(">H", 1686) + b"\x00" + b"\x00" + varbind)
    pkt = b"\x30" + _len(b"\x02\x01\x01" + b"\x04\x06public" + pdus)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3)
    try:
        s.sendto(pkt, (HOST, 161))
        data, _ = s.recvfrom(2048)
        print("SNMP 161: reply %d bytes: %r" % (len(data), data[:160]))
    except socket.timeout:
        print("SNMP 161: no answer")
    except OSError as e:
        print("SNMP 161: %s" % e)


def _len(b: bytes) -> bytes:
    n = len(b)
    if n < 128:
        return bytes([n])
    return bytes([0x80 | 2, n >> 8, n & 0xFF]) if n > 255 else bytes([0x81, n])


def tftp_read():
    # RRQ "etc/passwd" netascii from root — harmless read attempt
    pkt = b"\x00\x01" + b"etc/passwd" + b"\x00" + b"netascii" + b"\x00"
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3)
    try:
        s.sendto(pkt, (HOST, 69))
        data, addr = s.recvfrom(1024)
        if data[:2] == b"\x00\x03":
            print("TFTP 69: DATA opcode, %d bytes (server served the file!)" % len(data))
        elif data[:2] == b"\x00\x05":
            print("TFTP 69: ERROR packet %r" % data[:80])
        else:
            print("TFTP 69: reply %r" % data[:80])
    except socket.timeout:
        print("TFTP 69: no answer")
    except OSError as e:
        print("TFTP 69: %s" % e)


def dns_query():
    # DNS A query for "lubancat" directly at the board's IP
    tid = 0x1234
    pkt = struct.pack(">HHHHHH", tid, 0x0100, 1, 0, 0, 0)
    for label in (b"lubancat",):
        pkt += bytes([len(label)]) + label
    pkt += b"\x00" + struct.pack(">HH", 1, 1)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3)
    try:
        s.sendto(pkt, (HOST, 53))
        data, _ = s.recvfrom(512)
        print("DNS 53: reply %d bytes" % len(data))
    except socket.timeout:
        print("DNS 53: no answer")
    except OSError as e:
        print("DNS 53: %s" % e)


if __name__ == "__main__":
    ntp_client()
    snmp_get()
    tftp_read()
    dns_query()
