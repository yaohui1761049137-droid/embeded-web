# NTP 监控设计说明（服务监控及报警 Tab）

> 2026-09-16 · 关联：`ntpmon.cgi`（后端）、`control_panel.html` 的 `tab-monitor`（前端）、
> `chrony_acl_apply.sh` / `ntp_stats_sample.sh`（板端）、README 部署 Phase 2 步骤 3c

## 1. 功能与数据源总览

| 功能 | 数据源 | 说明 |
|---|---|---|
| 黑白名单表格（allow/deny） | `/etc/chrony/acl-web.conf`（托管文件） | 表格只显示**由本系统管理**的规则；他人手工 `chronyc allow` 的运行时规则不在表内 |
| 请求量趋势（近 24h） | `chronyc -c serverstats` 的 `ntp_hits`（每分钟采样） | 折线 = 每分钟请求数（差分）；浅色线 = 累计值（右轴）。chronyd 重启使计数器归零 → 曲线断点 |
| 各网口请求量（柱状图） | iptables 每网口 `udp/123` 计数（每分钟采样） | 精确到网口、**含被 ACL 拒绝的请求**；累计值（规则生效以来）+ 近 1h 增量。设备重启后清零 |
| 采样历史 | `/var/db/ntp_stats.csv` | `epoch,ntp_hits,ntp_drops,cmd_hits,cmd_drops,log_drops,e0,e1,e2,e3`，保留 7 天 |

chrony 3.4 **没有**按网口/接口的统计（`chronyc help` 无接口命令；`serverstats` 仅 5 个全局
计数器且无发出包数）。按客户端 IP 的计数（`chronyc clients`）只收录通过 ACL 的客户端、
无时间戳且重启清零，因此柱状图选择 iptables 计数路线（用户确认）。

## 2. 访问控制语义（基于 chrony 3.4 源码核实）

- **运行时即时生效**：`chronyc allow|deny <CIDR>` 走 `NCR_AddAccessRestriction`，立即影响
  后续请求，**无需重启**。→ 新增规则用此路径。
- **运行时不持久**：重启 chronyd 即丢失 → 每次新增同时写入托管文件。
- **无"删除/列出"命令**：3.4 没有运行时的规则删除或列表导出 → **删除规则 = 改写文件 +
  `systemctl restart chrony`**（从配置重载，唯一确定性路径）。代价：约 1 分钟重新锁定
  `#* PPS`、请求计数归零（前端确认框与图表断点已分别提示/处理）。
- **SIGHUP 是退出信号**（与 SIGINT/SIGTERM/SIGQUIT 同一 handler），**绝不能用于 reload**。
- 权限：allow/deny 属非监控命令，仅允许 **root 经本地 unix socket** 执行 → Web 侧经
  `sudo -n` + sudoers 白名单调用 `chrony_acl_apply.sh`（助手内二次校验 CIDR）。
- 验证工具：`chronyc accheck <IP>` 返回 `208 Access allowed` / `209 Access denied`
  （exit code 恒为 0，需解析文本）——部署验收用。

### 2.1 Web 与 console 的边界（运维必读）

黑白名单**两条路都能改，但只有 Web 是受管路径**：

| 通道 | 命令 | 持久化 | 即时生效 | 入审计 | 在 Web 表格可见 |
|---|---|---|---|---|---|
| **Web（推荐）** | 「服务监控及报警」→ NTP 访问控制 | ✅ 写入 `acl-web.conf` | ✅ 新增即时生效 | ✅ `ntp_acl_change` | ✅ |
| console 新增 | `chronyc allow\|deny <CIDR>` | ❌ 重启即失 | ✅ | ❌ | ❌ **不可见** |
| console 持久 | 手改 `acl-web.conf` + `systemctl restart chrony` | ✅ | 需重启 | ❌ | ✅ 可见 |

**已知运维陷阱**：在 console 上直接 `chronyc allow/deny` 加的规则是**运行时**的 ——
它不在 `/etc/chrony/acl-web.conf` 里，因此

1. **Web 表格看不到它**（表格只显示由本系统管理的规则）；
2. **Web 删不掉它**（删除走的是改文件 + 重启，重启后该规则自然消失，但删除操作本身会报"规则不存在"）；
3. **chronyd 一重启就丢**。

于是可能出现「`chronyc accheck` 说拒绝、但 Web 表格里找不到对应规则」的困惑。
排查与清理：

