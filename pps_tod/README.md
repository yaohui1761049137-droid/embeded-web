# pps_tod/ — PPS+TOD → chrony 用户态授时栈（路线 1）

本目录是**独立交付的授时子系统源码**，2026-09-21 从交付工作区收编入本仓库，
配合 `deploy/install_timing_stack.sh`（+ `fix_crlf_and_start.sh` 兜底）一步部署。
实测：鲁班猫 2N 无 HDMI 板卡 + UT986 接收机，µs 级锁定
（`chronyc tracking` Reference ID `50505300 (PPS)`，System time ±2µs）。

## 文件清单（10 个，即部署 scp 集合）

| 文件 | 用途 |
|---|---|
| `pps_tod.c` | 主守护进程：读 PPS 边沿（gpiochip 字符设备，内核时间戳）+ NMEA/TOD 串口，配对后写 SysV SHM(0x4e545030) 给 chrony refclock SHM 0；含粗同步引导（偏差 >1s 直写系统钟） |
| `pps_tod_watchdog.sh` / `.conf` / `.service` | 样本断流 30s 有界重启 + 参考丢失 DEGRADED 告警 |
| `pps_tod_rtc_save.sh` / `.service` / `.timer` | RTC 每 6h 持久化（板卡无 RTC 电池） |
| `sanitize-drift.sh` / `.service` | 开机 drift 消毒（防止坏 drift 文件把 chrony 带偏） |
| `chrony-restart.conf` | chrony systemd drop-in（Restart=on-failure，防止 ACL 操作重启 chronyd 后不恢复） |

> ⚠️ 历史交付目录里另有 `pps_tod.c.orig-20260917`（修复前备份）——**不要部署它**。
> 修复前后功能差异见仓库 `docs/feature-implementation-report-2026-09-17.md` §4。

## 校验

- 本仓库 `pps_tod.c`（LF 行尾）：md5 `3f25f24c553094e04f17b01d9bc60a87`
- 历史交付 CRLF 原版：md5 `58e2bdc9d1d7fe35a198e0ed4e7f5987`（二者功能相同，仅行尾差异）

## 默认参数（部署命令必须注意）

- **串口默认 `/dev/ttyS3`**（`pps_tod.c:546`）——本板必须显式 `-t /dev/ttyS7`
  （`install_timing_stack.sh` 的 service 单元已写死）；
- **GPIO 默认 `chip3:line5` = GPIO3_A5**（`pps_tod.c:636`），`-g` 可不传；
- 编译：`gcc -O2 -Wall -o pps_tod pps_tod.c -lpthread`（板上执行）；
- 运行：`pps_tod -D -b 115200 -t /dev/ttyS7`（`-D` 前台运行，由 systemd 托管）。

## chrony 配方（install_timing_stack.sh 自动写入）

```
makestep 0.01 3
refclock SHM 0 refid PPS precision 1e-6 poll 2 delay 0.05
```

验收：`/run/pps_tod/status` 出现 `good=1`；`chronyc sources` 出现 `#* PPS`；
`chronyc tracking` Reference ID `50505300 (PPS)`。硬件接线见
`docs/timing-wiring-2026-09-20/`（GPIO3_A5=pin 11、UART7 M1=pin 35/37、共地）。
