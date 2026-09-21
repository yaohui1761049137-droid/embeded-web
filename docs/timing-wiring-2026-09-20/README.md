# LubanCat 2N 授时链路接线图（2026-09-20）

本目录存放 LubanCat 2N（RK3568）与 UT986 接收机之间授时链路的接线图，共两图一表：

| 文件 | 说明 |
|---|---|
| `lubancat2n-timing-wiring-annotated.png` | **照片标注版**：在实物板照上标注接线点（pin 9/11/35/37/39）、网口角色与右侧抽象连线面板 |
| `timing-wiring-schematic.svg` / `.png` | **原理示意图**：UT986 ↔ 板的信号连线、方向、参数与板内数据流（SVG 为源文件） |
| `annotate.py` | 照片标注版生成脚本（可改坐标/文案后重跑） |
| `board-photo-source.png` | 原始板照（含手工标注，保留未动） |

## 授时接线表（UT986 ↔ LubanCat 2N）

板侧均为 40-pin 排针**下排**（奇数脚），pin 1 在排针左端（靠近丝印 1/2 一侧）：

| # | 信号 | UT986 侧 | 板侧脚号 / 信号名 | 方向 | 电气 / 参数 | 功能 |
|---|---|---|---|---|---|---|
| 1 | 1PPS | 1PPS 输出端（占位） | **pin 11** · GPIO3_A5 | UT986 → 板 | 3.3V TTL 秒脉冲 | `pps_tod` 经 gpiochip 边沿事件读秒沿（内核时间戳），实现"整秒+亚秒"配对 |
| 2 | NMEA | NMEA 输出 TX（占位） | **pin 37** · UART7_RX_M1 | UT986 → 板 | 115200 8N1 | `pps_tod` 自解析 $GNRMC/ZDA 得到 TOD（秒级绝对时间） |
| 3 | 模式切换命令 | 命令输入 RX（占位） | **pin 35** · UART7_TX_M1 | 板 → UT986 | 115200 8N1 | `timesync.cgi` 只写固定载荷 $CFGGNSS/$CFGSAVE（5 种模式切换），**不读串口** |
| 4 | GND | GND（占位） | **pin 9 + pin 39** | — | 共地 | 接收机与板**必须共地**；照片框选为 pin 9（PPS 侧）与 pin 39（串口侧） |

> UT986 侧端子名称为**占位**，待按实物丝印补全；补全后请更新本表与两张图。

## 网口角色

| 网口 | IP | 角色 |
|---|---|---|
| eth0 | 192.168.137.100/24 | 管理口：Web 管理（https）+ SSH，兼做 NTP 注入测试 |
| eth1 | 192.168.1.150/24 | NTP 服务口：NTP 客户端接入 / 压测流量源侧 |
| eth2 / eth3 | 192.168.7.150 / 192.168.6.150 | 未接线 |

## 电气与使用注意

- **共地**：接收机与板必须共 GND，否则 PPS 边沿/NMEA 电平不可靠。
- **电平匹配**：3.3V TTL 可直连 GPIO/UART；RS232 电平需电平转换，禁止直连。
- **串口为交叉连接**：UT986 TX → 板 RX（pin 37），板 TX（pin 35）→ UT986 RX。
- **波特率 115200** 为板端实配（`pps_tod -b 115200 -t /dev/ttyS7`，见 `docs/system-log-audit-2026-09-17.md`）。
- 串口双使用者：`pps_tod` 读 NMEA；`timesync.cgi` 只写。Web 侧写串口依赖 www-data 在 dialout 组。

## 板内数据流（原理图右半部分）

GPIO3_A5 边沿事件（内核时间戳）+ NMEA → `pps_tod`（整秒+亚秒配对）→ SysV SHM（0x4e545030）→ chrony refclock SHM 0 → PLL 锁钟（实测 µs 级，`#* PPS`，Reference ID 50505300）。方案细节见 `README.md` §时间同步子系统 与 `docs/handoff-2026-09-17.md`。

## 脚位依据与来源

- 板侧脚号依据官方 **LubanCat2 (V1-V2) 40-pin 引脚表**（`LubanCat2-V1-40pin.png`：pin 11=GPIO3_A5/编号101，pin 35=UART7_TX_M1/编号116，pin 37=UART7_RX_M1/编号117）。
- 照片上接线点位置由 `annotate.py` 内的几何常量（排针塑料体 x 757.4..1265、栅距 25.4 px）从板照量得，与官方脚位表逐一核对。

## 再生成方法

```bash
cd docs/timing-wiring-2026-09-20
python3 annotate.py                                   # 重新生成照片标注版
python3 -c "import cairosvg; cairosvg.svg2png(url='timing-wiring-schematic.svg', write_to='timing-wiring-schematic.png', scale=2)"
```
