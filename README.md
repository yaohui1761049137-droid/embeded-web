# embeded-web — Lighttpd + CGI Multi-User Web System

嵌入式 Web 管理系统，基于 **Lighttpd 1.4.59 + C CGI**，运行在 LubanCat ARM aarch64 开发板上。

通过串口 (`/dev/ttyS7`) 与 Remote Board 通信，实现网络参数的远程查询和配置。

## 功能

- **HTTPS 加密传输**（TLS 1.2+，HSTS，自签名证书）
- **多用户认证**（root / admin 角色，SHA-256 密码哈希，10 万轮迭代）
- **Session + CSRF 双重防护**（HttpOnly Cookie + CSRF Token）
- **串口通信**（termios2 / BOTHER 自定义波特率，PING 预热 + 重试）
- **用户管理 API**（root 可创建/启用/禁用/删除 admin 用户，含审计日志）
- **SQLite 存储**（WAL 模式，多进程 CGI 并发安全）
- **31 项自动化测试**（`test_suite.sh`）

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
┌─────────────────────┐      Serial /dev/ttyS7     ┌──────────────────┐
│  CGI Programs (C)   │  ───────────────────────→   │  Remote Board    │
│  + auth.c (SQLite)  │  ←───────────────────────   │  handler.sh      │
│  + common.c (串口)   │     115200 8N1              │  (Rockchip)      │
└─────────────────────┘                              └──────────────────┘
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
│   ├── control_panel.html     控制面板（6 Tab）
│   └── style.css              全局样式
└── test_suite.sh              31 项端到端测试
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
  cd /tmp && tar xzf src.tar.gz
  gcc -c -O2 -DSQLITE_THREADSAFE=0 sqlite3.c -o sqlite3.o

  # 编译所有 CGI（统一命令）
  for src in login.cgi.c logout.cgi.c main.cgi.c network.cgi.c action.cgi.c \
             user_list.cgi.c user_create.cgi.c user_passwd.cgi.c \
             user_toggle.cgi.c user_delete.cgi.c; do
    name=$(echo $src | sed "s/\.cgi\.c//" | sed "s/\.c//").cgi
    gcc -Wall -O2 -o $name $src common.c auth.c sha256.c sqlite3.o -lpthread
  done

  # 初始化数据库
  gcc -Wall -O2 -o db_init db_init.c auth.c sha256.c sqlite3.o -lpthread
  mkdir -p /var/db
  ./db_init admin
  chown -R www-data:www-data /var/db

  # 安装
  cp *.cgi /home/www/cgi-bin/
  chown www-data:www-data /home/www/cgi-bin/*.cgi
  chmod 755 /home/www/cgi-bin/*.cgi
  cp control_panel.html index.html style.css /home/www/
'
```

### 运行测试

```bash
./test_suite.sh <board_ip>
```

## 安全模型

| 层面 | 机制 |
|------|------|
| 传输 | TLS 1.2+，HTTP→HTTPS 强制跳转，HSTS |
| 认证 | SHA-256 密码哈希（10 万轮 + 随机盐），`$5$` modular crypt 格式 |
| Session | 64 字符随机 hex token，HttpOnly Cookie，1 小时过期 |
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

通过 `/dev/ttyS7` (115200 8N1) 与 Remote Board 通信：

| 命令 | 方向 | 响应 |
|------|------|------|
| `PING` | → | `OK` |
| `REMOTE_GET_IPV4` | → | `OK <ip> <mask> <gateway>` |
| `REMOTE_SET_IPV4 <ip> <mask> <gw>` | → | `OK` |
| `REMOTE_GET_IPV6` | → | `OK <addr/prefix>` |
| `REMOTE_SET_IPV6 <addr/prefix>` | → | `OK` |

## License

MIT
