# ADR-0003: 本地网口网络配置（移除 Remote Board 串口层）

- 日期:2026-08-18
- 状态:已接受
- 词汇:网口、NetworkManager profile、唯一网关、回滚看门狗、确认凭据（见 CONTEXT.md 词汇表）

## 背景

新系统（LubanCat 2N）没有 Board 1/2/3/4，取而代之的是 4 个本地网口 eth0/1/2/3。旧「网络参数配置」tab 通过串口（`/dev/ttyS7`/`/dev/ttyS4`）读写远端板的 IPv4/IPv6 配置（remote.c 板表 + PING/REMOTE_GET/SET 文本协议）。

需求:功能与旧系统一致——可设置每个网口的 IPv4 和 IPv6，并在页面显示——但目标设备从「远端板」变为「本机网口」。本机已安装 NetworkManager（active+enabled，eth0 由 NM 的 "Wired connection 1" profile 管理）。

## 决策

1. **配置介质:不用文件层,直接 nmcli 管 NetworkManager**。CGI 经 nmcli 读写 NM connection profile（NM 自身持久化到 `/etc/NetworkManager/system-connections/`）。不新建配置文件——避免双数据源/双份真相/同步问题。
2. **显示内容:实际生效配置**。CGI 用 `nmcli connection show` 查询当前生效值（与旧系统 REMOTE_GET 返回远端板实际配置的行为一致），不显示未生效的期望值。
3. **串口层彻底移除**:`remote.c/h`、`action.cgi`、`handler.sh`、`test_remote.c`、`test_fake_board.py`、「服务器状态查看」tab 的串口调试卡片（sendCommand）。主备协同等占位 tab 不受影响（本为"开发中..."）。
4. **IPv4:仅静态**（IP/mask/网关），与旧协议一致，无 DHCP 模式。
5. **IPv6:仅手动**（addr/prefix），无 SLAAC/关闭切换。
6. **DNS:可选配置**（1-2 个服务器），静态 IP 下保证域名解析能力。
7. **全局唯一网关 + 自动迁移**:同一时刻最多一个网口配置网关;新网关落在其他口时,自动清除原口的网关（用户改到哪口，网关就移到哪口）。
8. **eth 网关优先于 4G**:配置 eth 网关时把 4G（quectel-CM 的 wwan0 默认路由）降级为后备（route metric 调大），避免用户配了网关却不生效的困惑。
9. **提权:www-data 经 sudoers NOPASSWD 白名单调用受限 nmcli 子命令**,参数严格校验（网口名白名单 eth0-3、IP 正则）。不采用 polkit（复杂度高、板端配置未验证）。
10. **eth0（SSH 口）变更走自动回滚（3 分钟未登录即回滚）**:
    - 触发:仅 eth0 的配置变更（eth1-3 改完立即生效,用户自担风险）。
    - 流程:备份旧配置 → nmcli 应用 → 写 `/var/db/rollback.json`（pending 状态,持久化）→ 看门狗 180s → 期间任一登录信号出现则清 pending（保留新配置）;超时无登录 → 自动还原旧配置。
    - 登录信号 = 网页登录（login.cgi 成功,写 confirmed 标记）**或** SSH 登录成功（看门狗检查 auth.log 中来自新 IP 的 Accepted 记录）——覆盖 SSH-only 与网页两种验证习惯。
    - 看门狗:systemd-run 一次性 unit（`--on-active=180`）;开机恢复由 oneshot 服务检查 pending 状态（掉电重启也能救回）。
11. **审计保留**:网络配置修改写入 `audit_log`（沿用旧系统行为）。
12. **前端**:`BOARDS` 注册表改为 4 网口注册表（eth0-3）;`test_frontend.py` 改为断言前端网口注册表 ↔ 后端网口表一致（沿用双表一致性模式）。
13. **测试**:宿主环境变量 `NMCLI_OVERRIDE` 覆盖 nmcli 命令（假命令脚本,离线可跑,沿用 REMOTE_SERIAL_DEVICE_OVERRIDE 模式）;板端 `test_suite.sh` 改为真实 nmcli 验证（改 eth1 → 查实际配置 → 还原）。

## 理由

- 单一数据源是核心原则:NM profile 已是持久化真相,文件层必然产生同步问题（配置失败但文件已更新、重启回退后文件过期）;本系统之前为 SQLite WAL 多进程并发吃过亏,不为错误边界再引入一类。
- 显示实际配置保证「所见即所得」,与旧系统语义一致;用户无需理解期望/实际两个概念。
- 移除串口层是顺水推舟:remote.c 仅被 network.cgi 引用,action.cgi 仅被串口调试卡片使用,主备协同等 tab 是占位——移除面干净,不留死代码。
- 仅 eth0 回滚:其他口改动不影响管理面,统一回滚反而对 eth1-3 用户造成不必要的「等 60 秒」体验;eth0 断连是唯一的锁死风险,回滚只保这一点。
- 回滚必须板端自主执行:改 eth0 后网页与 SSH 同时断,任何依赖客户端的确认都不成立;持久化 + 开机恢复覆盖掉电场景。
- 「未登录即回滚」优于 token 确认:改完 IP 后重新登录本就是用户自然动作,登录成功 =「新配置可达且凭据有效」的最强信号,无需额外 token 机制（少一个接口、一类凭据管理）;网页+SSH 双信号避免 SSH-only 用户的误回滚。
- sudoers 白名单 + 参数校验:www-data 只能执行白名单 nmcli 子命令,即使参数注入也被网口名/IP 正则挡在门外;比 polkit 更易审计、更少板端依赖。
- 全局唯一网关是单机多口的必然约束:一台机器默认路由只有一条,4 口各自网关必然竞争;显式「唯一 + 迁移」把竞争变成用户可理解的设计。eth 优先于 4G 同理——配置了网关就该生效。

## 后果

- **新增文件**:`/usr/local/bin/rollback_watchdog.sh`（或 systemd-run 内联,含 auth.log 的 SSH Accepted 检查）、开机恢复 oneshot unit（`rollback-recover.service`）。
- **挂钩**:`login.cgi` 登录成功时若存在 pending 回滚项则写 confirmed 标记;`network.cgi` 负责备份/应用/写 pending/触发看门狗。
- **删除文件**:`src/remote.c/h`、`src/action.cgi.c`、`handler.sh`、`test_remote.c`、`test_fake_board.py`;前端删除 sendCommand/串口调试卡片。
- **改造**:`src/network.cgi.c`（nmcli 读写 + 回滚编排 + 审计）、`src/common.c` 可能删串口相关函数、`www/control_panel.html`（网口注册表 + 表单字段:IP/mask/网关/DNS/IPv6）、`test_gate.sh`（network.cgi 冒烟改为 NMCLI_OVERRIDE 假命令）、`test_suite.sh`（板端真测）、`test_frontend.py`（ETH 一致性）、`docs/adr/0001`（如有串口相关 ADR 需标记废弃）。
- **部署变更**:sudoers 白名单条目、开机恢复服务安装、4G auto_4G 脚本路由 metric 调整（第 8 条）。
- **测试影响**:旧 46 项板端套件全部重写为网口语义;宿主机套件以假 nmcli 覆盖离线运行,不依赖宿主机 NM。
