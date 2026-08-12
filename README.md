# embeded-web — Lighttpd + CGI Multi-User Web System

嵌入式 Web 管理系统，基于 **Lighttpd 1.4.55 + C CGI**，运行在 LubanCat ARM aarch64 开发板上（实测 Ubuntu 20.04 focal）。

通过串口连接多个 Remote Board，实现网络参数的远程查询和配置。支持双板独立管理。

## 功能

- **HTTPS 加密传输**（TLS 1.2+，HSTS，自签名证书）
- **多用户认证**（root / admin 角色，SHA-256 密码哈希，10 万轮迭代）
- **Session + CSRF 双重防护**（HttpOnly Cookie + CSRF Token）
- **串口通信**（termios2 / BOTHER 自定义波特率，PING 预热 + 重试）
- **多 Board 支持**（Web 控制面板一键切换，独立配置互不干扰；ADR-0001 起 Board 1 即本机 LubanCat，经 FIFO + nmcli 本地配置）
- **用户管理 API**（root 可创建/启用/禁用/删除 admin 用户，含审计日志）
- **业务审计日志**（网络配置修改自动记录操作者和时间到 `audit_log` 表）
- **SQLite 存储**（WAL 模式，多进程 CGI 并发安全）
- **36 项自动化测试**（`test_suite.sh`，含 Board 1 本地模式、Board 2 和审计日志验证；真机实测 33 通过，3 项因测试环境限制未通过，详见文末）

## 架构

```
 Browser (HTTPS)
     │
     ▼
┌─────────────────────┐
│  Lighttpd 1.4.55    │  ← 静态文件 + CGI (fork/exec)
│  (www-data)         │
├─────────────────────┤
│  mod_openssl        │  ← TLS 终止
│  mod_cgi            │  ← CGI 调度
│  mod_redirect       │  ← HTTP → HTTPS
└──────┬──────────────┘
       │
       ▼
┌──────────────────────────┐      FIFO /run/serial_protocol.fifo  ┌──────────────────┐
│  CGI Programs (C)         │  ──────────────────────────────→ │  handler-local.sh │
│  + auth.c (SQLite/审计)    │  ←────────────────────────────── │  (root, nmcli)    │
│  + common.c (串口/FIFO)    │                                  │  = 本机 (Board 1)  │
│                           │                                  │  192.168.1.100    │
│  network.cgi (无 port) ───│                                  └──────────────────┘
│                           │      Serial /dev/ttyS4 (115200)  ┌──────────────────┐
│  network.cgi ?port=s4 ────│  ──────────────────────────────→ │  Board 2          │
│                           │  ←────────────────────────────── │  handler.sh       │
│  audit_log (业务操作)      │                                  │  192.168.8.199   │
└──────────────────────────┘                                   └──────────────────┘
       │
       ▼
┌──────────────┐
│   SQLite DB   │  ← users / sessions / audit_log
│  /var/db/     │
└──────────────┘
       ▼
┌──────────────┐
│   SQLite DB   │  ← users / sessions / audit_log
│  /var/db/     │
└──────────────┘
```

## 目录结构

```
embeded_Lighttpd/
├── config/
│   ├── lighttpd.conf         主配置（模块、文档根、MIME）
│   ├── 10-cgi.conf           CGI 路径映射
│   └── 10-ssl.conf           HTTPS + HTTP→HTTPS 跳转
├── src/
│   ├── sqlite3.h / sqlite3.c  SQLite3 amalgamation（单文件嵌入式数据库）
│   ├── sha256.h / sha256.c    SHA-256 实现（密码哈希）
│   ├── auth.h / auth.c        认证库（Session/User/CSRF/Audit）
│   ├── common.h / common.c    CGI 公共库（HTTP/串口/POST 解析）
│   ├── login.cgi.c            登录（DB 验证 + 双 Cookie）
│   ├── logout.cgi.c           登出（销毁 Session）
│   ├── main.cgi.c             控制面板入口（Session 校验）
│   ├── network.cgi.c          网络配置（串口通信 + CSRF）
│   ├── action.cgi.c           串口调试工具
│   ├── db_init.c              数据库初始化（创建 root 用户）
│   ├── user_list.cgi.c        [root] 用户列表
│   ├── user_create.cgi.c      [root] 创建用户
│   ├── user_passwd.cgi.c      [root] 重置密码
│   ├── user_toggle.cgi.c      [root] 启用/禁用
│   └── user_delete.cgi.c      [root] 删除用户
├── www/
│   ├── index.html             登录页面
│   ├── control_panel.html     控制面板（7 Tab，含 Board 1/2 切换）
│   └── style.css              全局样式
├── handler.sh                 Remote Board 串口协议处理脚本
├── handler-local.sh           本机协议执行器（FIFO + nmcli，带看门狗回滚，ADR-0001）
└── test_suite.sh              36 项端到端测试
```

