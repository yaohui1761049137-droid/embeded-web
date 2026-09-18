# 四项需求完成方案汇总

> 2026-09-17 · 板卡 LubanCat 2N（192.168.137.100 / 192.168.1.150，RK3568 aarch64，Debian 10，kernel 4.19.232）
> 本文汇总以下四项需求的判定、实现方案、验证结果与遗留边界。
> 细节文档：`system-log-audit-2026-09-17.md`（日志核验）、`ntp-monitor.md`（NTP 监控设计）、
> `feature-implementation-report-2026-09-17.md`（实施记录，含五个附录）、`ntp-nic-test-report-2026-09-17.md`（网口流量测试）

---

## 0. 总览

| # | 需求 | 起始判定 | 本轮结果 | 状态 |
|---|---|---|---|---|
| 1 | 确认 system 日志功能正常 | ⚠️ 日志在写，**无查看功能**；且核验发现轮转已失控 | 核验完成 + 新增「系统日志」Tab | ✅ 已部署实测；**日志治理建议未执行** |
| 2 | 实现每个授时网口的授时监测 | ❌ 只有端口级包计数，无授时质量 | 新增采集守护进程 + 客户端明细 + 应答率 + 三档时间窗 | ✅ 已部署实测，含 10k/s 压测验证 |
| 3 | chrony 黑白名单：Web 还是 console | ✅ 已实现，Web 为受管路径 | 边界文档化 + 同网段 allow/deny 共存 | ✅ 已部署实测 |
| 4 | TOD 时码比对 + 连续异常重置 | ⚠️ 核心已实现，有两处缺口 | 四处缺陷修复 + 故障注入验证 | ⚠️ 已部署验证；**±0.5s 绝对相位限制仍在** |

**验收**：`test_gate.sh` **171 项全绿**、`test_frontend.py` 通过、`test_ntp_nic.sh` **25 项全绿**、
浏览器实测 28 项通过、压测与故障注入均已执行。

---

## 1. 需求一：确认 system 日志功能正常

### 1.1 起始判定：日志在写，但没有查看功能，且轮转已失控

| 日志源 | 写入 | 轮转 | 核验时占用 |
|---|---|---|---|
| `pps_tod`（`/var/log/pps_tod/`） | ✅ | ✅ 按天 + gzip + 100MB 封顶 | 1.4 MB（**治理最好**） |
| lighttpd | ✅ | ❌ 从未轮转（目录 mtime 停在 2019） | 1.3 MB |
| rsyslog（daemon/syslog） | ✅ | ❌ **记账失真，实际未轮转** | **678 MB** |

- Web 侧 7 个 Tab **无一日志入口**；`audit_log` 表**只写不读**（`auth_audit_log()` 是唯一 API）
- 三条根因（详见 `system-log-audit-2026-09-17.md`）：
  1. **hostapd 失败刷屏**：`/etc/hostapd/hostapd.conf` 不存在而服务 enabled，每 2 秒失败写 4 行 →
     **约 10 MB/天**；`daemon.log` 347 万行里 **56.8% 是它**
  2. **logrotate 日期记账失真**：板子无 RTC 电池、时钟多次跳变，状态文件里所有条目都是同一次开机时刻，
     `logrotate -d` 因此认为"刚轮转过"而跳过；`daemon.log` 里躺着 2019 年的内容
  3. **cron 服务未运行**：`/etc/cron.d/pps_tod_rtc`（RTC 每 6h 持久化）从未执行，与根因 2 形成恶性循环

### 1.2 实现：新增「系统日志」Tab

**后端 `src/log.cgi.c`（新增）**
- `GET ?action=sources` → 可用日志源列表（含大小/mtime）
- `GET ?action=tail&id=<源>&lines=N&grep=<kw>` → 尾部 N 行
- **安全模型**：路径由固定 id→path 表推导，客户端不参与任何路径构造（穿越在结构上不可能）；
  只读、无 fork/exec；**只读文件尾部 256KB**（300MB 的日志与 3KB 的开销相同）；
  行数上限 500、单行上限 1000 字符；**root-only**（`gate_require_role`）
- `LOGVIEW_ROOT` 可覆盖日志根目录，便于离线测试

**前端**：新增「系统日志」Tab（root 可见），日志源下拉 + 行数（100/200/500）+ 关键字过滤 + 刷新 +
自动刷新开关；元信息行显示路径、字节数、匹配行数、是否被窗口/行数截断。

### 1.3 验证

