# ADR-0001: 合并 LubanCat 与 Board 1(本地网络配置)

- 日期:2026-08-10
- 状态:已接受(原型阶段)
- 关联:[[context 词汇表]]中的 本地模式、handler-local.sh、看门狗回滚

## 背景

原架构中,LubanCat(主控,运行 Lighttpd + C CGI)通过串口 `/dev/ttyS7` @ 115200 将网络配置命令发给远端 Board 1(192.168.8.201,运行 handler.sh),由 handler.sh 在远端用 `ip addr` 临时生效地修改 IP(重启丢失)。

本方案将主控自身作为 "Board 1":其 IP 配置不再走串口,而是由本地 root 守护进程直接调用 nmcli 完成,配置持久化,并带断连回滚保护。

## 决策

1. **本地模式**:`network.cgi` 不带 `port=` 参数时即本地模式——不打开串口,GET 直接用 `ip` 命令读取本机地址(无需提权);SET 把协议命令写入 FIFO(`/run/serial_protocol.fifo`),由本地 root 守护进程执行。
2. **handler-local.sh**:复用 handler.sh 的 flock/PID/重启框架,监听 FIFO 而非串口,收到 `REMOTE_GET/SET_IPV4/IPV6` 命令后用 **nmcli connection profile**(`nmcli con mod` + `nmcli con up`)执行,实现持久化。连接名通过 `nmcli -t -f GENERAL.CONNECTION dev show eth0` 动态解析,不写死。
3. **看门狗回滚**:SET 执行前将旧配置存档到 `/var/run/` 并启动倒计时(默认 180s,可配置);用户以新 IP 重新登录后,前端通过新协议命令 `REMOTE_CONFIRM_IPV4` 取消倒计时;超时未确认则自动回滚旧配置。
4. **远端 Board 2/3/4**:继续使用原串口协议与 handler.sh(Board 2 = ttyS4 @ 115200 / 192.168.8.199;Board 3/4 预留,处理方式相同,设备节点待硬件到位后补充)。
5. **权限模型**:nmcli 仅由 root 守护进程执行;CGI(www-data)只写 FIFO;本机读取不提权。不使用 sudoers 白名单或 setuid。
6. **前端**:Board 1 Tab 改为本机配置(不再标注 ttyS7),处理"改 IP 断连 → 引导用新 IP 重登 → 确认生效"的交互。
7. **审计**:本地模式的配置修改同样写入 `audit_log`(复用 auth_audit_log)。

## 理由

- 省掉一块物理板与一条串口链路;nmcli profile 使配置重启后仍生效,优于原 handler.sh 的临时 `ip addr`。
- 复用既有文本行协议与 handler.sh 基础设施,CGI 与前端改动面最小。
- root 操作不进 CGI,保持最小权限与命令注入面。
- 看门狗回滚避免"填错 IP 后主控永久失联"。

## 后果

- 新增:handler-local.sh;`REMOTE_CONFIRM_IPV4` 协议命令;FIFO 传输(common.c);network.cgi 本地模式分支 + confirm action。
- 修改:common.h(本地 FIFO 路径宏)、control_panel.html(Board 1 Tab)、test_suite.sh(本地模式用例)。
- 风险:改 IP 瞬间 HTTP 连接断开(已由确认流程处理);主控(192.168.1.x)与板卡(192.168.8.x)不同网段,跨网段互访需另行规划。

## 备选方案(未采纳)

- **A. sudoers 白名单**放行 www-data 执行 nmcli:配置最少,但 root 操作进入 CGI 进程,参数注入面更大。
- **B. setuid helper**:多一份二进制与 setuid 滥用风险。