## 快速开始

### 依赖

- **目标板**：ARM aarch64，Ubuntu 20.04 focal（README 原标称 Debian Buster 已过时）
- **运行时**：Lighttpd 1.4.55 (mod_openssl 内置，无需 lighttpd-mod-openssl 包)，OpenSSL 1.1.1+，NetworkManager（nmcli 已接管 eth0）
- **编译**：**板载 gcc 9.4 实测损坏**（编译器/汇编器随机段错误），需在 WSL2 宿主机用 `aarch64-linux-gnu-gcc 11.2` 交叉编译（见下文）。SQLite 和 SHA-256 均内嵌源码，除 glibc/pthread/dl 外无外部库依赖

### 安装 Lighttpd

目标板无外网，需在能访问 USTC 镜像的宿主机（如 WSL2）下载 focal arm64 包后上传安装：

```bash
# 下载（pool/main/l/l-lighttpd / pool/main/l/libxxhash0 / ... 实际路径见镜像索引）
libxxhash0_0.7.3-1_arm64.deb
libdeflate0_1.5-3_arm64.deb
libgamin0_0.1.10-6_arm64.deb   # 注意：不要用 libfam0！
gamin_0.1.10-6_arm64.deb
lighttpd_1.4.55-1ubuntu1_arm64.deb

scp *.deb root@<board>:/tmp/
ssh root@<board> 'cd /tmp && dpkg -i *.deb'
```

**坑**：focal 的 `libfam0` 缺少 lighttpd 1.4.55 需要的 `FAMNoExists` 符号（启动即报 undefined symbol），
必须安装 `libgamin0 + gamin`（gamin 的 libfam 兼容库含该符号；libgamin0 与 libfam0 互相 Conflicts，需先卸 libfam0）。

### 部署 Phase 1（HTTPS，不动 CGI）

```bash
# 1. 部署配置
scp config/*.conf root@<board>:/etc/lighttpd/
scp config/10-*.conf root@<board>:/etc/lighttpd/conf-available/
ssh root@<board> '
  ln -sf /etc/lighttpd/conf-available/10-cgi.conf /etc/lighttpd/conf-enabled/
  ln -sf /etc/lighttpd/conf-available/10-ssl.conf /etc/lighttpd/conf-enabled/
  usermod -a -G dialout www-data
'

# 2. 生成 TLS 证书
ssh root@<board> '
  openssl req -x509 -newkey rsa:2048 -keyout /etc/ssl/private/server.key \
    -out /etc/ssl/certs/server.crt -days 3650 -nodes -subj "/CN=lubancat.local"
  cat /etc/ssl/certs/server.crt /etc/ssl/private/server.key > /etc/lighttpd/server.pem
  chmod 600 /etc/lighttpd/server.pem
'

# 3. 启动 Lighttpd（dpkg 已创建 lighttpd.service，开机自启）
ssh root@<board> '
  systemctl enable lighttpd.service
  systemctl start lighttpd.service
'
```

注意：focal 的 lighttpd 1.4.55 中 `ssl.cipher-list` 不支持多行数组语法，需写成单行字符串
（见 `config/10-ssl.conf` 注释，部署时若报 parser error 请对照仓库内的单行写法）。

### 部署 Phase 2（多用户系统）— 交叉编译

板载 gcc 损坏，改用 WSL2 交叉编译。链接时必须使用 focal 版系统库（板子 glibc 2.31，
WSL2 的 22.04 交叉工具链默认 glibc 2.34 产物无法运行），做法：下载 focal arm64 的
`libc6`、`libpthread`(含于 libc6 包)、`libdl`(含于 libc6 包)、`libsqlite3-0` deb 解压成 sysroot，
再用 `-nostdlib` 手动指定 crt 文件和库顺序。