`test_gate.sh` Test 29-29h（29 项断言）：源列表、日期化文件名、行数上限、grep 过滤、
**只读尾部窗口**（>256KB 文件的头部标记不出现）、缺失文件降级、
**路径穿越与未知 id 拒绝且不泄露内容**、admin 403。
板端实测：四类日志源均可读，`tail` 行数与 grep 过滤正确，穿越被拒。

### 1.4 遗留

日志治理**已于 2026-09-17 晚执行**（详见 `system-log-audit-2026-09-17.md` §4.1 / §5）：

| 措施 | 结果 |
|---|---|
| 停用 hostapd | ✅ daemon.log 立即停止增长（10 MB/天刷屏止住） |
| 规范轮转 + 压缩巨型归档 | ✅ `/var/log` **694 MB → 83 MB**，根分区可用 3.1 G → 3.7 G |
| RTC 任务改为 systemd timer | ✅ 取代从未执行的 `/etc/cron.d/pps_tod_rtc`，实测写入成功 |

**一处更正**：本节初稿写"cron 未运行导致 `/etc/cron.d/*` 与 `/etc/cron.daily/*` 都不执行"，
实际 **`/etc/cron.daily/*` 一直由 `anacron.timer` 正常驱动**；只有 `/etc/cron.d/*` 受影响
（因为 **cron 包从未安装**，不是"没启动"）。

`audit_log` 仍**只写不读**（本轮未选"补业务审计日志查看"）。

---

## 2. 需求二：实现每个授时网口的授时监测

### 2.1 起始判定：只有端口级包计数

原方案逐网口只有 iptables 的 UDP/123 **包计数**；所有授时质量（offset/RMS/stratum/PPS 样本）都是
**全局单实例**。根因是 chrony 3.4 **没有按接口的统计**（`serverstats` 仅 5 个全局计数器）。

### 2.2 关键实测：两条数据通路必须分开

在板子上做了决定性验证，得到一条反直觉但关键的事实：

| 方向 | AF_PACKET 能否看到 | 结论 |
|---|---|---|
| 入向（请求） | ✅ 可见，且能拿到源 IP 与网口 | 用 AF_PACKET |
| 出向（应答） | ❌ **看不到**（注入 9 包、chronyd 实发 9 包、AF_PACKET outgoing=0） | **改用 iptables OUTPUT 计数** |

内核 4.19 无 `PACKET_IGNORE_OUTGOING`（4.20+ 才有），出入向靠 `sockaddr_ll.sll_pkttype` 区分。

### 2.3 实现

**采集守护进程 `ntp_nic_monitor.c`（新增，root 常驻）**
- 单个 `AF_PACKET/SOCK_DGRAM/ETH_P_IP` 套接字，非阻塞排空
- 每口统计：请求数、**有效 NTP（mode 3）数**、应答数（iptables delta）、三档时间窗求和
- 客户端表：`(ifindex, src_ip)` 开放寻址哈希，**1024 槽固定、零动态分配**；每口 Top 32
- 每 5 秒原子写 `/var/db/ntp_nic.json`（tmp+rename）
- iptables 规则 `-C || -I` 幂等创建；**启动时记录基线**，使 `rsp` 与 `req` 同为"自进程启动"口径

**后端 `src/ntpmon.c`**：把守护进程的 JSON **原样嵌入** stats 响应的 `nic` 键（无需写 C 解析器）；
文件缺失降级 `{"ok":false}`；超缓冲降级 `too_large` 而非输出被截断的非法 JSON。

**前端**：新增两块面板
- 「各网口授时服务状态」：网口 / 请求 / 有效 NTP / 应答 / 应答率 / 客户端数（应答率 <95% 标黄、<80% 标红）
- 「客户端明细」：网口**胶囊**筛选 + 客户端 IP / 累计请求 / 累计有效 NTP / 最后活跃

**页级时间范围开关**：近 1 小时 / 近 5 小时 / 近 24 小时，三块面板联动。
控件复用全站已有的 `.board-tab` 连体矩形，**刻意不用网口那套圆胶囊**（时间与网口是两个正交维度）。
守护进程桶放到 1440（覆盖最大档），三档由同一组环形的后缀求和得出；切换是纯前端切片，不重取数据。

### 2.4 验证

- **压测**（详见 §2.5）：10k/s 每口、双口并发 20k/s，**100% 送达、100% 应答、零串扰**
- **监控链路准确性**：用户态 AF_PACKET 抓包数与内核 iptables 计数**逐包相等**
  （10k/s 与 20k/s 下均 100.0%）
