# GPS 1PPS + NMEA 时间同步落地 SOP（LubanCat 2N 修正版）

> 本文是通用 1PPS+NMEA 授时 SOP 的修正版，针对 LubanCat 2N（Debian Buster）整理。
> 与原 SOP 的主要差异：修正串口 PPS 的错误做法与内核配置项、补充系统服务冲突处理、
> 去掉 NMEA 源的 `noselect`（见 §9）、精度预期修正为几十微秒级。

## 1. 背景与需求分级

本仓库（README、ADR-0002）明确记录：板子**无 RTC 电池，重启后系统时间错乱**，到期判定依赖板载时钟，
因此原设计"不引入 NTP"。GPS 授时恰好可以根治此问题——**不联网也能获得绝对时间**。
但先按需求分级，避免过度设计：

| 目标 | 需要 | 精度 | 复杂度 |
|------|------|------|--------|
| (a) 仅修到期判定 / 时间错乱 | NMEA-only（gpsd + chrony SHM 0） | 秒~毫秒级 | 低（无需 1PPS、无需内核 PPS） |
| (b) 高精度授时（PTP 基准、精确时间戳） | NMEA + 1PPS（本文完整方案） | 几十微秒级 | 高（内核 PPS + 硬件接线） |

**建议**：若目的只是让到期判定正确，走 NMEA-only 即可；有亚毫秒/微秒级需求才走完整方案。
以下按完整方案（b）编写，(a) 只需跳过方案 A/B 与 PPS 相关配置。

## 2. 总体架构

```
接收机 ── NMEA(9600,8N1) ──> UART(/dev/ttySx) ──> gpsd ── SHM ──> chrony ──> 系统时钟
接收机 ── 1PPS 秒脉冲 ──────> GPIO(方案A) 或 串口DCD(方案B) ──> 内核PPS(/dev/pps0) ──> gpsd/chrony
```

- gpsd 解析 NMEA（提供年月日时分秒，绝对时间），并把内核 PPS 时间写入共享内存 SHM；
- chrony 以 SHM 0（NMEA，秒级）与 SHM 1（PPS，高精度）为参考源修正系统时钟；
- 注意：**PPS 时间戳由内核用系统时钟（CLOCK_REALTIME）打标**，因此 PPS 只能"锁稳"时间，
  绝对时间必须由 NMEA 提供（见 §9，这是 `noselect` 不能用的原因）。

## 3. 板级检查清单（接线前完成）

- [ ] **空闲 UART**：GPS 使用的串口必须既不是内核 console（`console=ttyS0` 等，`cat` 会混入内核日志、
      `ldattach` 挂不上），也没有 `serial-getty@ttySx.service`。用 `systemctl list-units | grep getty` 和
      `dmesg | grep console` 确认；占用则改内核 cmdline 或换用其他 UART（以板级设备树为准）。
- [ ] **电平**：接收机输出 TTL 3.3V 可直接接；RS232 电平需 MAX3232 转换。
- [ ] **共地**：接收机与板子必须共 GND。
- [ ] **GPIO（方案 A）**：确认引出的空闲 GPIO 支持中断（`pps-gpio` 靠中断捕获边沿），引脚号以板级 dtsi 为准。
- [ ] **DCD（方案 B）**：确认板子把串口 DCD 引脚引出且 pinmux 配为流控功能——很多板只引 RX/TX，方案 B 不可用。
- [ ] **天线视野**：接收机需锁定（GPRMC 状态 A）才有有效时间，室内测试大概率无信号，先拿到窗边/室外。

## 4. 内核 PPS 支持（先确认，再买硬件/接线）

检查内核配置（多数嵌入式板无 `/proc/config.gz`，用以下方式）：

```bash
grep -E 'CONFIG_PPS' /lib/modules/$(uname -r)/build/.config   # 或厂商内核 out 目录下的 .config
```

需要开启：

| 配置项 | 用途 |
|--------|------|
| `CONFIG_PPS=y` | PPS 核心 |
| `CONFIG_PPS_CLIENT_GPIO=y` | 方案 A：GPIO 接 1PPS |
| `CONFIG_PPS_CLIENT_LDISC=y` | 方案 B：串口行规程 PPS（`ldattach`） |
| `CONFIG_PPS_CLIENT_KTIMER=y` | 建议：无硬件时用 ktimer 假源验证软件管线 |

> ⚠️ Rockchip BSP 默认内核通常未开启 PPS 系列配置，**大概率需要重编内核或补模块**。
> 这是整个方案中风险最高、最耗时的步骤，务必最先确认。

## 5. 方案 A：1PPS 接 GPIO（推荐）

