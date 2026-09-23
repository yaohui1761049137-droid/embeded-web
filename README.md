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
- **逐网口授时监测**（`ntp-nic-monitor.service`：AF_PACKET 抓入向请求给出每口**客户端明细**，
  iptables OUTPUT 计数给出每口**应答量与应答率**；快照 `/var/db/ntp_nic.json` 每 5 秒原子更新，
  前端「各网口授时服务状态」+「客户端明细」两块面板）
- **系统日志查看**（root 专属 Tab：pps_tod 当日日志 / pps_tod 看门狗 / lighttpd
  access·error 四类来源，行数与关键字过滤；`log.cgi` 用固定 id→路径白名单，只读文件尾部 256KB）
- **业务审计日志**（网络配置修改、接收机模式切换、NTP 黑白名单变更自动记录操作者和时间到 `audit_log` 表）
- **SQLite 存储**（WAL 模式，多进程 CGI 并发安全）
- **171 项自动化测试**（`test_gate.sh`，宿主机离线全绿，含 NMCLI_OVERRIDE / CHRONYC_OVERRIDE / NTPMON_HELPER_OVERRIDE / LOGVIEW_ROOT / NTPMON_NIC_FILE 假命令与假数据）
- **逐网口端到端测试**（`test_ntp_nic.sh`，真实注入 + 物理收包计数交叉验证，25 项）

## 四项需求现状

> 2026-09-17 · 完整方案、验证记录与遗留边界见 [`docs/requirements-summary-2026-09-17.md`](docs/requirements-summary-2026-09-17.md)

| # | 需求 | 现状 |
|---|---|---|
| 1 | 确认 system 日志功能正常 | ✅ 查看功能已实现 · ⚠️ 板端日志治理部分完成 |
| 2 | 每个授时网口的授时监测 | ✅ 已实现，压测验证通过 |
| 3 | chrony 黑白名单：Web 还是 console | ✅ Web 为受管路径（含同网段 allow/deny 共存） |
| 4 | TOD 时码比对 + 连续异常重置 | ⚠️ 已实现并故障注入验证，存在设计固有的 ±0.5s 绝对相位限制 |

**① 系统日志** —— 新增 root 专属「系统日志」Tab（`src/log.cgi.c`）：固定 id→路径白名单
（穿越在结构上不可能）、只读文件尾部 256KB、行数与关键字过滤。
核验发现板端 `/var/log` 曾积压 **694 MB**：hostapd 因缺配置文件每 2 秒失败刷屏（约 10 MB/天，
占 `daemon.log` 行数 56.8%），叠加无 RTC 电池导致时钟跳变、logrotate 日期记账失真而从不轮转。
已停用 hostapd、规范轮转并压缩归档，降至 **83 MB**；RTC 每 6h 持久化由从未执行的
`/etc/cron.d` 条目改为 systemd timer。

**② 逐网口授时监测** —— 新增常驻采集进程 `ntp-nic-monitor.service`。
实测确认这块板子的 **AF_PACKET 看不到出向包**（注入 9 包、chronyd 实发 9 包、userspace 见 0），
因此走两条路：**入向用 AF_PACKET 抓客户端身份，出向用 iptables OUTPUT 计数得应答率**。
前端新增「各网口授时服务状态」「客户端明细」两块面板，并加**页级时间范围开关**
（近 1h / 5h / 24h，控件用连体矩形以区别于网口的圆胶囊）。
压测 **10k/s 每口、双口并发 20k/s：100% 送达、100% 应答、零串扰**；
监控链路在 20k pps 下与内核计数**逐包相等**；实测上限约 **34,500 请求/秒**（chronyd 单线程收包）。

**③ chrony 黑白名单** —— 结论是 **Web 为受管路径，console 仅应急**：
Web（root + CSRF）走 `chronyc allow|deny` 即时生效 + 持久化 + 审计；删除改文件 + 重启重载。
console 手工 `chronyc` 的规则**不持久、不入审计、Web 表格看不到也删不掉**，边界已写入
[`docs/ntp-monitor.md`](docs/ntp-monitor.md) §2.1。本轮另补齐 **同网段 allow 与 deny 共存**
（chrony 按最长前缀匹配、等前缀下 deny 优先）：翻转网段放行状态不再需要"先删后加"
（删除会重启 chronyd、清零请求计数并重锁 PPS 约 1 分钟），UI 两行各自携带动作、不会误删。