- 端到端：注入 → iptables → CSV → CGI JSON → 浏览器 uPlot 实例数据**四层数值一致**
- 浏览器实测：三档逐一切换，柱图增量与 `eth.last_*` 逐项相等、趋势图跨度 = `min(窗口, 可用数据)`、
  状态表与 `nic.*_N` 相等、**切回 1h 数值完全还原**（证明是本地切片）
- `test_gate.sh` Test 30-30d（10 项）+ `test_frontend.py` 面板契约 + `test_ntp_nic.sh` 第 7 节（6 项）

### 2.5 压测结论（eth0 / eth1）

| 场景 | 实际发出 | 内核送达 | chrony 处理 | 应答 |
|---|---|---|---|---|
| eth0 单口 10k/s ×1s | 9973 pps | **10000/10000** | +10000 | **100%** |
| eth1 单口 10k/s ×1s | 9942 pps | **10000/10000** | +10000 | **100%** |
| **双口并发**各 10k/s | 9882 / 9863 pps | **20000/20000** | +20000 | **100%** |
| 20k/s 各口 | 19.9k pps | 40000/40000 | 34526（86%） | 86% |

**实际上限约 34,500 请求/秒（聚合）**，丢包位置已钉死：

```
内核 UDP InDatagrams      +34526   ← 实际投递给 socket
chrony 处理(ntp_hits)     +34526   ← 收到的全部处理
chrony 发出(OutDatagrams) +34526   ← 处理多少回多少
内核 RcvbufErrors          +5474   ← 收包队列溢出丢弃（缺口完全吻合）
```

即 **chronyd 自身从不丢包**，瓶颈是它**单线程的收包循环**来不及排空 socket 队列。
调大 `rmem_max` 只能拖延、不能提高持续速率。

**压测必须限速**：不限速时压测器可达 104k pps，但 10k 包只送达约 50%，
而板端网卡 `rx_dropped/rx_missed/rx_errors` **全为 0** —— 丢失在 PC 侧协议栈/驱动，
全速突发测不出板子能力。

### 2.6 边界

- 守护进程**重启清零全部统计**（含三档窗口），iptables 柱状图不受影响（规则持久）
- 客户端「累计请求」是**累计值**（自首次见到该客户端起）；窗口只决定"哪些客户端被列出"
- 趋势图 24h 上限 = CSV 环 1441 行（磁盘留 7 天，但后端每次只装载 24h）
- 两套计数器的**采样节奏不同**：CSV 面板块最多旧 60 秒，守护进程面板最多旧 5 秒

---

## 3. 需求三：chrony 黑白名单配置（Web 还是 console）

### 3.1 结论：已实现，**Web 是受管路径**

| 通道 | 持久化 | 即时生效 | 入审计 | Web 表格可见 |
|---|---|---|---|---|
| **Web（推荐）** | ✅ 写入 `acl-web.conf` | ✅ 新增即时生效 | ✅ | ✅ |
| console `chronyc allow\|deny` | ❌ 重启即失 | ✅ | ❌ | ❌ |
| console 手改文件 + 重启 | ✅ | 需重启 | ❌ | ✅ |

**Web 路径**：「服务监控及报警」→ NTP 访问控制（root + CSRF）→ `chronyc allow|deny`（即时生效）
+ 持久化 + 审计；删除走改文件 + 重启 chronyd（chrony 3.4 无运行时删规则命令，SIGHUP 是退出信号）。

### 3.2 本轮工作一：边界文档化

`docs/ntp-monitor.md` 新增 **§2.1 Web 与 console 的边界**，明确 console 手工规则的
"不持久、不入审计、表格看不到也删不掉、重启即失"，并给出排查与清理命令。
README 安全模型表补注。

### 3.3 本轮工作二：同网段 allow/deny 共存

**测试时撞出的真实限制**：给已有 allow 的网段加 deny 会被拒（`规则已存在`），
因为 `ntpmon_acl_find()` 只按 CIDR 匹配、不看动作。想翻转网段放行状态必须**先删后加**，
而删除会重启 chronyd（计数器归零 + 约 1 分钟重锁 PPS）。

chrony 本身支持共存（**等前缀长度下 deny 优先**），所以这是 Web 层的限制。改动五处：