设备树添加 `pps-gpio` 节点（引脚号以板级 dtsi 为准，示例为占位）：

```dts
pps {
    compatible = "pps-gpio";
    gpios = <&gpio0 RK_PB2 GPIO_ACTIVE_HIGH>;   /* 换成实际 bank/引脚 */
    status = "okay";
};
```

重新编译烧录设备树，重启后应出现 `/dev/pps0`。验证：

```bash
ls /sys/class/pps/          # 应看到 pps0
sudo ppstest /dev/pps0      # 每秒一行时间戳 = 正常（需 pps-tools 包）
```

## 6. 方案 B：1PPS 接串口 DCD（修正版）

⚠️ 原 SOP 的 `echo 1 > /sys/class/tty/ttyS0/pps` **在主线内核中不存在**，不要使用。
串口 PPS 的正确做法是挂 N_PPS 行规程：

```bash
sudo ldattach PPS /dev/ttyS0
```

前提：
- 内核开启 `CONFIG_PPS_CLIENT_LDISC`；
- 该 UART 驱动支持 DCD 边沿中断（标准 8250 可）；
- DCD 引脚已引出并配好 pinmux（见 §3）；
- 串口未被 console / getty 占用。

`ldattach` 是前台进程，需常驻，做成 systemd 单元（`/etc/systemd/system/pps-ldisc.service`）：

```ini
[Unit]
Description=Attach PPS line discipline to GPS serial port
After=dev-ttyS0.device

[Service]
Type=simple
ExecStart=/usr/sbin/ldattach PPS /dev/ttyS0
Restart=always

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl enable --now pps-ldisc
```

验证同 §5（`ppstest /dev/pps0`）。

## 7. 软件安装与系统服务冲突处理

```bash
sudo apt update
sudo apt install gpsd gpsd-clients pps-tools chrony
```

安装后**必须处理三个冲突**，否则手动测试/生产运行都会被干扰：

```bash
# 1. Buster 默认时间同步是 systemd-timesyncd，与 chrony 抢 123 端口、互相改时间
sudo systemctl disable --now systemd-timesyncd

# 2. gpsd 包自带 udev 规则 + gpsd.socket，串口设备一出现就自动拉起 gpsd，抢走串口
sudo systemctl mask gpsd.socket

# 3. 确认 GPS 串口没有 serial-getty（见 §3），有则停掉
sudo systemctl stop serial-getty@ttySx.service
sudo systemctl disable serial-getty@ttySx.service
```

## 8. NMEA 测试与 gpsd 启动

先手动验证串口（常见波特率 9600，以接收机为准）：

```bash
stty -F /dev/ttyS0 9600 raw -echo
cat /dev/ttyS0          # 应看到 $GPRMC/$GPGGA 等语句；看不到先查接线/电平/波特率
```

启动 gpsd（`-n` 无客户端也持续读取）：

```bash
sudo gpsd -n /dev/ttyS0 /dev/pps0 -F /var/run/gpsd.sock
```

> 若 `/dev/pps0` 不存在，gpsd 只会提供 NMEA 时间（SHM 0），PPS（SHM 1）缺失——先回 §4/§5/§6。

确认识别：

```bash
gpsmon      # 有 PPS 时会显示 PPS 信息
```

## 9. chrony 配置（修正版）

编辑 `/etc/chrony/chrony.conf`，追加：

```
# NMEA：提供绝对时间（年月日时分秒）。不要加 noselect！
refclock SHM 0 offset 0.5 delay 0.2 refid NMEA
# PPS：高精度秒脉冲
refclock SHM 1 offset 0.0 delay 0.1 refid PPS precision 1e-7
# 允许开机时时间偏差大时直接跳变（无 RTC 电池的板子重启后时间差可能达数年）
makestep 1 3
```

**为什么不能给 NMEA 加 `noselect`**：
- chrony 手册对 `noselect` 的定义是 "Never select this source"——该源不参与选源，也就不会用于校时；
- PPS 的时间戳来自系统时钟本身，时钟差了几年时 PPS 测量出的 offset 仍是 0，**PPS 永远无法纠正错误年份**；
- 绝对时间只能靠 NMEA。若 NMEA 被 `noselect` 排除，无 RTC 板重启后的表现是：`chronyc sources` 显示 PPS 已被选中（`^*`）、offset 正常，但系统时间停在错误年份。
- 因此 NMEA 与 PPS 都参与选源，靠精度差异（PPS `precision 1e-7`）让 PPS 胜出；PPS 未锁定时空窗由 NMEA 兜底（毫秒级，可接受）。
- `precision 1e-7` 是 gpsd 官方明确建议加的：chronyd 无法从 SHM 结构读取精度信息，不写则按默认精度处理，PPS 可能输给 NMEA。
- `offset 0.5`（NMEA）与 `offset 0.0`（PPS）是经验值，稳定运行后按 `chronyc sources` 的实际偏差微调。