**④ TOD 时码比对** —— 修复四处缺口：**锚点序列仲裁**（锚的秒值连续递增 ≥3 次才允许写钟，
源跳变时不写）、**死区自愈**（0.5~1.0s 区间原先既不进 SHM 也不被纠正，现连续门控 ≥10 次强制步进）、
**小时配额**（超限转 `CAPPED` 停止写钟，防错误参考把时钟来回拖）、注释与实现对齐。
故障注入 4 项验证通过（0.7s 死区自愈 / 1.5s 快速通道 / 配额闸 / 正常态回归）。
**遗留限制**：`S_round = round(边沿本地时间)` 使亚秒相位误差对系统不可见，
绝对精度存在约 **±0.5s** 的锁定不确定性（**先于本轮存在**）；
此精度指"与 TOD 源整秒一致"，chrony 自述的 µs 级是**内部自洽口径**，不等于绝对 UTC 精度。

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
├── docs/foolproof-deploy.md   傻瓜式部署手册（Step1–Step9 全流程 + 踩坑速查总表）
├── docs/headless-deploy-guide.md  无 HDMI/无外网排障手册（按现象查：SSH/死锁/授时三层）
├── docs/timing-wiring-2026-09-20/ 授时链路接线图（照片标注版 + 原理图 + 接线表）
├── docs/diagnosis/            SSH 排障探针脚本 ×18（主机密钥损坏定案过程）
├── deploy/                    部署脚本 ×9（SSH 引导/构建/集成/overlay/授时栈/登录与功能验证）
├── debs/                      离线依赖 arm64 deb ×4（lighttpd/mod-openssl/xxhash/chrony 3.4）
├── pps_tod/                   PPS+TOD→chrony 用户态授时栈源码 ×10（含 README 溯源说明）
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

### 一键部署（推荐，幂等可重跑）

```bat
pip install paramiko
python deploy\deploy_all.py                    :: 全流程到功能验证（默认值见下）
python deploy\deploy_all.py --with-timing      :: 含 Step9 授时链路（需 UT986 接线）
python deploy\deploy_all.py --reboot --with-timing   :: 完整版（含断电重启复测）
```

- 默认参数：`--host 192.168.1.111 --user root --board-password root
  --panel-password Testpassword1234@@`，换板卡/密码用 flags 覆盖。
- **逐阶段幂等**：preflight → 离线依赖 → HTTPS → CGI 编译 → 面板密码 →
  系统集成（含 time-wait-sync mask）→ 功能验证 12/12 →（可选）授时栈+守卫 →
  （可选）重启复测；已部署的部分自动跳过，任一阶段失败即停并提示重跑，
  重跑不会重复已完成的动作。全部复用仓库里已真机验证的板端脚本，
  驱动只做编排与验收。
- 首次部署会在面板密码阶段自动完成首登强制改密
  （root/admin → 你指定的 `--panel-password`），结束时把密码记下来。
- 2026-09-23 实机验证：已部署板重跑 7/7 阶段 PASS（全跳过 + 功能 12/12）。

### 手动分步（兜底，Step1–Step9）

> 每一步的验收标准、失败对照与踩坑速查总表见
> [`docs/foolproof-deploy.md`](docs/foolproof-deploy.md)（Step1–Step9 全流程，
> 2026-09-20/21 已在无 HDMI、无外网的鲁班猫 2N 上两次真机实测通过
> ——功能 12/12 + 断电重启自恢复 + 授时链路 µs 级锁定）。
> 手册示例地址 `root@192.168.1.111`，换板卡时全局替换 IP 即可。
> 以下命令在 PC（Windows）上 git clone 本仓库后直接粘贴。

### 部署包（仓库自带，克隆即用，无需外网）

| 目录 | 内容 |
|---|---|
| `src/` `www/` `config/` | CGI 源码 / 前端 / lighttpd 配置（含本次实测修复） |
| `debs/` | 4 个 arm64 离线 deb：lighttpd 1.4.59 (bpo10) + mod-openssl + libxxhash0 + **chrony 3.4**（替换厂商 ntp） |
| `deploy/` | SSH 引导 / 板上构建 / 系统集成 / uart7 overlay / 授时栈部署 / 登录流与 12 步功能验证 |
| `pps_tod/` | PPS+TOD→chrony 授时栈源码 ×10（默认串口 ttyS3 **必须传 `-t /dev/ttyS7`**，GPIO 默认 chip3:line5 已对） |