| 位置 | 改动 |
|---|---|
| `ntpmon.h` / `ntpmon.c` | `ntpmon_acl_find()` 增加 action 参数（NULL = 只比 CIDR，兼容旧调用） |
| `ntpmon.c` `ntpmon_acl_apply()` | 传参由"仅 add 带动作"改为"动作非空就带"，remove 也能定向下发 |
| `ntpmon.cgi.c` | 查重改为按 (action, CIDR)；remove 可带动作并校验，不带时用匹配到的规则动作补全 |
| `chrony_acl_apply.sh` | `remove [allow\|deny] <CIDR>`，不带动作退回旧的"allow 优先"行为 |
| `control_panel.html` | 删除按钮与确认框携带动作（`aclRemove(action, cidr)`） |

### 3.4 验证

- 一步加 `deny 192.168.1.0/24`（allow 已存在）→ **成功，且 chronyd 启动时间未变、`serverstats` 未归零**
- `accheck`：被禁网段 209 denied，其他网段 208 allowed
- UI：同网段两行共存（allow 绿 `rgb(74,138,90)` / deny 红 `rgb(196,80,80)`），
  **删除按钮各自携带动作**，不会误删另一行
- 拦截实效：注入 10 包 → `OUTPUT` 与 `ntp_hits` 均不增长
- 清理：`remove deny` → 仅删 deny、allow 保留
- `test_gate.sh` 新增 5 项断言（28h / 28h2 / 28i / 28i2 / 28i3）

### 3.5 边界

删除**仍然**会重启 chronyd（chrony 3.4 无运行时删规则命令）；表格会出现同网段两行，
需理解"等前缀下 deny 优先"。

---

## 4. 需求四：TOD 时码比对 + 连续异常重置

### 4.1 起始判定：核心已实现，有四处缺口

已实现：每边沿比对 offset；三层仲裁；bootstrap 在 |偏差|>1s 时 `clock_settime`；看门狗断流重启。
缺口（核实源码 `/mnt/c/.../PPS_TOD/pps_tod.c`）：

1. **没有"连续比对异常计数 → 触发"**：`strike` / `strike_pause_mono` 是删旧 ±1s 自校准 toggle 后
   遗留的**死变量**，全文无引用
2. **0.5~1.0s 死区**：样本被门控丢弃（>0.5s）而 coarse 路径需 >1.0s —— 落在该区间的时钟误差
   既不进 SHM 也不被纠正，看门狗重启也无法自愈
3. **`bts.tv_nsec = 0`** 使 coarse 步进只能写到整秒，固有 0~1s 误差
4. **coarse 路径无仲裁**：样本路径有 `|S_round − S_chain| ≤ 1s` 检查，coarse 路径直接信任 `anchor_S`

### 4.2 实现（`pps_tod.c`，改动 +93 / −24 行）

| 改动 | 说明 |
|---|---|
| **锚点序列仲裁** | 新增 `anchor_seq`：锚的秒值**连续递增 ≥3 次**才允许写钟；源跳变时绝不写钟 |
| **死区自愈** | 门控时 `strike++`（并带**符号一致性保护**：偏差符号翻转即清零），
正常入样清零；`strike ≥ -M`（默认 10）触发强制步进 |
| **步进动作** | 保持**整秒对齐**（`tv_sec = anchor_S + edge_delta − 1`, `nsec = 0`）——
`nsec=0` 并非粗疏，它同时承担**强制相位归零**的职责 |
| **小时配额** | `-X`（默认 6/h）超限转 `reset_state=CAPPED`，**停止写钟**并告警，防错误参考把时钟来回拖 |
| **状态可见** | `/run/pps_tod/status` 新增 `anchor_seq`、`consec_gated`、`forced_resets`、`reset_state` |
| **CLI** | `-M <n>` 连续门控阈值、`-X <n>` 小时步进上限 |
| **注释修正** | 头注释与行内注释改为与实现一致（原来三处互相矛盾：`>1.5s` / `>0.5s` / 实现 `>1.0s`） |

### 4.3 验证（故障注入，天线接回后执行）

| # | 注入 | 期望路径 | 实测 |
|---|---|---|---|
| 1 | −0.7s | stuck（连续门控 ≥10） | `gated ... consec=1..10` → `forced clock step by +0.702s (anchor_seq=49 resets=1/h)` → 收敛到 −32µs ✅ |
| 2 | −1.5s | urgent（\|偏差\|>1.0s） | `gated (off 2.503s, consec=1)` → **2 秒后** `coarse clock step by +1.502s` ✅ |
| 3 | 配额闸（`-X 1 -M 5`） | 超限转 CAPPED 且不再写钟 | 第 2 次注入 `consec` 涨到 26 而 `forced_resets` 仍为 1、状态 CAPPED，**时钟不再被写** ✅ |
| 4 | 正常态回归 | 无门控无步进 | 连续 45 秒 `consec=0 resets=0`、`#* PPS` Reach 377 ✅ |

