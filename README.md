# embeded-web — Lighttpd + CGI Multi-User Web System

嵌入式 Web 管理系统，基于 **Lighttpd 1.4.59 + C CGI**，运行在 LubanCat ARM aarch64 开发板上。

通过串口连接多个 Remote Board，实现网络参数的远程查询和配置。支持双板独立管理。

## 功能

- **HTTPS 加密传输**（TLS 1.2+，HSTS，自签名证书）
- **多用户认证**（root / admin 角色，SHA-256 密码哈希，10 万轮迭代）
- **密码策略**（10-64 字符四类组合，90 天有效期，到期强制改密，用户自助改密）
- **Session + CSRF 双重防护**（HttpOnly Cookie + CSRF Token）
- **串口通信**（termios2 / BOTHER 自定义波特率，PING 预热 + 重试）
- **多 Board 支持**（Web 控制面板一键切换，独立配置互不干扰）
- **用户管理 API**（root 可创建/启用/禁用/删除 admin 用户，含审计日志）
- **业务审计日志**（网络配置修改自动记录操作者和时间到 `audit_log` 表）
- **SQLite 存储**（WAL 模式，多进程 CGI 并发安全）
- **46 项自动化测试**（`test_suite.sh`，含 Board 2 和审计日志验证）

## 架构

```
 Browser (HTTPS)
     │
     ▼
┌─────────────────────┐
│  Lighttpd 1.4.59    │  ← 静态文件 + CGI (fork/exec)
│  (www-data)         │
├─────────────────────┤
│  mod_openssl        │  ← TLS 终止
│  mod_cgi            │  ← CGI 调度
│  mod_redirect       │  ← HTTP → HTTPS
└──────┬──────────────┘
       │
       ▼
┌──────────────────────────────┐      Serial /dev/ttyS7 (115200)  ┌──────────────────┐
│  CGI Programs (C)            │      ──────────────────────────→ │  Board 1          │
│  + gate.c (门卫阶梯+强制改密)│      ←────────────────────────── │  handler.sh       │
│  + auth.c (SQLite/审计/策略) │                                  │  192.168.8.201    │
│  + users.c (用户管理/自改密) │      Serial /dev/ttyS4 (38400)   └──────────────────┘
│  + remote.c (板表+协议)      │      ──────────────────────────→ ┌──────────────────┐
│  + common.c (HTTP/串口/POST) │      ←────────────────────────── │  Board 2          │
│                              │                                  │  handler.sh       │
│  network.cgi ?port=s4 ───────│                                  │  192.168.8.99     │
│                              │                                  └──────────────────┘
│  audit_log (业务操作)        │
└──────────────────────────────┘
       │
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
│   ├── gate.h / gate.c        CGI 请求门卫（session → role → CSRF 阶梯 + 强制改密，统一错误输出）
│   ├── users.h / users.c      用户管理模块（业务不变量 + SQL + 审计，位于 gate 之上）
│   ├── remote.h / remote.c    Remote Board 协议客户端（板表 + PING 预热 + 重试 + OK/ERR 文法）
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
│   ├── user_delete.cgi.c      [root] 删除用户
│   └── user_change_pass.cgi.c 用户自改密（当前密码验证 + 策略 + 踢会话）
├── www/
│   ├── index.html             登录页面
│   ├── control_panel.html     控制面板（7 Tab，含 Board 1/2 切换）
│   ├── change.html            修改密码页面（自改密入口）
│   └── style.css              全局样式
├── docs/adr/                  ADR 决策记录（0002 = 密码策略）
├── handler.sh                 Remote Board 串口协议处理脚本
├── test_suite.sh              46 项端到端测试
├── test_gate.sh               宿主机门卫单测（CGI 级，无需板子）
├── test_users.c               宿主机用户模块单测（API 级，无需板子）
├── test_remote.c              宿主机协议客户端单测（API 级，pty 假板驱动）
├── test_fake_board.py         脚本化假 Remote Board（pty 协议仿真，供离线测试）
└── test_frontend.py           前端板注册表 ↔ remote.c 板表一致性测试（离线）
```

