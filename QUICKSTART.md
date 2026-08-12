# 快速启动 — LubanCat Web 管理系统

板子上电后 **lighttpd 和 handler-local.sh 均通过 systemd 开机自启**，通常无需手动操作。
本页用于启动后快速验证、服务异常时快速恢复。

> Board 1（本机，IP 192.168.1.100）= 主控 LubanCat：Web 面板 + 本机网络配置。
> Board 2（IP 192.168.8.199）= 远端板，经 `/dev/ttyS4`（115200）串口管理，见第 7 节。

## 1. 关键信息

| 项目 | 值 |
|------|-----|
| 板子 IP | 192.168.1.100 |
| Web 地址 | `https://192.168.1.100`（自签名证书，浏览器点"继续访问"） |
| SSH | `ssh root@192.168.1.100`（root / root） |
| Web 登录 | root / **Abcd1234!xyz**（ADR-0002 上线时改的强密码；存量/新建账号首次登录均需先改密，见 §4 密码策略） |
| Board 2 | 192.168.8.199，`/dev/ttyS4` @ 115200（经串口访问，SSH 不直连） |

## 2. 启动后快速验证

```bash
# ① 服务状态（两项都应显示 active / enabled）
systemctl status lighttpd.service serial-handler.service

# ② Web 是否响应
curl -skI https://127.0.0.1/ | head -3          # 期望 HTTP/1.1 200

# ③ 本机 FIFO 通道（handler-local.sh 是否在工作）
printf "PING\n" > /run/serial_protocol.fifo
timeout 2 head -1 /run/serial_protocol.resp      # 期望 OK

# ④ 完整功能测试（在 WSL2 宿主机执行）
./test_suite.sh 192.168.1.100 Abcd1234!xyz       # 期望 47/48 通过（1 项环境限制，见 README）
```

## 3. 服务异常时

```bash
# 手动启动 / 重启
systemctl start  serial-handler.service    # FIFO + nmcli 处理器
systemctl restart lighttpd.service         # Web 服务器

# 查看日志
journalctl -u serial-handler.service -n 50        # handler 日志
cat /var/log/serial_protocol.log                  # 协议收发日志（每次命令都有记录）
tail -20 /var/log/lighttpd/error.log              # lighttpd 错误日志
```

## 4. 常用操作速查

| 操作 | 命令 |
|------|------|
| 修改自己密码 | 控制面板右上「修改密码」→ /change.html（需验证当前密码，成功后踢其他会话） |
| 密码到期处理 | 到期/存量账号登录后自动跳转 /change.html 强制改密；改密前所有接口返回"密码已过期,请先修改密码" |
| 查看当前 IP 配置 | `ip -4 addr show eth0` |
| 查询本机配置（走协议） | `printf "REMOTE_GET_IPV4\n" > /run/serial_protocol.fifo && timeout 2 head -1 /run/serial_protocol.resp` |
| 看门狗回滚档案 | `/var/run/serial_protocol.rollback`（存在 = 有待确认的 IP 修改） |
| 数据库 | `/var/db/myapp.db`（www-data 所有，勿用 root 写） |
| 证书 | `/etc/lighttpd/server.pem`（过期重新生成：`openssl req -x509 -newkey rsa:2048 -keyout /etc/lighttpd/server.key -out /etc/lighttpd/server.crt -days 3650 -nodes -subj "/CN=lubancat.local" && cat /etc/lighttpd/server.crt /etc/lighttpd/server.key > /etc/lighttpd/server.pem && chmod 600 /etc/lighttpd/server.pem && systemctl restart lighttpd`） |

## 5. 首次部署 / 重新部署

完整步骤见 [README.md](README.md)（依赖、交叉编译、安装、systemd 注册）。要点：

- lighttpd 依赖：`libxxhash0` `libdeflate0` `libgamin0` `gamin`（**不要用 libfam0**，缺 FAMNoExists 符号）
- CGI 为 WSL2 交叉编译产物（板载 gcc 损坏），动态链接，依赖板子自带 glibc 2.31 / libsqlite3.so.0
- handler-local.sh 看门狗默认 180s：改本机 IP 后未确认会自动回滚

## 7. Board 2 快速启动（远端板，192.168.8.199）

Board 2 经 LubanCat 的 `/dev/ttyS4`（115200）串口管理。接线：**LubanCat ttyS4 ↔ Board 2 串口**（交叉接法，GND 必须共地）。

### 7.1 首次部署（一次性，在 Board 2 上执行）

```bash
# 从主控/WSL2 复制 handler.sh 并配置串口
scp handler.sh root@<board2_ip>:/usr/local/bin/
ssh root@<board2_ip> '
  sed -i "s|DEV=/dev/ttyS7|DEV=/dev/ttyS4|" /usr/local/bin/handler.sh
  sed -i "s|BAUD=115200|BAUD=115200|" /usr/local/bin/handler.sh   # 已是 115200 则无需改
  chmod +x /usr/local/bin/handler.sh
'
```

> Board 2 的初始 IP（192.168.8.199）在部署时需先用其自有方式（串口终端/网口直连）配好，
> 之后便可通过 Web 面板远程改 IP。

### 7.2 启动 / 开机自启（Board 2 上）

```bash
# 方式 A：systemd 自启（推荐）
ssh root@<board2_ip> '
  cat > /etc/systemd/system/serial-handler.service << "EOF"
[Unit]
Description=Remote Board serial protocol handler
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/handler.sh
ExecStartPre=/bin/rm -f /var/run/serial_protocol.lock /var/run/serial_protocol.pid
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF
  systemctl daemon-reload
  systemctl enable --now serial-handler.service
'

# 方式 B：临时调试
ssh root@<board2_ip> 'nohup /usr/local/bin/handler.sh > /tmp/handler.log 2>&1 &'
```

### 7.3 验证

```bash
# ① 主控上确认串口 handler 就绪
systemctl status serial-handler.service          # LubanCat 本机（已自启）

# ② 命令行直查 Board 2（主控上执行）
#    Web API 带 port=s4 即走串口 → Board 2
curl -sk -b "session_id=$SID; csrf_token=$CSRF" \
     "https://127.0.0.1/cgi-bin/network.cgi?action=get&port=s4"   # 期望返回 192.168.8.199 配置

# ③ 浏览器：控制面板切到 "Board 2" Tab，点查询/保存
# ④ 完整测试（含 Board 2 的 Test 12/12c）：./test_suite.sh 192.168.1.100
```

Board 2 修改 IP 后无看门狗回滚（`REMOTE_CONFIRM_IPV4` 仅本机模式有效，ADR-0001）：
改错 IP 导致串口失联时，需直接接串口终端恢复。

## 6. 常见问题

| 现象 | 处理 |
|------|------|
| 登录后改 IP 断线 | 正常现象。用新 IP 重新登录，控制面板会自动发 `confirm` 取消看门狗；180s 内未登录会自动回滚 |
| network.cgi 报"本地通道不可用" | handler-local.sh 未运行：`systemctl start serial-handler.service` |
| network.cgi 报"Remote Board not responding"（port=s4） | 检查串口接线（ttyS4 ↔ Board 2，GND 共地）、Board 2 的 handler.sh 是否运行、波特率是否 115200 |
| Board 2 改 IP 后失联 | 无看门狗回滚。直接接串口终端进 Board 2 恢复（见 7.3 末注） |
| 页面打不开 | `ping 192.168.1.100` 通但 HTTPS 不通 → `systemctl restart lighttpd`；ping 不通 → 检查网线/网段 |
| 证书过期/浏览器拦截 | 过期才需重新生成（见上表）；未过期直接"继续访问" |