### 依赖

- **目标板**：ARM aarch64，Debian Buster（鲁班猫官方镜像即可），gcc 8.3+
- **运行时**：Lighttpd 1.4.59+ (with mod_openssl)（用 `debs/` 离线安装，板卡无需外网），
  NetworkManager + nmcli（镜像自带），chrony 3.4（`debs/` 内）
- **PC 侧**：Python 3.10+ `pip install paramiko`，Windows OpenSSH（无 git/plink 也能全流程）

### 一页部署流水（Step1–Step9）

```bat
:: Step1 SSH 引导（新板必做；若握手全 RST = 主机密钥损坏，板上 ssh-keygen -A，详见手册 §2）
python deploy\kbdint_ssh.py
ssh -o BatchMode=yes root@192.168.1.111 "hostname"

:: Step2 离线依赖（chrony 与厂商 ntp 冲突，先卸 ntp）
scp -o BatchMode=yes debs\*.deb root@192.168.1.111:/tmp/
ssh -o BatchMode=yes root@192.168.1.111 "dpkg -i /tmp/libxxhash0.deb /tmp/lighttpd.deb /tmp/lighttpd-mod-openssl.deb && dpkg -r ntp && dpkg -i /tmp/chrony.deb"

:: Step3 阶段一 HTTPS：部署配置 + 自签证书 + 起服务（验收 301 跳转，命令见手册 §4）
scp -o BatchMode=yes config\lighttpd.conf root@192.168.1.111:/etc/lighttpd/
scp -o BatchMode=yes config\10-cgi.conf config\10-ssl.conf root@192.168.1.111:/etc/lighttpd/conf-available/
::   证书/软链/enable 的完整一条命令见 docs/foolproof-deploy.md §4（勿手打旧 Phase1，含 99-unconfigured 坑）

:: Step4 阶段二 CGI：板上编译 13 个 CGI + db_init（-lpthread -ldl，www 资产从 /tmp 显式复制）
tar czf src.tar.gz src
scp -o BatchMode=yes src.tar.gz www\* root@192.168.1.111:/tmp/
scp -o BatchMode=yes deploy\build_on_board.sh root@192.168.1.111:/tmp/
ssh -o BatchMode=yes root@192.168.1.111 "bash /tmp/build_on_board.sh"      :: 验收 DEPLOY-OK

:: Step5 登录流验证（顺带设置正式密码 Testpassword1234@；跳过则 Step7 无法登录）
python deploy\verify_login.py

:: Step6 系统集成（sudoers 白名单×2 / dialout / NTP 监控栈 / 回滚看门狗，一键脚本）
scp -o BatchMode=yes chrony_acl_apply.sh ntp_stats_sample.sh ntp_nic_monitor.c ntp-stats.service ntp-stats.timer ntp-nic-monitor.service rollback_watchdog.sh rollback-recover.service root@192.168.1.111:/tmp/
scp -o BatchMode=yes deploy\integrate_system.sh root@192.168.1.111:/tmp/
ssh -o BatchMode=yes root@192.168.1.111 "bash /tmp/integrate_system.sh"
::   无外网板卡必做（堵死启动事务的 systemd-time-wait-sync）：
ssh -o BatchMode=yes root@192.168.1.111 "systemctl disable --now systemd-time-wait-sync && systemctl mask systemd-time-wait-sync"

:: Step7 功能验证 12/12（验收 12/12 steps passed -> PASS）
python deploy\verify_functions.py

:: Step8 重启复测（md5 基线 → reboot → ~1 分钟 SSH 自恢复 → 5 服务自启 → 复验；myapp.db 变更属预期）
ssh -o BatchMode=yes root@192.168.1.111 "md5sum /etc/ssh/ssh_host_ed25519_key /etc/lighttpd/lighttpd.conf /var/db/myapp.db /home/www/cgi-bin/main.cgi /usr/local/bin/ntp_nic_monitor /root/.ssh/authorized_keys > /var/db/md5_baseline.txt; sync; reboot"
::   等 1 分钟后：
ssh -o BatchMode=yes root@192.168.1.111 "systemctl is-active lighttpd chrony ntp-nic-monitor ntp-stats.timer; md5sum -c /var/db/md5_baseline.txt"
python deploy\verify_functions.py

:: Step9 授时链路（可选，需 UT986 接线：GPIO3_A5=pin11、UART7 M1 TX/RX=pin35/37、共地、天线锁星）
scp -o BatchMode=yes deploy\enable_uart7_overlay.sh root@192.168.1.111:/tmp/
ssh -o BatchMode=yes root@192.168.1.111 "bash /tmp/enable_uart7_overlay.sh && sync && (sleep 2; reboot)"
::   等 1 分钟：ls -l /dev/ttyS7  应为 crw-rw---- root dialout
scp -o BatchMode=yes pps_tod\pps_tod.c pps_tod\pps_tod_watchdog.sh pps_tod\pps_tod_watchdog.conf pps_tod\pps_tod_watchdog.service pps_tod\pps_tod_rtc_save.sh pps_tod\pps_tod_rtc.service pps_tod\pps_tod_rtc.timer pps_tod\sanitize-drift.sh pps_tod\sanitize-drift.service pps_tod\chrony-restart.conf root@192.168.1.111:/tmp/
scp -o BatchMode=yes deploy\install_timing_stack.sh deploy\fix_crlf_and_start.sh root@192.168.1.111:/tmp/
ssh -o BatchMode=yes root@192.168.1.111 "bash /tmp/install_timing_stack.sh && bash /tmp/fix_crlf_and_start.sh"
::   接好 UT986 约 2 分钟后验收：
ssh -o BatchMode=yes root@192.168.1.111 "cat /run/pps_tod/status; chronyc tracking; chronyc sources; date"
::   期望：good=1；Reference ID 50505300 (PPS)；date 为真实时间
```

