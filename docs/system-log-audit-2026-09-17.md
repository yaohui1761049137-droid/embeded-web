# 系统日志核验记录（LubanCat 2N）

> 2026-09-17 · 板卡 192.168.137.100（RK3568, Debian 10, kernel 4.19.232）
> 关联：「确认 system 日志功能正常」的板端核验（Phase A1）

## 1. 核验结论速览

| 日志源 | 写入 | 轮转 | 容量 | 判定 |
|---|---|---|---|---|
| `pps_tod`（`/var/log/pps_tod/`） | ✅ 正常 | ✅ 按天 + gzip | 1.4 MB | **健康** |
| lighttpd（`/var/log/lighttpd/`） | ✅ 正常 | ❌ **从未轮转过** | 1.3 MB | 暂不紧急 |
| rsyslog（daemon/syslog/kern…） | ✅ 正常 | ❌ **记账失真，实际未轮转** | **678 MB** | **需要处理** |
| journald | ✅ 正常 | — | 19.6 MB（易失） | 健康 |
| 业务审计 `audit_log`（SQLite） | ✅ 正常 | — | — | 健康，但**无查看界面** |
| `/etc/cron.d/*` | — | — | — | ❌ **从未执行**（cron 包未安装）；已改为 systemd timer，见 §4.1 |

`/var/log` 总占用 **678 MB**，根分区 7.0 G 已用 3.7 G、剩 3.1 G。

## 2. 最严重的问题：hostapd 失败刷屏

`hostapd.service` 处于 `enabled` 且卡在 `activating`，**每 2 秒失败一次**：

```
Sep 17 12:06:40 lubancat hostapd[22322]: Configuration file: /etc/hostapd/hostapd.conf
Sep 17 12:06:40 lubancat hostapd[22322]: Could not open configuration file '/etc/hostapd/hostapd.conf' for reading.
Sep 17 12:06:40 lubancat hostapd[22322]: Failed to set up interface with /etc/hostapd/hostapd.conf
Sep 17 12:06:40 lubancat hostapd[22322]: Failed to initialize interface
Sep 17 12:06:40 lubancat systemd[1]: hostapd.service: Failed with result 'exit-code'.
Sep 17 12:06:40 lubancat systemd[1]: Failed to start Advanced IEEE 802.11 AP and IEEE 802.1X/WPA/WPA2/EAP Authenticator.
```

根因：**`/etc/hostapd/hostapd.conf` 不存在**（该目录下只有 `ifupdown.sh`）。

- 每次失败写 4 行，**约 10 MB/天**持续刷屏；
- `daemon.log` 共 3 474 797 行，其中 **hostapd 相关 1 973 338 行，占 56.8%**。

这是 `/var/log` 膨胀的**主要贡献者**，且与业务无关（本板不需要 AP 功能）。

## 3. 为什么 logrotate 配置在却不轮转

`logrotate.timer` 在正常工作（`NEXT Fri 2026-09-18 00:00`，上次执行 2026-09-17 00:00），配置也在：

```
/etc/logrotate.d/rsyslog
  /var/log/syslog                    → daily,  rotate 7, compress
  /var/log/daemon.log 等一組          → weekly, rotate 4, compress
/etc/logrotate.d/lighttpd
  /var/log/lighttpd/*.log            → weekly, rotate 12, compress
```

但 `logrotate -d` 干跑显示它认为**所有日志都刚轮转过**：

```
considering log /var/log/daemon.log
  log does not need rotating (log has been rotated at 2026-9-16 18:0, that is not week ago yet)
```

而实际情况恰恰相反：

- `/var/log/daemon.log` 是**单个 328 MB 的文件**，头一行是 `Feb 14 18:11:59 …`（2019-02-14 = 镜像构建日期）；
- `/var/log/syslog.1` **323 MB 且未压缩**（`delaycompress` 只压 `.2` 以上）；
- `/var/log/lighttpd/` 目录 mtime 停在 **2019-02-14**，没有任何 `.1`/`.gz` 归档。

**根因：板子没有 RTC 电池，时钟在历史上多次跳变。** 证据：

```
$ who -b
         system boot  2017-01-01 20:00        ← 启动时 RTC 无有效时间
$ systemctl status pps_tod
Sep 17 11:42:21 … /usr/local/bin/pps_tod -D -b 115200 -t /dev/ttyS7
```

`/var/lib/logrotate/status` 里所有条目都是 `2026-9-16-18:0:0`（正是板子上次开机时刻），
说明 logrotate 是在一次**时钟刚被拨正**的时刻执行的，此后它就用这个日期做「距上次轮转多久」的判断，
而实际的文件内容与目录 mtime 证明这些日志**从未真正被轮转过**。
时钟在 2017 / 2019 / 2022-12 / 2026-09 之间反复跳变，logrotate 的日期记账因此完全失真。

## 4. 附带发现：`/etc/cron.d/*` 从未执行（cron 包未安装）

> ⚠️ **本节初版有误，2026-09-17 晚更正**：初版写"`/etc/cron.daily/*` 也全部不执行"，
> 实际核验后发现 **`cron.daily` 一直由 `anacron.timer` 正常驱动**（`/var/spool/anacron/cron.daily`
> 的 mtime 当天有更新）。只有 `/etc/cron.d/*` 受影响。