## 快速开始

### 依赖

- **目标板**：ARM aarch64，Debian Buster，gcc 8.3+
- **运行时**：Lighttpd 1.4.59+ (with mod_openssl)，OpenSSL 1.1.1+，PCRE
- **编译**：仅需 gcc + make，零外部库依赖（SQLite 和 SHA-256 均内嵌）

### 安装 Lighttpd

```bash
# 下载 Debian Buster arm64 包并安装
dpkg -i libxxhash0_*.deb lighttpd_*.deb lighttpd-mod-openssl_*.deb
```

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

# 3. 停止 Boa，启动 Lighttpd
ssh root@<board> '
  kill $(pidof boa) 2>/dev/null
  lighttpd -f /etc/lighttpd/lighttpd.conf
'
```

### 部署 Phase 2（多用户系统）

```bash
# 1. 打包源码并上传
tar czf src.tar.gz src/*.c src/*.h
scp src.tar.gz www/* root@<board>:/tmp/

# 2. 编译
ssh root@<board> '
  cd /tmp && tar xzf src.tar.gz && cd src
  # sqlite3.o 跨部署复用（tar 只含 .c/.h，不会被覆盖）
  [ -f sqlite3.o ] || gcc -c -O2 -DSQLITE_THREADSAFE=0 sqlite3.c -o sqlite3.o

  # 编译所有 CGI（统一命令）
  for src in login.cgi.c logout.cgi.c main.cgi.c network.cgi.c action.cgi.c \
             user_list.cgi.c user_create.cgi.c user_passwd.cgi.c \
             user_toggle.cgi.c user_delete.cgi.c user_change_pass.cgi.c; do
    name=$(echo $src | sed "s/\.cgi\.c//" | sed "s/\.c//").cgi
    gcc -Wall -O2 -o $name $src common.c auth.c gate.c users.c remote.c sha256.c sqlite3.o -lpthread -ldl
  done

  # 初始化数据库
  gcc -Wall -O2 -o db_init db_init.c auth.c gate.c common.c sha256.c sqlite3.o -lpthread -ldl
  mkdir -p /var/db
  ./db_init admin
  chown -R www-data:www-data /var/db

  # 安装
  cp *.cgi /home/www/cgi-bin/
  chown www-data:www-data /home/www/cgi-bin/*.cgi
  chmod 755 /home/www/cgi-bin/*.cgi
  cp control_panel.html index.html style.css change.html /home/www/
'
```

### 密码策略与迁移（ADR-0002）

- **全新部署**：`db_init admin` 创建的 root 处于「首次登录强制改密」状态——登录成功后会被
  302 到 `/change.html`，改完密码（须满足策略）才能进入控制面板。root/admin 无豁免。
- **存量升级**：旧库无需手动迁移——首个 CGI 进程运行时 auth_init 会自动
  `ALTER TABLE users ADD COLUMN password_changed_at ... DEFAULT 0`，存量用户一律视为
  已过期 → 首次登录强制改密（旧密码仍可登录，只是先改密）。
- **重设密码**：root 密码过期时不会被锁死（登录 → 强制改密页）；若忘记密码，可重跑
  `./db_init <新密码>`（root 已存在 → 更新密码并重置计时）。
- **到期判定依赖板载时钟**（无 RTC 电池时重启时间错乱可能误判到期），不引入 NTP。

### 运行测试

宿主机单测（无需板子，x86_64 gcc 编译 + 临时 SQLite 库）：

```bash
./test_gate.sh
```

`test_gate.sh` 内置四层：
- CGI 级门卫测试（gate 阶梯 + 登录/登出 + 用户 CRUD 冒烟）
- API 级用户模块测试（`test_users.c` 直接断言 users 模块的不变量：自删/删 root/禁最后
  root 拒绝、审计行 target_user_id 正确）
- 串口离线测试：`test_remote.c` 通过 pty 假板（`test_fake_board.py`）驱动 remote 模块，
  覆盖重试、超时、ERR、乱码响应与板表查表；`test_gate.sh` 内另有 4 项 network.cgi 冒烟
  （`REMOTE_SERIAL_DEVICE_OVERRIDE` 指向 pty，端到端验证 JSON 形状与未知 port 拒绝）
- 前端注册表一致性：`test_frontend.py` 解析 `control_panel.html` 的 `BOARDS` 与
  `remote.c` 板表，断言 port 键双向一致（渲染本身由浏览器截图核对，curl 测不到 JS 生成物）

板端端到端测试（需 LubanCat + Remote Board 在线）：

```bash
./test_suite.sh <board_ip>
```

> 门卫模块（gate.c）读取 `DB_PATH` 环境变量覆盖数据库路径（默认 `/var/db/myapp.db`），
> 使 CGI 二进制可在宿主机以受控环境变量离线运行——`test_gate.sh` 依赖此特性。
> 同理，remote 模块读取 `REMOTE_SERIAL_DEVICE_OVERRIDE` 覆盖板子的串口设备路径，
> 使串口测试可用 pty 假板离线进行。

## 安全模型

| 层面 | 机制 |
|------|------|
| 传输 | TLS 1.2+，HTTP→HTTPS 强制跳转，HSTS |
| 认证 | SHA-256 密码哈希（10 万轮 + 随机盐），`$5$` modular crypt 格式 |
| 密码策略 | 10-64 字符（大小写/数字/特殊符号四类），90 天有效期，到期强制改密，root 无豁免 |
| CSRF | 双 Cookie：`session_id`(HttpOnly) + `csrf_token`(JS可读)，POST 需回传 |
| 权限 | root/admin 角色分离，root 保护（不可自删/不可禁最后一个 root） |
| 审计 | `audit_log` 表记录所有管理操作 |
| SQL | 参数化查询（SQLite prepared statements） |

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

## Board 配置

| Board | IP | 串口 | 波特率 | CGI 参数 |
|-------|-----|------|--------|----------|
| Board 1 | 192.168.8.201 | `/dev/ttyS7` | 115200 | 默认（无 port 参数） |
| Board 2 | 192.168.8.99 | `/dev/ttyS4` | 38400 | `port=s4` |

Web 控制面板的板卡选项卡由前端注册表（`control_panel.html` 的 `BOARDS`）驱动渲染：
单一表单按当前 Board 重载各自配置，Board 3/4 以占位 tab 预留。

板表有两处、共享 port 键：协议路由表在 `src/remote.c`（port → device/baud），
UI 注册表在前端 `BOARDS`（tab 标签 / port 参数 / 占位板）。`test_frontend.py` 断言
两者双向一致（加板漏改任一侧即红）。CGI 经 `remote_board_lookup()` 查表，未知 port
返回错误而非静默落到 Board 1。Board 3 上线 = 两张表各加一行。

## Remote Board 部署

将 `handler.sh` 部署到 Remote Board 并修改顶部的 `DEV` 和 `BAUD`：

```bash
scp handler.sh root@<remote_ip>:/usr/local/bin/
ssh root@<remote_ip> '
  sed -i "s|DEV=/dev/ttyS7|DEV=/dev/ttyS4|" /usr/local/bin/handler.sh
  sed -i "s|BAUD=115200|BAUD=38400|" /usr/local/bin/handler.sh
  chmod +x /usr/local/bin/handler.sh
  rm -f /var/run/serial_protocol.lock /var/run/serial_protocol.pid
  nohup /usr/local/bin/handler.sh > /tmp/handler.log 2>&1 &
'
```

handler.sh 特性：PID 清理、串口断开自动重连、flock 防重复启动。

## License

MIT
