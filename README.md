# embeded-web — Lighttpd + CGI Multi-User Web System

嵌入式 Web 管理系统，基于 **Lighttpd 1.4.59 + C CGI**，运行在 LubanCat ARM aarch64 开发板上。

通过 NetworkManager（nmcli）对开发板本机网卡（eth0-3）进行网络参数配置：静态 IPv4（IP/掩码/网关）、
可选 DNS、静态 IPv6，支持网卡切换与 eth0 变更自动回滚。

「时间服务校验设置」Tab 展示 PPS+TOD+chrony 授时状态（chronyc sources/tracking + pps_tod 状态文件），
并可切换 UT986 GNSS 接收机工作模式（GNSS 全系统 / GPS / 北斗 / Galileo / GLONASS）。

## 功能

- **HTTPS 加密传输**（TLS 1.2+，HSTS，自签名证书）
- **多用户认证**（root / admin 角色，SHA-256 密码哈希，10 万轮迭代）
- **密码策略**（10-64 字符四类组合，90 天有效期，到期强制改密，用户自助改密）
- **Session + CSRF 双重防护**（HttpOnly Cookie + CSRF Token）
- **本地网卡配置**（eth0-3 四个网口，通过 NetworkManager nmcli 读写，无串口层）
- **多 NIC 支持**（Web 控制面板网口标签页一键切换，独立配置互不干扰）
- **唯一网关约束**（新配置生效时自动清空其他网口的网关，避免多默认路由）
- **eth0 回滚保护**（变更后 3 分钟登录信号超时自动回滚旧配置，掉电重启后恢复）
- **用户管理 API**（root 可创建/启用/禁用/删除 admin 用户，含审计日志）
- **时间同步监控**（PPS+TOD+chrony 实时状态：授时源/系统偏差/RMS/频偏/看门狗，3 秒轮询）
- **接收机模式切换**（UT986 五种模式，CSRF + 枚举白名单后写 ttyS7 固定载荷，掉电保持）
- **NTP 访问控制**（chrony allow/deny 黑白名单表格，root 专属；新增走 chronyc 运行时 ACL
  即时生效、删除重启重载；CIDR 双重校验 + CSRF，操作入审计）
- **NTP 流量监控**（近 24h 请求量趋势 + 四网口请求量柱状图；chronyc serverstats 与 iptables
  每网口计数每分钟采样落盘；uPlot 渲染，悬停读数 + 摘要数字卡）
- **业务审计日志**（网络配置修改、接收机模式切换、NTP 黑白名单变更自动记录操作者和时间到 `audit_log` 表）
- **SQLite 存储**（WAL 模式，多进程 CGI 并发安全）
- **120 项自动化测试**（`test_gate.sh`，宿主机离线全绿，含 NMCLI_OVERRIDE / CHRONYC_OVERRIDE / NTPMON_HELPER_OVERRIDE 假命令）

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
┌──────────────────────────────┐
│  CGI Programs (C)            │
│  + gate.c (门卫阶梯+强制改密)│
│  + auth.c (SQLite/审计/策略) │
│  + users.c (用户管理/自改密) │
│  + nmcli.c (本地网卡配置库)  │  ← fork/execvp，无 shell，参数白名单校验
│  + timesync.c (时间同步/串口)│  ← chronyc 读取 + 固定载荷写 ttyS7
│  + ntpmon.c (NTP 监控/ACL)  │  ← 采样 CSV 解析 + chrony ACL helper 调用
│  + common.c (HTTP/POST)      │
│                              │
│  network.cgi ?port=eth0 ─────│  ← nmcli 读写 + 回滚编排 + 审计
│  timesync.cgi ?action=... ───│  ← chrony/pps_tod 状态 + UT986 模式切换
│  ntpmon.cgi ?action=... ─────│  ← NTP 黑白名单 + 请求量图表数据
│                              │
│  audit_log (业务操作)        │
└──────────────┬───────────────┘
       │                       │
       ▼                       ▼