```bash
# 1. 准备 focal sysroot（宿主机，一次性）
mkdir -p /tmp/focal-sysroot/lib/aarch64-linux-gnu /tmp/focal-sysroot/usr/lib/aarch64-linux-gnu
#   从 mirrors.ustc.edu.cn/ubuntu-ports 下载解压：
#   libc6_2.31-0ubuntu9.18_arm64.deb  →  libc.so.6 libpthread.so.0 libdl.so.2 libm.so.6 ld-linux-aarch64.so.1
#   libsqlite3-0_3.31.1-4ubuntu0.7_arm64.deb → libsqlite3.so.0
#   并创建 -l 所需的未版本化符号链接（libc.so → libc.so.6 等）

# 2. 交叉编译（宿主机）
#    注意:链接行末尾需追加 focal ld-linux 路径(ld 需要其提供的
#    GLIBC_PRIVATE 符号,如 _dl_make_stack_executable),否则报 undefined reference
SRC=src; SYSROOT=/tmp/focal-sysroot; CRT=/usr/aarch64-linux-gnu/lib
LOADER=$SYSROOT/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1
for src in login.cgi.c logout.cgi.c main.cgi.c network.cgi.c action.cgi.c \
           user_list.cgi.c user_create.cgi.c user_passwd.cgi.c \
           user_toggle.cgi.c user_delete.cgi.c user_change_pass.cgi.c; do
  name=$(echo $src | sed "s/\.cgi\.c//").cgi
  aarch64-linux-gnu-gcc -Wall -O2 -nostdlib -o $name $CRT/crt1.o $CRT/crti.o \
    $SRC/$src $SRC/common.c $SRC/auth.c $SRC/sha256.c \
    -L$SYSROOT/lib/aarch64-linux-gnu -L$SYSROOT/usr/lib/aarch64-linux-gnu \
    -lsqlite3 -lpthread -ldl -lm -lc -lgcc -lgcc_eh $LOADER $CRT/crtn.o \
    -Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1
done
  name=$(echo $src | sed "s/\.cgi\.c//").cgi
  aarch64-linux-gnu-gcc -Wall -O2 -nostdlib -o $name $CRT/crt1.o $CRT/crti.o \
    $SRC/$src $SRC/common.c $SRC/auth.c $SRC/sha256.c \
    -L$SYSROOT/lib/aarch64-linux-gnu -L$SYSROOT/usr/lib/aarch64-linux-gnu \
    -lsqlite3 -lpthread -ldl -lm -lc -lgcc -lgcc_eh $CRT/crtn.o \
    -Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1
done

# 3. 上传、初始化数据库、安装
scp *.cgi root@<board>:/tmp/
ssh root@<board> '
  # db_init 也交叉编译一份（同上命令，src=db_init.c）
  mkdir -p /var/db && cd /tmp && ./db_init.cgi admin
  chown -R www-data:www-data /var/db
  cp *.cgi /home/www/cgi-bin/
  chown www-data:www-data /home/www/cgi-bin/*.cgi
  chmod 755 /home/www/cgi-bin/*.cgi
  cp control_panel.html index.html style.css change.html /home/www/
  systemctl restart lighttpd
'

# 4. 密码策略迁移（ADR-0002，自动完成，无需手工 SQL）
#    auth_init() 启动时自动执行:
#    ALTER TABLE users ADD COLUMN password_changed_at INTEGER NOT NULL DEFAULT 0
#    (存量行自动填 0 → 视为已过期 → 首次登录强制改密)。
#    db_init 新建的 root 同样置 0,首次登录需先改密(满足策略的强密码)。
```

产物为动态链接（约 28KB/CGI），只依赖板子自带 glibc 2.31 / libsqlite3.so.0，仅需 GLIBC_2.17 符号。

> 备选：`-static` 全静态链接也能运行，但实测偶发 glibc NSS 堆损坏（"corrupted double-linked list"），
> 不推荐。

### 运行测试

```bash
./test_suite.sh <board_ip> [root_password]
```

在 WSL2 宿主机对真机运行。root 密码需满足密码策略（ADR-0002）；若 root 处于"强制改密"状态（存量库首次部署后），先手动改一次密再跑本套件。
真机实测 47/48 通过，唯一失败为测试环境限制：

| 测试 | 失败原因 |
|------|----------|
| 3. HSTS | WSL2 curl 走 HTTP 代理导致头部丢失；板内 curl 确认 HSTS 头正常 |