重启生效：

```bash
sudo systemctl restart chrony
```

## 10. 验证

```bash
chronyc sources -v      # 期望第一列出现：^* PPS（星号 = 当前选用）
chronyc tracking        # 看 System time 偏移；稳定后典型几十微秒级（几十 µs），
                        # 硬件/中断路径好时可到个位数微秒。达不到"微秒级"预期请先检查中断延迟
dmesg | grep -i pps     # 内核 PPS 事件
ppstest /dev/pps0       # 每秒一行 = 脉冲正常
```

判定要点：
- `chronyc sources -v` 中 `^?` 表示未选通、`^+` 备选、`^*` 当前源；
- 无定位（室内/天线没接好）时两个源都会停摆——先解决 §3 的天线视野。

## 11. 开机自启

### gpsd（编辑 `/etc/default/gpsd`）

```
START_DAEMON="true"
USBAUTO="false"
DEVICES="/dev/ttyS0 /dev/pps0"
GPSD_OPTIONS="-n"
```

```bash
sudo systemctl enable --now gpsd
```

### chrony

```bash
sudo systemctl enable chrony
```

### PPS 设备自动创建

- 方案 A（GPIO）：设备树节点随内核自动创建，无需额外操作；
- 方案 B（串口）：§6 的 `pps-ldisc.service` 已处理（不要再用不存在的 sysfs 方法）。

## 12. 常见问题

| 问题 | 可能原因 | 解决 |
|------|----------|------|
| 没有 `/dev/pps0` | 内核未配置 PPS / 设备树没加节点 / DCD 未引出 | 按 §4 查内核配置；方案 A 查 dts，方案 B 查 DCD 引出 |
| `ppstest` 无输出 | 1PPS 接线错误、电平不匹配、接收机未锁定 | 示波器查 1PPS，检查共地；先让接收机锁定（窗边/室外） |
| NMEA 有输出但 gpsd 不识别 | 串口权限或波特率不对；gpsd.socket 未 mask | 用户加入 dialout 组；确认波特率；§7 第 2 条 |
| chrony 显示 NMEA 但无 PPS | gpsd 未传入 PPS 设备 / SHM 段不对 / 未锁定 | 检查 gpsd 启动参数，`gpsmon` 确认 PPS，确认接收机已锁定 |
| chrony 两个源都 `^?` | 接收机无定位 | 天线拿到窗边/室外，等首次定位（冷启动可能数分钟） |
| 时间偏差大 | NMEA offset 不当 / 时钟初始差太大 | 按 `chronyc sources` 调 offset；确认 `makestep 1 3` 已配 |
| `ldattach` 挂不上 | 串口被 console/getty 占用；内核无 LDISC | §3 检查；§4 开 `CONFIG_PPS_CLIENT_LDISC` |
| 重启后时间仍停在旧年份 | NMEA 被 `noselect`（旧配方）；或 gpsd 未自启 | 用 §9 配置；检查 §11 自启 |

## 13. 与本仓库的关系

- **ADR-0002 决策更新**：本方案若被采纳，"不引入 NTP"的表述需要另行决策更新（README:220、
  ADR-0002 风险条款），本文不代为修改。
- **Web 入口**：`www/control_panel.html` 的「时间服务校验设置」Tab 已落地为 PPS+TOD+chrony
  状态展示 + UT986 接收机模式选择（后端 `src/timesync.cgi.c`/`timesync.c`）：GET `action=status`
  聚合 `chronyc -c sources/tracking` 与 `/run/pps_tod/{status,watchdog.state}`；POST
  `action=setmode` 经 CSRF + 枚举白名单后向 /dev/ttyS7 写固定 `$CFGGNSS`/`$CFGSAVE` 载荷
  （www-data 需属 dialout 组，见 README 部署 Phase 2 第 3b 步，模式掩码/校验和见 timesync.c）。
  「NTP及PTP时间服务配置」占位 Tab 仍待后续。
- **时区**：系统时钟为 UTC。到期判定等业务逻辑若按本地时间比较日期，需注意时区换算（可配 `/etc/timezone` 或代码内处理）。
- **串口占用**：ADR-0003 已删除串口层，本方案接 GPS 是新增的硬件 + 内核 + 系统级改造，与旧串口协议无关。