┌──────────────┐   ┌───────────────────────────────┐
│   SQLite DB   │   │  NetworkManager (系统服务)     │
│  /var/db/     │   │  nmcli ← sudoers 白名单        │
└──────────────┘   │  配置文件在 /etc/NetworkManager │
                   └───────────────────────────────┘
                   ┌───────────────────────────────┐
                   │  chrony / pps_tod (系统服务)   │
                   │  chronyc 本地查询（无特权）     │
                   │  ACL: acl-web.conf ← helper    │
                   │  /run/pps_tod 状态文件（0644）  │
                   │  /dev/ttyS7 ← www-data dialout │
                   │  ntp-stats.timer → 每分钟采样  │
                   │  iptables 每网口 udp/123 计数  │
                   └───────────────────────────────┘
```

CGI 以 www-data 身份运行，经 `/etc/sudoers.d/99-www-nmcli` 的 NOPASSWD 白名单调用 nmcli，
参数在白名单和 nmcli.c 双重校验（IP/掩码/网关/DNS/IPv6 格式 + 网口名枚举）。

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
│   ├── nmcli.h / nmcli.c      本地网卡配置库（fork/execvp 调用 nmcli + 参数校验 + 掩码⇄前缀）
│   ├── timesync.h / timesync.c 时间同步库（chronyc 执行/解析、状态文件读取、UT986 模式切换）
│   ├── ntpmon.h / ntpmon.c    NTP 监控库（ACL 规则读写与校验、采样 CSV 解析、ACL helper 调用）
│   ├── common.h / common.c    CGI 公共库（HTTP/POST 解析）
│   ├── login.cgi.c            登录（DB 验证 + 双 Cookie + 回滚确认标记）
│   ├── logout.cgi.c           登出（销毁 Session）
│   ├── main.cgi.c             控制面板入口（Session 校验）
│   ├── network.cgi.c          网络配置（nmcli 读写 + 回滚编排 + 审计）
│   ├── timesync.cgi.c         时间同步（PPS/TOD 状态 + 接收机模式，见 Tab「时间服务校验设置」）
│   ├── ntpmon.cgi.c           NTP 监控（黑白名单 + 流量图表，见 Tab「服务监控及报警」）
│   ├── db_init.c              数据库初始化（创建 root 用户）
│   ├── user_list.cgi.c        [root] 用户列表
│   ├── user_create.cgi.c      [root] 创建用户
│   ├── user_passwd.cgi.c      [root] 重置密码
│   ├── user_toggle.cgi.c      [root] 启用/禁用
│   ├── user_delete.cgi.c      [root] 删除用户
│   └── user_change_pass.cgi.c 用户自改密（当前密码验证 + 策略 + 踢会话）
├── www/
│   ├── index.html             登录页面
│   ├── control_panel.html     控制面板（7 Tab；网口由 NICS 注册表驱动，时间同步/NTP 监控见对应 Tab）
│   ├── change.html            修改密码页面（自改密入口）
│   ├── style.css              全局样式
│   ├── uPlot.iife.min.js      图表库（vendored v1.6.32，MIT，离线可用）
│   └── uPlot.min.css          图表库样式（vendored）
├── docs/adr/                  ADR 决策记录（0002 = 密码策略，0003 = 本地网络配置）
├── docs/ntp-monitor.md        NTP 监控设计说明（数据源、ACL 语义与重启策略）
├── rollback_watchdog.sh       eth0 回滚看门狗（systemd-run 触发 + 开机恢复双入口）
├── rollback-recover.service   开机回滚恢复 oneshot 服务
├── chrony_acl_apply.sh        NTP 黑白名单应用助手（root，经 sudoers 白名单调用）
├── ntp_stats_sample.sh        NTP 统计采样器（每分钟，iptables + chronyc → CSV）
├── ntp-stats.service/.timer   采样器 systemd 定时单元
├── fake_nmcli.sh              宿主机离线测试用假 nmcli（NMCLI_OVERRIDE）
├── fake_chronyc.sh            宿主机离线测试用假 chronyc（CHRONYC_OVERRIDE）
├── fake_ntp_acl_helper.sh     宿主机离线测试用假 ACL helper（NTPMON_HELPER_OVERRIDE）
├── test_suite.sh              板端端到端测试（真实 nmcli）
├── test_gate.sh               宿主机门卫单测（CGI 级，无需板子，120 项）
├── test_users.c               宿主机用户模块单测（API 级，无需板子）
└── test_frontend.py           前端 NICS 注册表 ↔ nmcli.c 网口枚举 + CGI action 契约一致性测试（离线）
```