> 历史项：Test 12（Board 2 SET）2026-08-11 串口硬件连通（B1 ttyS4 ↔ B2 ttyS4，TX/RX 交叉 + GND 共地）后实测通过；12b 审计日志需板子装有 python3（现已具备）。

## 安全模型

| 层面 | 机制 |
|------|------|
| 传输 | TLS 1.2+，HTTP→HTTPS 强制跳转，HSTS |
| 认证 | SHA-256 密码哈希（10 万轮 + 随机盐），`$5$` modular crypt 格式 |
| 密码策略 | **ADR-0002**：10-64 字符，四类各 ≥1（大写/小写/数字/ASCII 标点），拒中文/空格；每次修改后 90 天有效期，到期登录后强制改密（除改密外全部 CGI 后端拦截）；存量用户首登强制改密；自改密需验证当前密码，成功后踢其他会话 |
| Session | 64 字符随机 hex token，HttpOnly Cookie，1 小时过期 |
| CSRF | 双 Cookie：`session_id`(HttpOnly) + `csrf_token`(JS可读)，POST 需回传 |
| 权限 | root/admin 角色分离，root 保护（不可自删/不可禁最后一个 root） |
| 审计 | `audit_log` 表记录所有管理操作（含 `change_password`） |
| SQL | 参数化查询（SQLite prepared statements） |

> 密码策略详情与设计决策见 [docs/adr/0002-password-policy.md](docs/adr/0002-password-policy.md)。到期判定依赖板载时钟（无 RTC/NTP 时重启后时间错乱可能误判到期，届时以强制改密流程恢复即可）。

## Cookie 设计

```
Set-Cookie: session_id=<64 hex>; Path=/; HttpOnly; Secure; SameSite=Lax; Max-Age=3600
Set-Cookie: csrf_token=<32 hex>; Path=/; Secure; SameSite=Lax; Max-Age=3600
```

- `session_id` — HttpOnly，XSS 无法窃取，服务端 SQLite 校验
- `csrf_token` — JS 可读，POST 时在 body 中回传，服务端比对

## 串口协议

与 Remote Board 通信的文本行协议（`\n` 结尾）：

| 命令 | 方向 | 响应 |
|------|------|------|
| `PING` | → | `OK` |
| `REMOTE_GET_IPV4` | → | `OK <ip> <mask> <gateway>` |
| `REMOTE_SET_IPV4 <ip> <mask> <gw>` | → | `OK` |
| `REMOTE_GET_IPV6` | → | `OK <addr/prefix>` |
| `REMOTE_SET_IPV6 <addr/prefix>` | → | `OK` |
| `REMOTE_CONFIRM_IPV4` | → | `OK`（仅本机模式：取消看门狗回滚，ADR-0001） |

## Board 配置

| Board | IP | 通道 | 波特率 | CGI 参数 |
|-------|-----|------|--------|----------|
| Board 1（本机 LubanCat，ADR-0001） | 192.168.1.100 | FIFO + nmcli（无串口） | — | 默认（无 port 参数） |
| Board 2 | 192.168.8.199 | `/dev/ttyS4` | 115200 | `port=s4` |
| Board 3/4 | 预留 | 待定 | 待定 | 待定 |

Web 控制面板中 Board 1/2 各自拥有独立表单，切换时互不覆盖。Board 3/4 预留位置。

本机（Board 1）修改 IP 后当前连接会断开：需用新 IP 重新登录，登录后控制面板会自动发送
`action=confirm` 取消 handler-local.sh 的看门狗回滚（默认 180 秒，超时未确认则自动回滚旧配置）。

## Remote Board 部署

将 `handler.sh` 部署到 Remote Board 并修改顶部的 `DEV`，**建议注册为 systemd 服务**（
nohup 手动启动无守护：SSH 会话退出、进程崩溃后不会自动拉起，曾导致"串口无响应"误判，
详见下文排障记录）：

