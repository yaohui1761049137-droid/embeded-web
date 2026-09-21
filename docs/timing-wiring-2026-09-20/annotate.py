#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""照片标注版接线图生成脚本。

在用户已标注的 LubanCat 2N 板照（board-photo-source.png，含红框/红字标注）上补充：
  - 排针脚号标签（按官方 LubanCat2 (V1-V2) 引脚表）
  - 右侧抽象连线面板（UT986 ↔ 板，4 根授时线 + 电气注意事项）
  - 顶部标题与底部网口角色标注
输出 lubancat2n-timing-wiring-annotated.png。可重复运行。

脚位依据（官方引脚表 + 板照用户框选核对）：
  pin 9  GND（PPS 就近共地）   pin 11 GPIO3_A5（1PPS 输入）
  pin 35 UART7_TX_M1（ttyS7）  pin 37 UART7_RX_M1   pin 39 GND
排针几何（由照片量得）：塑料体 x 757.4..1265.4，20 列 × 25.4 px 栅距，
pin 1 在左端，奇数脚在下排（近观察者），焊盘中心 y ≈ 262。
"""
from PIL import Image, ImageDraw, ImageFont

SRC = 'board-photo-source.png'
OUT = 'lubancat2n-timing-wiring-annotated.png'

# ── 画布：顶部标题条 150px + 照片 + 底部条 70px + 右侧面板 660px ──
TOP, BOT, PANEL_W = 150, 70, 660
photo = Image.open(SRC).convert('RGB')
PW, PH = photo.size                     # 1529 x 1127
W, H = PW + PANEL_W, TOP + PH + BOT     # 2189 x 1347

canvas = Image.new('RGB', (W, H), 'white')
canvas.paste(photo, (0, TOP))
d = ImageDraw.Draw(canvas)

TTF = '/usr/share/fonts/opentype/noto/NotoSansCJK-%s.ttc'
try:
    ImageFont.truetype(TTF % 'Bold', 20, index=2)
    bold_path = TTF % 'Bold'
except OSError:
    bold_path = TTF % 'Regular'

def F(size, bold=False, mono=False):
    idx = (7 if mono else 2)
    return ImageFont.truetype(bold_path if bold else TTF % 'Regular', size, index=idx)

def text(dr, x, y, s, size=18, fill=(20, 20, 20), bold=False, anchor='la', mono=False):
    if isinstance(x, tuple):            # 兼容 text(d, (x, y), s, ...) 写法
        x, s, y = x[0], y, x[1] if isinstance(y, str) else y
    dr.text((x, y), s, font=F(size, bold, mono), fill=fill, anchor=anchor)

def ctext(dr, cx, cy, s, size=18, fill=(20, 20, 20), bold=False, anchor='mm'):
    dr.text((cx, cy), s, font=F(size, bold), fill=fill, anchor=anchor)

# ── 排针几何（照片坐标系，照片粘贴在 (0, TOP)）──
HDR_X0, PITCH, ODD_Y = 757.4, 25.4, 262.0
PLASTIC_TOP, PLASTIC_BOT = 224, 274

def pin_x(n):
    return HDR_X0 + PITCH / 2 + PITCH * ((n - 1) // 2)

# ══ 1. 顶部标题条 ══
d.rectangle([0, 0, W, TOP], fill=(255, 255, 255))
d.line([(0, TOP), (W, TOP)], fill=(180, 180, 180), width=2)
text(d, (36, 26), '鲁班猫 LubanCat 2N 授时链路接线图', 44, bold=True)
text(d, (38, 96), 'UT986 接收机 → 1PPS（GPIO3_A5 = pin 11）+ NMEA（ttyS7 = pin 35/37/39）→ pps_tod → chrony SHM refclock',
     22, fill=(90, 90, 90))
text(d, (W - 30, 40), '2026-09-20 · 照片标注版', 20, fill=(120, 120, 120), anchor='ra')

# ══ 2. 排针脚号标签（照片内，下排奇数脚）══
PANEL_L = PW                            # 照片区右边界
tag_bg, tag_bd = (255, 232, 0), (230, 40, 40)

def pin_tag(dr, cx, y0, y1, lines, pin_list):
    y0, y1 = y0 + TOP, y1 + TOP         # 照片内坐标 → 画布坐标（照片贴在 y+TOP）
    f = F(19, bold=True)
    w = max(f.getlength(s) for s in lines) + 26
    x0, x1 = cx - w / 2, cx + w / 2
    dr.rounded_rectangle([x0, y0, x1, y1], 6, fill=tag_bg, outline=tag_bd, width=2)
    for i, s in enumerate(lines):
        ctext(dr, cx, y0 + (y1 - y0) * (i + 0.5) / len(lines), s, 19, bold=True)
    for n, _ in pin_list:               # 引线：塑料体下缘 → 标签顶
        px = pin_x(n)
        dr.line([(px, PLASTIC_BOT + TOP), (px, y0)], fill=tag_bd, width=2)

# pin 9/11 簇（GND + 1PPS）
pin_tag(d, (pin_x(9) + pin_x(11)) / 2, 284, 332,
        ['pin 9 GND · pin 11 GPIO3_A5', '（1PPS 输入）'],
        [(9, None), (11, None)])
# ttyS7 三根（TX/RX/GND）
pin_tag(d, pin_x(37), 284, 332,
        ['pin 35 TX · pin 37 RX · pin 39 GND', '（ttyS7 串口）'],
        [(35, None), (37, None), (39, None)])

# ══ 3. 网口角色标注（照片下方新白条）══
y_bot = TOP + PH
d.rectangle([0, y_bot, W, H], fill=(255, 255, 255))
d.line([(0, y_bot), (W, y_bot)], fill=(180, 180, 180), width=2)
text(d, (170, y_bot + 14), 'eth2 / eth3 —— 未接线', 19, fill=(130, 130, 130))
text(d, (478, y_bot + 14), 'eth1 = 192.168.1.150/24', 21, bold=True, fill=(200, 60, 40))
text(d, (480, y_bot + 42), 'NTP 服务口（压测 / 客户端）', 16, fill=(100, 100, 100))
text(d, (762, y_bot + 14), 'eth0 = 192.168.137.100/24', 21, bold=True, fill=(200, 60, 40))
text(d, (757, y_bot + 42), '管理口（Web https / SSH）', 16, fill=(100, 100, 100))

# ══ 4. 右侧抽象连线面板 ══
PX0, PY0, PX1, PY1 = PW + 20, TOP, W - 20, TOP + PH
d.rectangle([PX0, PY0, PX1, PY1], fill=(250, 250, 250))
d.rectangle([PX0, PY0, PX1, PY1], outline=(160, 160, 160), width=2)
cx0, cx1 = PX0 + 10, PX1 - 10
ctext(d, (cx0 + cx1) / 2, PY0 + 26, '抽象连线示意（UT986 ↔ 板）', 26, bold=True)

PORT_X0, PORT_X1 = cx0 + 6, cx0 + 226   # UT986 端子框列
PIN_X0, PIN_X1 = cx1 - 190, cx1 - 6     # 板侧脚号框列
rows = [
    dict(port='1PPS OUT',      pin='pin 11  GPIO3_A5',  wire='3.3V TTL',
         sub='1PPS 秒脉冲 → gpiochip 边沿事件（内核时间戳）',
         color=(0, 150, 60),  arrow='right'),
    dict(port='NMEA OUT (TX)', pin='pin 37  UART7_RX',  wire='115200 8N1',
         sub='$GNRMC / ZDA → pps_tod 解析 TOD（秒级绝对时间）',
         color=(20, 90, 200), arrow='right'),
    dict(port='命令 IN (RX)',   pin='pin 35  UART7_TX',  wire='只写命令',
         sub='timesync.cgi 只写 $CFGGNSS/$CFGSAVE（UT986 5 种模式切换）',
         color=(230, 120, 20), arrow='left'),
    dict(port='GND',           pin='pin 9/39  GND', wire='共地',
         sub='接收机与板必须共 GND（照片框选：pin 9 与 pin 39）',
         color=(70, 70, 70),  arrow=None),
]
ctext(d, (PORT_X0 + PORT_X1) / 2, PY0 + 62, 'UT986 接收机', 22, bold=True)
text(d, (PORT_X0 + PORT_X1) / 2, PY0 + 80, '端子按实物丝印标注（占位）', 15, fill=(120, 120, 120), anchor='ma')
ctext(d, (PIN_X0 + PIN_X1) / 2, PY0 + 62, '板侧 40-pin 排针', 22, bold=True)
text(d, (PIN_X0 + PIN_X1) / 2, PY0 + 80, '下排 = 奇数脚（pin 1 在左端）', 15, fill=(120, 120, 120), anchor='ma')

ry0 = PY0 + 108
RH = 122
for i, r in enumerate(rows):
    cy = ry0 + i * RH + 24
    # 端子框（虚线 = 占位）
    dr = d
    for k in range(4):
        dr.rounded_rectangle([PORT_X0 + k*2, cy - 22 + k*2, PORT_X1 - k*2, cy + 22 - k*2],
                             outline=(80, 80, 80), width=1)
    ctext(d, (PORT_X0 + PORT_X1) / 2, cy, r['port'], 20, bold=True)
    # 板侧框（黄底）
    d.rounded_rectangle([PIN_X0, cy - 22, PIN_X1, cy + 22], 6, fill=tag_bg, outline=tag_bd, width=2)
    ctext(d, (PIN_X0 + PIN_X1) / 2, cy, r['pin'], 20, bold=True)
    # 连线
    wy0, wy1 = PORT_X1, PIN_X0
    d.line([(wy0, cy), (wy1, cy)], fill=r['color'], width=5)
    if r['arrow'] == 'right':
        d.polygon([(wy1, cy), (wy1 - 14, cy - 7), (wy1 - 14, cy + 7)], fill=r['color'])
        text(d, (wy0 + wy1) / 2, cy - 22, r['wire'], 17, fill=r['color'], anchor='mb')
    elif r['arrow'] == 'left':
        d.polygon([(wy0, cy), (wy0 + 14, cy - 7), (wy0 + 14, cy + 7)], fill=r['color'])
        text(d, (wy0 + wy1) / 2, cy - 22, r['wire'], 17, fill=r['color'], anchor='mb')
    else:
        text(d, (wy0 + wy1) / 2, cy - 22, r['wire'], 17, fill=r['color'], anchor='mb')
    text(d, (PORT_X0 + PIN_X1) / 2, cy + 30, r['sub'], 17, fill=(70, 70, 70), anchor='ma')

# 图例
ly = ry0 + 4 * RH + 8
text(d, cx0, ly, '图例', 20, bold=True)
ly += 34
d.rectangle([cx0, ly, cx0 + 40, ly + 22], fill=tag_bg, outline=tag_bd, width=2)
text(d, cx0 + 52, ly + 2, '黄色标签 = 本次补充（脚号 / 角色 / IP）', 18)
ly += 32
d.rectangle([cx0, ly, cx0 + 40, ly + 20], outline=(120, 120, 120), width=2)
for k in range(3):
    d.line([(cx0 + k*3, ly), (cx0 + k*3, ly + 20)], fill=(250, 250, 250), width=1)
text(d, cx0 + 52, ly + 2, '虚线框 = UT986 侧端子占位，待按实物丝印补全', 18)
ly += 32
d.rectangle([cx0, ly, cx0 + 40, ly + 20], outline=(230, 40, 40), width=3)
text(d, cx0 + 52, ly + 2, '照片红框 = 板上实际接线点（原始标注保留）', 18)

# 注意事项
ny = ly + 52
for s in [
    '• 电气：信号电平 3.3V TTL，可与板直连（RS232 需电平转换）',
    '• 串口参数 115200 8N1（板端实配：pps_tod -b 115200 -t /dev/ttyS7）',
    '• 内部数据流：gpiochip 边沿事件 + NMEA → pps_tod',
    '  → SysV SHM(0x4e545030) → chrony refclock SHM 0 → 锁钟',
    '• 串口写入方向为“只写”，不读串口（避免抢走 pps_tod 的 NMEA）',
]:
    text(d, cx0, ny, s, 18, fill=(60, 60, 60))
    ny += 30

# 来源
sy = PY1 - 92
d.line([(cx0, sy - 10), (cx1, sy - 10)], fill=(200, 200, 200), width=1)
for s in [
    '来源：README §时间同步子系统 · docs/handoff-2026-09-17.md ·',
    'docs/system-log-audit-2026-09-17.md（运行参数）·',
    '官方 LubanCat2 (V1-V2) 40-pin 引脚表（LubanCat2-V1-40pin.png）',
]:
    text(d, cx0, sy, s, 15, fill=(140, 140, 140))
    sy += 26

canvas.save(OUT)
print('saved', OUT, canvas.size)