## 快速开始

### 依赖

- **目标板**：ARM aarch64，Debian Buster，gcc 8.3+
- **运行时**：Lighttpd 1.4.59+ (with mod_openssl)，OpenSSL 1.1.1+，PCRE
- **网络**：NetworkManager（Debian Buster 自带 1.14），nmcli
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

### 部署 Phase 2（多用户系统 + 本地网络配置）

```bash
# 1. 打包源码并上传
tar czf src.tar.gz src/*.c src/*.h
scp src.tar.gz www/* root@<board>:/tmp/

# 2. 编译（含 nmcli.c）
ssh root@<board> '
  cd /tmp && tar xzf src.tar.gz && cd src
  # sqlite3.o 跨部署复用（tar 只含 .c/.h，不会被覆盖）
  [ -f sqlite3.o ] || gcc -c -O2 -DSQLITE_THREADSAFE=0 sqlite3.c -o sqlite3.o

  for src in login.cgi.c logout.cgi.c main.cgi.c network.cgi.c \
             user_list.cgi.c user_create.cgi.c user_passwd.cgi.c \
             user_toggle.cgi.c user_delete.cgi.c user_change_pass.cgi.c \
             timesync.cgi.c ntpmon.cgi.c; do
    name=$(echo $src | sed "s/\.cgi\.c//" | sed "s/\.c//").cgi
    gcc -Wall -O2 -o $name $src common.c auth.c gate.c users.c nmcli.c timesync.c ntpmon.c sha256.c sqlite3.o -lpthread -ldl
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
  cp control_panel.html index.html style.css change.html uPlot.iife.min.js uPlot.min.css /home/www/
'

# 3. www-data sudoers 白名单（nmcli 调用入口，务必先于任何网络配置使用）
ssh root@<board> '
  cat > /etc/sudoers.d/99-www-nmcli <<EOF
www-data ALL=(ALL) NOPASSWD: /usr/bin/nmcli -t -f NAME\\,UUID connection show, /usr/bin/nmcli -t connection show *, /usr/bin/nmcli -t device show *, /usr/bin/nmcli connection show *, /usr/bin/nmcli connection add type ethernet ifname *, /usr/bin/nmcli connection modify *, /usr/bin/nmcli connection up *, /usr/bin/systemd-run --on-active=180 /usr/local/bin/rollback_watchdog.sh
EOF
  chmod 440 /etc/sudoers.d/99-www-nmcli
  visudo -cf /etc/sudoers.d/99-www-nmcli

  # 3b. www-data 加入 dialout 组（timesync.cgi 写 /dev/ttyS7 控制 UT986 接收机）
  usermod -aG dialout www-data
  systemctl restart lighttpd   # 组变更对新 CGI 进程生效

  # 3c. NTP 监控（黑白名单 + 流量图表）
  #     助手脚本 + 采样器 + 采样定时器
  cp chrony_acl_apply.sh ntp_stats_sample.sh /usr/local/bin/
  chmod 755 /usr/local/bin/chrony_acl_apply.sh /usr/local/bin/ntp_stats_sample.sh
  cp ntp-stats.service ntp-stats.timer /etc/systemd/system/
  systemctl enable --now ntp-stats.timer
  #     chrony ACL 托管文件 + include（一次性；随后重启 chrony 激活）
  #     现有生效的 allow/deny 行迁入 acl-web.conf（行为不变）
  printf "# Web-managed NTP access rules (chrony allow/deny) - do not edit by hand.\nallow 192.168.137.0/24\n" > /etc/chrony/acl-web.conf
  chmod 644 /etc/chrony/acl-web.conf
  grep -q "include /etc/chrony/acl-web.conf" /etc/chrony/chrony.conf || {
      sed -i '/^allow /d; /^deny /d' /etc/chrony/chrony.conf
      echo "include /etc/chrony/acl-web.conf" >> /etc/chrony/chrony.conf
  }
  systemctl restart chrony
  #     www-data sudoers 白名单（ACL helper 入口）
  cat > /etc/sudoers.d/99-www-ntpacl <<EOF
www-data ALL=(root) NOPASSWD: /usr/local/bin/chrony_acl_apply.sh add allow *, /usr/local/bin/chrony_acl_apply.sh add deny *, /usr/local/bin/chrony_acl_apply.sh remove *
EOF
  chmod 440 /etc/sudoers.d/99-www-ntpacl
  visudo -cf /etc/sudoers.d/99-www-ntpacl

  # 4. 回滚看门狗 + 开机恢复服务
  cp rollback_watchdog.sh /usr/local/bin/
  chmod 755 /usr/local/bin/rollback_watchdog.sh
  cp rollback-recover.service /etc/systemd/system/
  systemctl enable rollback-recover.service
'
```