```bash
# 1. 部署脚本（先按实际接线修改 DEV=/dev/ttyS4）
scp handler.sh root@<remote_ip>:/usr/local/bin/
ssh root@<remote_ip> '
  sed -i "s|DEV=/dev/ttyS7|DEV=/dev/ttyS4|" /usr/local/bin/handler.sh
  chmod +x /usr/local/bin/handler.sh
'

# 2. 注册 systemd 服务（与 B1 的 serial-handler.service 同构）
ssh root@<remote_ip> '
  cat > /etc/systemd/system/serial-handler.service << "EOF"
[Unit]
Description=LubanCat Board2 serial protocol handler (ttyS4)
After=network-online.target
Wants=network-online.target

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
  systemctl enable serial-handler.service
  systemctl start serial-handler.service
'
```

handler.sh 特性：PID 清理、串口断开自动重连、flock 防重复启动。
注意：**不要**在 systemd 管理的同时保留 nohup 手动实例——两个实例争抢串口会互相干扰。

## Board 2 串口排障记录（S4 一直"无响应"的根因）

2026-08-11 真机排查结论：**串口硬件通路本身是好的**，此前"Board 2 无响应 / 已获取配置但表单为空"
由 4 个因素叠加造成，按影响排序：

1. **CGI 无条件返回 `status:"ok"`（代码 bug，已修复）**：`handle_get()` 在
   `REMOTE_GET_IPV4` 超时/失败时不检查返回值，直接返回
   `{"status":"ok","ipv4":{"ip":"","mask":"","gateway":""}}`。前端看到 ok 显示"已获取配置"，
   但表单被空字符串填充。修复：`ip[0] == '\0'` 时返回 `status:"error"` 并提示
   "Board 2 无响应(未连接或 handler.sh 未运行?)"。

2. **读取时序误判（测试方法问题）**：`printf "PING\n" > /dev/ttyS4; timeout 2 head -1 /dev/ttyS4`
   是先发后读——发送瞬间读端尚未打开，B2 的回复到达后被丢弃（串口不缓冲）。
   正确做法是**先监听再发送**：
   ```bash
   stty -F /dev/ttyS4 115200 cs8 -cstopb -parenb -echo -icanon -opost min 0 time 10
   (timeout 5 cat /dev/ttyS4 > /tmp/rx.log &); sleep 0.5
   printf "PING\n" > /dev/ttyS4; sleep 2; cat /tmp/rx.log   # 期望 OK
   ```

3. **handler 进程无守护**：B2 的 handler.sh 用 nohup 手动启动，曾出现进程死亡
   （`ps` 查不到）导致 CGI 真无响应。现已注册 systemd 服务解决。

4. **接线确认**：TX/RX 必须交叉（B1 pin 35 TX → B2 pin 33 RX，B2 pin 35 TX → B1 pin 33 RX），
   GND 共地（pin 39）。LubanCat Zero 40pin 引脚（已验证）：
   - **UART4_M1 = ttyS4**：pin 33 (RX) / pin 35 (TX)
   - **UART3_M0 = ttyS3**：pin 3 (RX) / pin 5 (TX)（备选通道，同样实测双向通）

验证命令（B2 端 handler 运行中）：见上文"先监听再发送"示例，收到 `OK` 即链路正常。

## 本机模式部署（ADR-0001）

将 `handler-local.sh` 部署到 LubanCat（本机即 Board 1），需系统已安装 NetworkManager
并接管 eth0。建议注册为 systemd 服务以便开机自启：

```bash
scp handler-local.sh root@<board>:/usr/local/bin/
ssh root@<board> '
  chmod +x /usr/local/bin/handler-local.sh
  cat > /etc/systemd/system/serial-handler.service << "EOF"
[Unit]
Description=LubanCat local serial protocol handler (FIFO + nmcli)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/local/bin/handler-local.sh
ExecStartPre=/bin/rm -f /var/run/serial_protocol.lock /var/run/serial_protocol.pid /var/run/serial_protocol.rollback /var/run/serial_protocol.watchdog
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF
  systemctl daemon-reload
  systemctl enable serial-handler.service
  systemctl start serial-handler.service
'
```

临时调试可用 `nohup /usr/local/bin/handler-local.sh > /tmp/handler-local.log 2>&1 &`。

handler-local.sh 监听 `/run/serial_protocol.fifo`，配置修改通过 `nmcli con mod + con up`
持久化（重启后保留）；修改 IP 时存档旧配置并启动看门狗（默认 180s，`WATCHDOG_SECS` 可调），
未收到 `REMOTE_CONFIRM_IPV4` 确认则自动回滚。CGI（www-data）只写 FIFO，nmcli 仅由该 root
守护进程执行。

## License

MIT
