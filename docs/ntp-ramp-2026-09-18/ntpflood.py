# ntflood.py SRC DST RATE_HZ SECONDS — 限速 NTP client 请求注入（忙等节奏）
import socket, sys, time
src, dst = sys.argv[1], sys.argv[2]
rate = float(sys.argv[3]); secs = float(sys.argv[4])
pkt = bytearray(48); pkt[0] = 0x1B
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind((src, 0))
interval = 1.0/rate if rate > 0 else 0
t0 = time.time(); n = 0
while True:
    now = time.time() - t0
    if now >= secs: break
    s.sendto(bytes(pkt), (dst, 123)); n += 1
    if interval:
        nxt = (n) * interval
        while time.time() - t0 < nxt: pass
dt = time.time() - t0
print("SENT %d (%.1f pps avg over %.2fs)" % (n, n/dt, dt))