### 网络配置说明（ADR-0003）

- **配置源**：NetworkManager profile 是唯一事实源，`network.cgi` 只做读写映射
  （IP/掩码 ⇄ CIDR，掩码校验连续 1 位）。
- **网口**：eth0-3 由前端 `NICS` 注册表（`control_panel.html`）与 `nmcli.c` 的 `g_nics`
  枚举双向一致（`test_frontend.py` 断言），默认激活 eth0。
- **IPv4**：静态 IP/掩码/网关；**DNS**：1-2 个，逗号分隔，可留空；**IPv6**：手动
  地址/前缀，留空则 `ipv6.method=ignore`（NM 1.14 不支持 `disabled`）。
- **唯一网关**：配置带网关的新网口时自动清空其他网口网关；eth0 网关优先于 4G
  （4G 由 `/opt/auto_4G.sh` 看门狗把 wwan0 默认路由 metric 调到 200）。
- **回滚保护（仅 eth0）**：写配置 → 写 `/var/db/rollback.json`（旧值+时间戳）→
  `systemd-run --on-active=180` 起看门狗。3 分钟内出现登录信号（web 登录写
  `confirmed=1`，或 SSH auth.log 有更新的 Accepted 记录）→ 保留新配置并清文件；
  否则回滚旧值。掉电重启时 `rollback-recover.service` 在开机执行同一检查。

### 时间同步子系统（PPS + TOD + chrony）

板载时基由**独立的授时子系统**提供（独立于本 Web 项目交付），Web 侧只做只读展示
与受限控制：

```
UT986 接收机 ── 1PPS 秒脉冲 ─▶ GPIO3_A5 ─(gpiochip 边沿事件,内核时间戳)─┐
             ── NMEA $GNRMC ─▶ ttyS7   ─(自实现 RMC/ZDA 解析)────────────┤
                                                                        ▼
                    pps_tod 守护进程（进程内"整秒+亚秒"配对；纯用户态，零内核改动）
                      ├─ 粗同步引导：|偏差|>1s 时直写系统钟（无 RTC 板重启场景）
                      ├─ 写 SysV SHM(0x4e545030) ─▶ chrony refclock SHM 0（单源）
                      └─ 状态出口 /run/pps_tod/{status,watchdog.state}（逐秒，0644）
                                                                        ▼
                    chrony 3.4（PLL 锁钟，实测 µs 级；`chronyc sources` = `#* PPS`）