### 常见坑速查（完整表见 docs/foolproof-deploy.md §11）

| 现象 | 一句话处理 |
|---|---|
| SSH 握手全被 RST | 板上 `ssh-keygen -A`（主机密钥损坏，auth.log 定案） |
| 服务全超时/启动卡死 | `systemctl mask systemd-time-wait-sync`（无外网必踩） |
| chrony.deb 拒装 | 先 `dpkg -r ntp` |
| 跳转丢主机名 / cipher 解析失败 | 本仓库 `config/` 已修（301 捕获组 + 单行 cipher） |
| systemd `Failed at step EXEC ... No such file` | CRLF 行尾 → `fix_crlf_and_start.sh` |
| chrony `Not synchronised` | 无时间源非故障：接 UT986（Step9）或外网 NTP |
| 面板授时 tab 空白 | pps_tod 未部署 → 完成部署包 Step9 |

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

板载时基由**独立的授时子系统**提供（源码已收编入仓库 [`pps_tod/`](pps_tod/README.md)，
一键部署脚本 [`deploy/install_timing_stack.sh`](deploy/install_timing_stack.sh)），
Web 侧只做只读展示与受限控制：

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
- 方案细节与验证记录见 `docs/gps-pps-time-sync.md`、`pps_tod/README.md` 及授时子系统交付文档
  （`PPS_TOD_chrony-会话交付总结.md`、`鲁班猫2N-PPS-TOD-chrony-实施记录.md`）；
  串口/PPS 的物理接线与设备树启用（uart7-m1 overlay）见 `docs/timing-wiring-2026-09-20/`
  与 `docs/headless-deploy-guide.md` §3.7。

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

逐网口 NTP 流量监控测试（需板子两条网口各有真实对端设备，见 `docs/ntp-nic-test-report-2026-09-17.md`）：

```bash
./test_ntp_nic.sh                 # 自动选传输层；本地不可达时经 Windows 转发
./test_ntp_nic.sh --quick         # 跳过 70 秒静默基线观察
```

> 计数规则挂在 INPUT 链，所以只有从外部经该物理网口进来的 UDP/123 才计入该口——
> 脚本先用物理收包计数做归属探针，不成立即终止；再逐层断言
> iptables → CSV → `ntpmon.cgi` JSON 的数值一致性。脚本只做注入与断言，
> 不改网口配置、不重启 chrony、不动 ACL 规则。

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
| NTP 访问控制 | ntpmon.cgi 改 ACL 限 root（角色门）+ CSRF；CIDR 在 CGI 与 root 助手内双重校验，助手经 sudoers NOPASSWD 白名单调用（步骤 3c）；新增走 chronyc 运行时 ACL（即时生效），删除重启 chrony 重载；全部入审计。**Web 是唯一受管路径**：console 上手工 `chronyc allow/deny` 的规则不持久、不入审计、Web 表格看不到也删不掉，重启即失——边界与清理方法见 `docs/ntp-monitor.md` §2.1 |
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
