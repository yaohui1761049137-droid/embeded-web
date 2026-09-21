"""Behavior test on the silently-open TCP ports (25/110/143/5355):
send 900B garbage, observe reply/RST/EOF, to see whether they share
sshd's crash-on-data behavior.
"""
import socket

HOST = "192.168.1.111"

for port in (25, 110, 143, 5355):
    try:
        s = socket.create_connection((HOST, port), timeout=5)
        s.settimeout(5)
        s.sendall(b"X" * 900)
        try:
            data = s.recv(1024)
            if data:
                print("port %-5d reply %d bytes: %r" % (port, len(data), data[:80]))
            else:
                print("port %-5d clean EOF after garbage" % port)
        except ConnectionResetError:
            print("port %-5d RST after garbage (same crash signature as sshd)" % port)
        except socket.timeout:
            print("port %-5d timeout, connection stayed open" % port)
        s.close()
    except OSError as e:
        print("port %-5d connect error %s" % (port, e))
