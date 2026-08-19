# 词汇表（Glossary）

术语定义，供 ADR 与代码注释引用。按 ADR 出现顺序累积。

## 认证与安全（ADR-0002）

- **密码策略**：10-64 字符，大写/小写/数字/ASCII 标点四类各至少 1 个；中文、空格、控制字符拒绝。后端 C 强制，前端 JS 仅 UX 提示。
- **密码有效期**：每次修改后 90 天；到期前 7 天登录提示剩余天数。
- **强制改密**：密码到期或首次登录时，登录成功但进入改密态——除 `user_change_pass.cgi` 外所有受保护 CGI 被 gate 拦截（页面 302 到 change.html，JSON 输出错误）。
- **存量用户**：策略上线时 `password_changed_at` 置 0 视为已过期的既有账号（首登强制改密）。
- **踢会话**：改密成功后删除该用户其他活跃会话，仅保留当前会话（防会话劫持者借改密接管账号）。

## 网络配置（ADR-0003）

- **网口**：本机以太网接口 eth0/eth1/eth2/eth3。eth0 为管理口（当前 SSH/网页入口），eth1-3 常处 NO-CARRIER（未插线）状态。
- **NetworkManager profile**：NM 的连接配置单元，持久化于 `/etc/NetworkManager/system-connections/`，经 `nmcli connection` 读写。本系统网络配置的唯一数据源（无文件层）。
- **实际生效配置**：`nmcli connection show` 查询到的当前值；页面显示的数据源（非保存的期望值）。
- **唯一网关**：同一时刻最多一个网口配置默认网关（单机默认路由只有一条的显式约束）。
- **网关自动迁移**：新网关落在其他口时自动清除原口网关。
- **回滚看门狗**：eth0 配置变更后由板端自主执行的超时还原机制——180s（3 分钟）内无登录信号则还原旧配置（持久化于 `/var/db/rollback.json`，开机恢复服务兜底掉电场景）。
- **登录信号**：回滚的确认条件 = 网页登录（login.cgi 成功写 confirmed 标记）或 SSH 登录成功（auth.log 中新 IP 的 Accepted 记录）。任一出现即保留新配置。
- **NMCLI_OVERRIDE**：宿主机测试环境变量，覆盖 network.cgi 内部调用的 nmcli 命令（假命令脚本），使网络逻辑可离线单测（沿用 REMOTE_SERIAL_DEVICE_OVERRIDE 模式）。

## 架构（全篇）

- **BOARDS 注册表（废弃）**：旧前端 Board 1-4 tab 驱动表；ADR-0003 后改为 4 网口注册表。
- **串口层（废弃）**：remote.c 板表 + PING/REMOTE_GET/SET 文本协议 + handler.sh；ADR-0003 彻底移除。
- **门卫阶梯（gate）**：session → role → CSRF 逐级校验，统一错误输出；强制改密检查位于会话校验之后。
