# CONTEXT.md — embeded-web

本项目的领域词汇表与核心概念。分析、议题、重构时必须使用这里的术语,不要漂移到同义词。

## 词汇表

| 术语 | 定义 |
|---|---|
| 主控 (LubanCat) | 运行 Lighttpd + C CGI 与本地执行器的板子;合并方案后即 "Board 1"(本机,192.168.1.100)。 |
| Remote Board | 通过串口连接、运行 handler.sh 的远端板卡(Board 2/3/4),接受主控下发的网络配置命令。 |
| handler.sh | 远端板上的串口协议执行器,以 root 运行,实现 PING / REMOTE_GET/SET_IPV4 / REMOTE_GET/SET_IPV6。 |
| handler-local.sh | 主控本地的协议执行器(ADR-0001),监听 FIFO 而非串口,用 nmcli 持久化地修改本机网络。 |
| 本地模式 | network.cgi 不带 `port=` 参数时的路径:对本机(LubanCat)直接读 `ip`、经 FIFO 下发 SET,不走串口。 |
| 看门狗回滚 | SET 本机 IP 时启动倒计时;用户以新 IP 重登并通过 `REMOTE_CONFIRM_IPV4` 确认则取消;超时未确认则回滚旧配置。 |
| FIFO 通道 | `/run/serial_protocol.fifo`;CGI(www-data)写、handler-local.sh(root)读,实现权限隔离。 |
| Board 配置 | 串口协议目标:Board 2 = ttyS4 @ 115200 / 192.168.8.199;Board 3/4 预留,处理方式相同。 |
| 密码策略 | 高强度密码规则(ADR-0002):10-64 字符,四类各至少 1 个(大写/小写/数字/ASCII 标点),拒绝中文、空格、控制字符;新密码不得与当前密码相同。创建、root 重置、用户自改均强制检查。 |
| 密码有效期 | 每次修改(创建/重置/自改)后 90 天,起点记于 `users.password_changed_at`;到期前 7 天登录时提示剩余天数。 |
| 强制改密 | 密码到期或存量用户首次登录时的状态:登录成功但除改密外所有 CGI 被后端拦截,改密成功后才恢复全部功能。 |
| 存量用户 | 密码策略上线前已存在的账号,其 `password_changed_at` 置 0,视为已过期,首次登录触发强制改密。 |
| 踢会话 | 用户改密成功后,删除该用户除当前外的全部活跃会话(防旧会话带旧凭据继续操作)。 |

## 核心决策

- 见 [docs/adr/0001-merge-lubancat-and-board-1.md](docs/adr/0001-merge-lubancat-and-board-1.md)。
- 见 [docs/adr/0002-password-policy.md](docs/adr/0002-password-policy.md)。