```sh
chronyc accheck <IP>                    # 208 allowed / 209 denied —— 看当前实际生效的结果
cat /etc/chrony/acl-web.conf            # Web 管理的规则（持久层）
chronyc serverstats                     # 顺带确认 chronyd 未重启（计数器是否归零）
systemctl restart chrony                # 清理所有 console 临时规则（会清零计数、重锁 PPS 约 1 分钟）
```

**约定**：变更黑白名单一律走 Web（保留持久化与审计）；console 仅用于应急排查，
且用完后应 `systemctl restart chrony` 清掉临时规则。

### 2.2 同网段 allow 与 deny 可以共存

chrony 的访问限制按**最长前缀**匹配，**等前缀长度下 deny 优先**。因此可以把某个网段从放行
翻成拒绝而**不必先删 allow**：

```
allow 192.168.1.0/24     ← 保留在文件里
deny  192.168.1.0/24     ← 新增；即时报文，chronyd 不重启、计数器不清零
```

这条路径 2026-09-17 起在 Web 上可用（`ntpmon_acl_find` 由"只比 CIDR"改为"比
(action, CIDR)"）。在此之前，给已有 allow 的网段加 deny 会被判为"规则已存在"，
只能先删后加——而删除会重启 chronyd，代价是计数器归零 + 约 1 分钟重锁 PPS。

配套改动：

- 表格里同网段会出现两行（allow 绿 / deny 红），**每行的删除按钮携带自己的动作**，
  删除不会误伤另一行；
- 助手脚本 `chrony_acl_apply.sh remove` 新增可选动作参数
  （`remove allow|deny <CIDR>`；不带动作时退回旧的"allow 优先"行为），
  否则文件里两行同网段时无法确定删哪条；
- 删除仍然会重启 chronyd（chrony 3.4 无运行时删规则命令），这一点没有改变。

## 3. 部署要点（详见 README 步骤 3c）

1. `include /etc/chrony/acl-web.conf` 加入 chrony.conf（一次），现有生效 `allow/deny` 行
   迁入托管文件（行为不变），重启 chrony 激活。
2. `chrony_acl_apply.sh` + `ntp_stats_sample.sh` 安装到 `/usr/local/bin/`；
   `ntp-stats.service/.timer` 启用（每分钟采样；iptables 规则由采样脚本幂等自愈）。
3. sudoers：`/etc/sudoers.d/99-www-ntpacl` 放行 www-data 调用助手的三种定形命令。

## 4. 前端交互

- 表格：全用户可看；新增表单与删除按钮**仅 root** 显示（`gate_require_role` 强制）。
- 删除确认框明示"将重启 chrony（约 1 分钟重新锁定 PPS，计数归零）"。
- 图表引擎：**uPlot v1.6.32**（vendored：`www/uPlot.iife.min.js` + `uPlot.min.css`，MIT；
  sha256 记录：js `19c8d4c6ad88929a79f4ae49d6f7161566dfd0ba3d15cc495e974f787eb78f1f`、
  css `df630c6a8d6f8eeaff264b50f73ce5b114f646ffd9a0bb74f049b0a00135fa04`）。
  本地静态文件，板端离线可用（不依赖 CDN）。
- 图 1「请求量趋势」：单主轴=每分钟请求数（面积渐变）；悬停十字线 + tooltip 显示
  时间 / 每分钟 / 累计；顶部**数字卡**＝当前速率 · 近 1 小时请求 · 峰值 · 累计
  （累计语义：自 chronyd 启动，重启清零，卡片标题已注明）。
- 图 2「各网口请求量」：uPlot bars（eth0-3 分类轴，累计值）；tooltip 显示累计 + 近 1h 增量。
- 60 秒轮询用 `setData()` 就地更新（光标不跳）；切到该 Tab / 窗口缩放触发 `setSize` 重排。
- 计数器归零断点：`m = -1` 转为 `null`（uPlot 自动断线）；累计归零自然回落到 0。

## 5. 测试与离线桩

- `test_gate.sh` Test 28-28k：假 helper（`NTPMON_HELPER_OVERRIDE`）记录参数、
  `NTPMON_ACL_FILE`/`NTPMON_CSV` 指临时文件，覆盖解析、降级、权限、校验、失败透传。
- `test_frontend.py`：断言 `ntpmon.cgi` 的 `stats`/`acl_op` 两个 action 在前后端同时存在；
  并断言两个 uPlot 静态文件存在且被页面引用（防部署漏拷）。