### 4.4 排查中我引入并修掉的缺陷（如实记录）

1. **"相位保持步进"是错的**：曾把动作改成 `clock_settime(now + off)`，但 `off` 来自
   `S_round = round(边沿本地时间)`、自带 ±0.5s 量化，当增量应用会**保留甚至放大**相位误差 ——
   实测表现为恢复服务后出现 **±1.000s 交替步进循环**。已改回整秒对齐并加符号翻转保护。
2. **用 `anchor_S − now_rt` 当偏差是错的**：NMEA 语句在秒内某时刻到达，该量混入了**到达相位**，
   正确时钟也会算出非零值。已改用基于 **PPS 边沿**的实测偏差。

### 4.5 遗留：`S_round = round()` 造成的绝对相位限制（**先于本轮存在**）

独立测量（Windows `w32tm`，NTP 协议含 RTT 补偿）与板端自述长期不一致：

| 时刻 | 板端自述 | 独立实测 |
|---|---|---|
| 15:42 | `good=1 offset_us=31`、chrony `System time 0.9µs`、`#* PPS` | +0.512s |
| 18:56 | `good=1 offset_us=87`、chrony `System time 3.0µs`、Reach 377 | −1.096s |

机理：`S_round = round(edge_local_time)` 把边沿的本地时间归到**最近整秒**，于是
`off = S_round − rt_e` 恒落在 ±0.5s 内 —— **无论真实相位误差多大**。chrony 忠实锁定这个自洽参考，
系统稳定在一个**整体偏移可达 ±0.5s** 的状态并报告 µs 级健康。

**含义**：交付文档中"µs 级"的表述是 **chrony 内部自洽口径**，不等于**绝对 UTC 精度**。
板子与 TOD 源的**整秒**保持一致（需求要求的"和外部 TOD 保持一致"成立），但亚秒相位有约 ±0.5s 的锁定不确定性。

> ⚠️ **测量方法的重要更正**：上表"独立实测"一栏是以 **PC 时钟**为基准的，而
> **PC 的 Windows 时间服务未运行（`0x80070426`），PC 自身快约 0.91 秒**
> （对 `ntp.aliyun.com` / `time.windows.com` / `pool.ntp.org` 三个独立源一致测得）。
> 换算到真实 UTC 后：15:42 约 **−0.39s**、18:56 约 **−0.19s** —— 方向不变（板子始终偏慢，
> 符合 `round()` 取整的系统性偏置），但绝对量级此前说大了约 0.9 秒。
> **后续绝对时间测量必须先修好 PC 的时间服务，或另找可信基准。**

**建议的后续修复方向**（本轮未做，需另行设计验证）：让 `S_chain`（NMEA 绝对秒 + 边沿计数推出的真值）
在 `anchor_seq` 良好时**参与相位校正**，而不是仅在 `|S_round − S_chain| > 1` 时才启用。
这样亚秒误差才能被持续消除，绝对精度才真正由 TOD 源决定。

---

## 5. 全部改动清单

### 5.1 仓库（`embeded_Lighttpd/`）

| 文件 | 类型 | 说明 |
|---|---|---|
| `src/log.cgi.c` | 新增 | 系统日志查看 CGI（需求一） |
| `ntp_nic_monitor.c` | 新增 | 逐网口授时采集守护进程（需求二） |
| `ntp-nic-monitor.service` | 新增 | 守护进程 systemd 单元（需求二） |
| `src/ntpmon.c` | 修改 | 嵌入 `nic` 块；`eth.last_1h/5h/24h`；`ntpmon_acl_find` 带动作（需求二、三） |
| `src/ntpmon.h` | 修改 | 同上签名 |
| `src/ntpmon.cgi.c` | 修改 | ACL 查重按 (action, CIDR)；remove 动作解析（需求三） |
| `chrony_acl_apply.sh` | 修改 | `remove [allow\|deny] <CIDR>`（需求三） |
| `www/control_panel.html` | 修改 | 日志 Tab、授时状态与客户端明细两块面板、时间范围开关、图表轴标注（需求一、二） |
| `test_gate.sh` | 修改 | `assert_not_contains` + Test 29（29 项）+ Test 30（10 项）+ ACL 共存 5 项 |
| `test_frontend.py` | 修改 | log.cgi 契约、面板结构、时间范围控件断言 |
| `test_ntp_nic.sh` | 新增 | 逐网口 NTP 流量端到端测试（25 项） |
| `docs/system-log-audit-2026-09-17.md` | 新增 | 日志核验记录（需求一） |
| `docs/ntp-monitor.md` | 修改 | §2.1 Web/console 边界、§2.2 同网段共存（需求三） |
| `docs/feature-implementation-report-2026-09-17.md` | 新增 | 实施记录 + 六个附录 |
| `docs/ntp-nic-test-report-2026-09-17.md` + 证据目录 | 新增 | 网口流量测试报告与原始证据 |
| `README.md` | 修改 | 功能列表、部署清单、测试章节、安全模型 |

