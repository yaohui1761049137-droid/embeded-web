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