```

- **Web 侧数据通路**：
  - `/run/pps_tod/{status,watchdog.state}` →「时间服务校验设置」Tab（授时状态、看门狗）；
  - `chronyc` 本地查询（sources/tracking/serverstats）→ 时间 Tab 与「服务监控及报警」Tab 图表；
  - 接收机模式切换：`timesync.cgi` 向 `/dev/ttyS7` **只写**固定字节
    `$CFGGNSS/$CFGSAVE` 载荷（不读串口、不碰 termios，避免抢走 pps_tod 的 NMEA）。
- **pps_tod 日志**（`/var/log/pps_tod/pps_tod_YYYY-MM-DD.log`，人工排查用）：
  - 按天滚动（次日封存压缩为 .gz，长期保留，不按时间清理）；总容量超过 **100MB** 时
    才自动删除最旧的日志（`-R <MB>` 可调，`0` = 不清理；当天文件永不删除）；
  - 周期性行 = **每分钟 1 条 `STAT:` 统计汇总**（合格样本数 / offset min-avg-max / 锚点数 /
    门控·无边沿·无锚点·坏 TOD 四个计数器），约 160KB/天；`-L 0` 可临时回退逐秒原始行；
  - 事件行（bootstrap / gated / idle / GPIO / 启停 / 日志清理等）逐条保留，带 **HH:MM:SS** 前缀；
  - 日志样例：
    ```
    21:10:20 TOD: opened /dev/ttyS7
    STAT: window=60s ts=2026-09-16 21:05:38 n=61 off_us min=-58 avg=-1 max=+69 tod_n=61 gated=0 noedge=0 noanchor=0 badtod=0
    ```
- **可靠性配套**：`pps_tod_watchdog`（样本断流 30s → 有界重启；参考丢失 → DEGRADED 告警，
  1 小时 3 次上限；7 项故障注入实测通过）+ RTC 每 6h 保存 + drift 消毒；
  「服务监控及报警」Tab 提供 NTP 黑白名单与请求量图表（见 `docs/ntp-monitor.md`）。
- 方案细节与验证记录见 `docs/gps-pps-time-sync.md` 及授时子系统交付文档
  （`PPS_TOD_chrony-会话交付总结.md`、`鲁班猫2N-PPS-TOD-chrony-实施记录.md`）。

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

宿主机单测（无需板子、无需 NetworkManager、无需 sudo，x86_64 gcc 编译 + 临时 SQLite 库）：

```bash
./test_gate.sh
```

`test_gate.sh` 内置四层，共 120 项：
- CGI 级门卫测试（gate 阶梯 + 登录/登出 + 用户 CRUD 冒烟）
- API 级用户模块测试（`test_users.c` 直接断言 users 模块的不变量：自删/删 root/禁最后
  root 拒绝、审计行 target_user_id 正确）
- 网络配置测试（Test 7-7f）：`NMCLI_OVERRIDE` 指向 `fake_nmcli.sh` 离线跑 network.cgi——
  eth0 读回、SET + 回滚文件断言（`ROLLBACK_FILE`）、非法网口拒绝、eth1 自动建 profile、
  网关自动迁移；`NMCLI_NO_SUDO=1` 跳过 sudo 前缀
- 时间同步测试（Test 27-27g）：`CHRONYC_OVERRIDE` 指向 `fake_chronyc.sh` + `TIMESYNC_DEV`/
  `TIMESYNC_STATUS_DIR`/`TIMESYNC_MODE_FILE` 指向临时文件，离线跑 timesync.cgi——status
  解析（chrony CSV 含 hex 解码、pps_tod/看门狗状态文件）、状态文件缺失降级、setmode 无
  CSRF/非法模式拒绝、写入串口的 $CFGGNSS/$CFGSAVE 载荷与校验和逐字节断言、接收机模式
  状态文件的持久化与状态回读（last_mode）
- NTP 监控测试（Test 28-28k）：`NTPMON_ACL_FILE`/`NTPMON_CSV`/`NTPMON_HELPER_OVERRIDE` 指向
  临时文件 + 假 helper，离线跑 ntpmon.cgi——stats 解析（含计数器归零断点与近 1h 增量）、
  CSV 缺失降级、admin 只读放行/改 ACL 403、CSRF/非法 CIDR/重复与不存在规则拒绝、
  helper 参数逐字断言与失败透传
- 前端注册表一致性：`test_frontend.py` 解析 `control_panel.html` 的 `NICS` 与
  `nmcli.c` 的 `g_nics`，断言网口列表双向一致；并断言 timesync.cgi / ntpmon.cgi 的各
  action 在前端 URL 与 C 端处理器同步存在（渲染本身由浏览器截图核对）

板端端到端测试（需 LubanCat 在线，配置真实 nmcli；凭据按环境传入）：

```bash
./test_suite.sh <board_ip>
```

> 门卫模块（gate.c）读取 `DB_PATH` 环境变量覆盖数据库路径（默认 `/var/db/myapp.db`），
> 使 CGI 二进制可在宿主机以受控环境变量离线运行——`test_gate.sh` 依赖此特性。
> 同理，nmcli 调用读取 `NMCLI_OVERRIDE` 覆盖可执行文件、`NMCLI_NO_SUDO=1` 去掉 sudo，
> 使网络配置测试可用假命令离线进行。timesync.cgi 读取 `CHRONYC_OVERRIDE`（chronyc 可
> 执行文件）、`TIMESYNC_DEV`（串口设备，默认 /dev/ttyS7）、`TIMESYNC_STATUS_DIR`
> （状态文件目录，默认 /run/pps_tod）、`TIMESYNC_MODE_FILE`（接收机模式状态文件，默认
> /var/db/ut986_mode）。ntpmon.cgi 读取 `NTPMON_ACL_FILE`（ACL 托管文件，默认
> /etc/chrony/acl-web.conf）、`NTPMON_CSV`（采样文件，默认 /var/db/ntp_stats.csv）、
> `NTPMON_HELPER_OVERRIDE`（ACL helper 可执行文件，板上默认经 sudo 调用
> /usr/local/bin/chrony_acl_apply.sh）——使各测试均可离线运行。

## 安全模型

| 层面 | 机制 |
|------|------|
| 传输 | TLS 1.2+，HTTP→HTTPS 强制跳转，HSTS |
| 认证 | SHA-256 密码哈希（10 万轮 + 随机盐），`$5$` modular crypt 格式 |
| 密码策略 | 10-64 字符（大小写/数字/特殊符号四类），90 天有效期，到期强制改密，root 无豁免 |
| CSRF | 双 Cookie：`session_id`(HttpOnly) + `csrf_token`(JS可读)，POST 需回传 |
| 权限 | root/admin 角色分离，root 保护（不可自删/不可禁最后一个 root） |
| 系统命令 | nmcli 经 www-data sudoers NOPASSWD 白名单放行；参数先白名单/格式双重校验，fork/execvp 无 shell |
| 串口控制 | timesync.cgi 仅向 /dev/ttyS7 写固定字节载荷（5 种 UT986 模式 × 固定校验和），枚举白名单 + CSRF；写入经 dialout 组授权，不读串口（避免抢走 pps_tod 的 NMEA） |
| NTP 访问控制 | ntpmon.cgi 改 ACL 限 root（角色门）+ CSRF；CIDR 在 CGI 与 root 助手内双重校验，助手经 sudoers NOPASSWD 白名单调用（步骤 3c）；新增走 chronyc 运行时 ACL（即时生效），删除重启 chrony 重载；全部入审计 |
| 流量采样 | iptables 每网口 udp/123 计数规则为"只计数不拦截"（ACCEPT 与当前默认策略一致）；采样器读取 chronyc 与 iptables 计数写 /var/db/ntp_stats.csv（0644） |
| 审计 | `audit_log` 表记录所有管理操作 |
| SQL | 参数化查询（SQLite prepared statements） |

## Cookie 设计

```
Set-Cookie: session_id=<64 hex>; Path=/; HttpOnly; Secure; SameSite=Lax; Max-Age=3600
Set-Cookie: csrf_token=<32 hex>; Path=/; Secure; SameSite=Lax; Max-Age=3600
```

- `session_id` — HttpOnly，XSS 无法窃取，服务端 SQLite 校验
- `csrf_token` — JS 可读，POST 时在 body 中回传，服务端比对

## 网口配置

| 网口 | 说明 |
|------|------|
| eth0 | 主网口（管理口），网关优先，变更带 3 分钟回滚保护 |
| eth1-3 | 扩展网口，无 profile 时首次设置自动创建持久 profile |

前端网口标签页由 `control_panel.html` 的 `NICS` 注册表驱动，默认激活 eth0；eth1-3
未配置时输入框为空。加网口 = 两处各加一行（前端 `NICS` + `nmcli.c` 的 `g_nics`），
`test_frontend.py` 防漏改。

## License

MIT