### 5.2 仓库外

| 文件 | 说明 |
|---|---|
| `/mnt/c/.../PPS_TOD/pps_tod.c` | TOD 比对四处缺陷修复（+93/−24），原件备份 `pps_tod.c.orig-20260917` |

### 5.3 板端已部署

| 路径 | 版本（md5） |
|---|---|
| `/usr/local/bin/ntp_nic_monitor` | `236103e7bc6791400f7bef846b68e21a` |
| `/usr/local/bin/pps_tod` | `63c0496a1be48f341654f8bdaa77ffda`（备份 `pps_tod.bak-1789630360`） |
| `/home/www/cgi-bin/ntpmon.cgi` | `fdff50932acb2700dc9c3932ca53a000` |
| `/home/www/cgi-bin/log.cgi` | `66061f441ec1ef262d33c4fc9d3722ba` |
| `/etc/systemd/system/ntp-nic-monitor.service` | 已 enable --now |

---

## 6. 验收记录

| 项目 | 结果 |
|---|---|
| `./test_gate.sh` | **171 项全绿**（原 120 → 新增 51） |
| `python3 test_frontend.py .` | 通过 |
| `./test_ntp_nic.sh` | **25 项全绿** |
| 浏览器实测 | 三档时间范围切换 17 项 + deny UI 联动 11 项，全过 |
| 压测 | 10k/s 每口 / 双口并发 20k/s：100% 送达、100% 应答、零串扰 |
| 故障注入 | 0.7s 死区自愈、1.5s 快速通道、配额闸、正常态回归，4 项全过 |
| 板端终态 | `chrony` / `pps_tod` / `pps_tod_watchdog` / `ntp-stats.timer` / `ntp-nic-monitor` 全 active；
授时 `good=1`、`anchor_seq=235`、`#* PPS` Reach 377；ACL 恢复原 4 条 allow |

---

## 7. 遗留事项

| # | 事项 | 归属 | 说明 |
|---|---|---|---|
| 1 | ~~日志治理未执行~~ → **已执行** | 需求一 | hostapd 已停、`/var/log` 694→83MB、RTC 任务改 systemd timer；仅剩"加 `maxsize` 兜底"与"加装 RTC 电池"两项未做 |
| 2 | **`audit_log` 只写不读** | 需求一 | 本轮未选，可在日志 Tab 扩展一个数据源 |
| 3 | **`S_round` 绝对相位限制** | 需求四 | ±0.5s 锁定不确定性；修复方向见 §4.5，需单独设计验证 |
| 4 | **PC 时钟不可信** | 测量方法 | Windows 时间服务未运行、快约 0.91s；修好前所有绝对时间对比都要换算 |
| 8 | `logrotate` 未加 `maxsize` 兜底 | 需求一 | 刷屏源头已停，边际价值不大；位置在 `/etc/logrotate.d/rsyslog` 的 daemon 组 |
| 9 | 板子**无 RTC 电池** | 需求一 | 时钟跳变的硬件根因，需硬件改动 |
| 10 | `watchdog.log` 无留存 → **已修** | 需求一 | 原先无轮转无上限（降级期约 230 KB/天）；已改为脚本内建容量封顶 `LOG_MAX_KB=512`/`LOG_KEEP_LINES=2000`，另加 `--prune-only` 入口。详见 PPS_TOD 交付总结 §6 |
| 5 | 客户端计数未窗口化 | 需求二 | 需给 1024 槽各配桶数组（1h 约 +245KB、5h 一分钟精度约 +1.2MB），收益有限 |
| 6 | 守护进程重启清零统计 | 需求二 | 设计性质；若需跨重启连续需改为落盘累计 |
| 7 | 仓库改动未提交 | 全部 | 见 §5.1，由用户决定提交策略 |