```
$ systemctl is-active cron
inactive
$ dpkg -l cron
un  cron   <none>   <none>   (no description available)   ← 从未安装，不是"没启动"
$ command -v anacron
/usr/sbin/anacron          ← anacron 装了，且 anacron.timer 是 active
```

两类目录的驱动方式不同，因此后果也不同：

| 目录 | 驱动者 | 实际状态 |
|---|---|---|
| `/etc/cron.daily/*` 等 | `anacron.timer` | ✅ **正常执行**（`logrotate`、`pps-tod-compress` 等都在内） |
| `/etc/cron.d/*` | 需要 cron 守护进程 | ❌ **从未执行** |

**唯一受影响的任务**是 `/etc/cron.d/pps_tod_rtc`（授时子系统的 RTC 每 6h 持久化）。
它与「无 RTC 电池 + 时钟跳变」本应形成恶性循环（时钟不被写回 RTC，下次开机又是错的），
但实际上 RTC 读回始终正确——说明此前有别的机制（或人工）写过它。

`logrotate` 的轮转问题因此**与 cron 完全无关**，纯粹是时钟跳变导致日期记账失真（见 §3）。

### 4.1 处置（2026-09-17）

按仓库既有风格（`ntp-stats.timer`）改为 systemd timer，不引入新的常驻守护进程：

- 新增 `/etc/systemd/system/pps_tod_rtc.service`（oneshot）+ `pps_tod_rtc.timer`
  （`OnCalendar=*-*-* 00/6:00:00`，与原 cron 表达式的 0/6/12/18 点一致）
- 交付件在 `/mnt/c/.../PPS_TOD/pps_tod_rtc.{service,timer}`，取代原来的 `pps_tod_rtc.cron`
- `/etc/cron.d/pps_tod_rtc` 已移出（备份 `/root/pps_tod_rtc.cron.disabled`）
- 实测：手动触发 `systemctl start pps_tod_rtc.service` 退出码 0，
  RTC 从 `20:07:43` 更新到 `20:07:47`；timer 下次触发 2026-09-18 00:00

## 5. 处置与执行记录

建议与执行情况（2026-09-17 晚执行）：

| # | 措施 | 状态 | 实测 |
|---|---|---|---|
| 1 | **停掉 hostapd**（`systemctl disable --now hostapd`） | ✅ **已执行** | daemon.log 立即停止增长（351577346 字节，5 秒后不变）；hostapd disabled + inactive |
| 2 | **清掉历史包袱**：`logrotate -f` 强制轮转 + 压缩巨型归档 | ✅ **已执行** | `/var/log` **694 MB → 83 MB**；根分区可用 **3.1 G → 3.7 G**（回收约 610 MB） |
| 3 | 给刷屏类日志加 `maxsize 上限` | ⬜ **未做**（见下） | — |
| 4 | **修复 RTC 任务**：改为 systemd timer | ✅ **已执行** | 见 §4.1；手动触发退出码 0，RTC 实测更新 |
| 5 | **硬件：加装 RTC 电池** | ⬜ 需硬件改动 | 时钟跳变的根因，未处理 |

### 5.1 第 2 步的实际做法（比"删除"更保守）

没有直接删除历史日志，而是**先规范轮转、再压缩**，保留可回溯性：

```
logrotate -f /etc/logrotate.d/rsyslog   # 当前日志按策略归档（logrotate 自己把 323MB 的
                                        #   syslog.1 压成了 syslog.2.gz，24 MB）
gzip -9 /var/log/daemon.log.1           # 336 MB → 23.6 MB（14:1），耗时 66 秒
```

`daemon.log` 的内容是 `syslog` 的子集（daemon 设施），且 56.8% 是 hostapd 刷屏、
时间戳横跨 2019→2026（坏时钟遗留）——若不想要可以再 `rm` 掉那两个 `.gz`，再省 47 MB。

### 5.2 第 3 步为何暂不做

`maxsize` 是针对"单个服务刷屏把日志撑爆"的兜底。**刷屏源头（hostapd）已停**，
且 `logrotate` 的日期记账在时钟修正后已恢复正常，`maxsize` 的边际价值不大。
若后续要加，位置是 `/etc/logrotate.d/rsyslog` 的 daemon 组。

## 6. 核验方法（可复现）

```sh
ls -lh /var/log/pps_tod/                  # pps_tod 按天 + .gz
ls -la  /var/log/lighttpd/                # 有无 .1/.gz 归档
du -sh  /var/log/*                      | sort -rh | head
df -h /                                   # 根分区余量
logrotate -d /etc/logrotate.d/rsyslog     # 干跑看轮转决策（只读）
cat /var/lib/logrotate/status             # logrotate 的日期记账
systemctl is-active cron; ps -eo cmd | grep -c '[c]ron'
systemctl is-active hostapd               # 是否卡在 activating
grep -c hostapd /var/log/daemon.log       # 刷屏占比
who -b                                    # 启动时刻（RTC 是否有有效时间）
```
