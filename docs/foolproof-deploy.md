# 傻瓜式部署手册：embeded-web on 鲁班猫 2N（无 HDMI / 无外网）

> 目标：照着粘贴命令就能完成全部部署——不依赖记忆里"README 提过但没写清楚"的东西。
> 来源：2026-09-20/21 两次真实部署实测（12/12 功能验证 + 授时链路 µs 级锁定）。
> 所有本地脚本与依赖都收在本仓库，缺什么本手册都列了精确来源与下载地址。

---

## 0. 总览：你需要准备什么

### 0.1 网络与硬件前提

| 项 | 要求 |
|---|---|
| 板卡上电 | HDMI 可以是坏的（本手册就是无 HDMI 验证），**SSH 是唯一交互通道** |
| 网络 | PC 与板卡**同一局域网**（本例 192.168.1.x，PC 静态 IP 192.168.1.121，板卡 eth0=192.168.1.111）。板卡**不需要外网** |
| UT986 接收机（授时链路可选） | PPS→40pin GPIO3_A5（pin 101），TOD→40pin UART7 M1（TX=pin35 / RX=pin37），共地，天线窗外锁星 |
| 真实授时前提醒 | 板卡无 RTC 电池；不接 UT986 也不通外网时，时钟会停在出厂假时间（2019-02-14） |

### 0.2 PC 侧软件（缺任何一个先装）

- Python 3.10+，`pip install paramiko`（SSH 引导用；**本机没有 git/plink 也能全流程**）
- Windows 自带 OpenSSH（`ssh`/`scp`/`ssh-keygen`）+ `curl`（下载 deb）
- WSL 不是必需品（mirror 模式连板卡局域网有已知缺陷，全部走 Windows 侧）

### 0.3 本仓库文件清单（部署所需，一个不缺）

```
debs\                                        ← 4 个离线 deb（板卡无外网，PC 下载）
  libxxhash0.deb / lighttpd.deb / lighttpd-mod-openssl.deb / chrony.deb
deploy\
  kbdint_ssh.py                              ← Step1 SSH 引导（装公钥免密）
  build_on_board.sh                          ← Step3 阶段二：板上编译+部署 CGI
  integrate_system.sh                        ← Step5 系统集成（sudoers/timer/守护）
  enable_uart7_overlay.sh                    ← Step8a 启用 ttyS7
  install_timing_stack.sh                    ← Step8b 授时栈部署
  fix_crlf_and_start.sh                      ← Step8b 修 CRLF 行尾并启动
  verify_login.py                            ← Step4 登录全流程验证
  verify_functions.py                        ← Step6 功能 12 步验证
PPS_TOD 源码（已收进本仓库 `pps_tod\`，10 个文件）
  pps_tod.c + watchdog.sh/.conf/.service + rtc_save.sh/.service/.timer
  + sanitize-drift.sh/.service + chrony-restart.conf
src\ + www\ + config\                        ← 项目源码 / 前端 / lighttpd 配置（仓库自带）
```

⚠️ `PPS_TOD` 授时栈源码**已收进本仓库 `pps_tod\`**（2026-09-21 入库；此前只存在于
PC 的 `C:\Users\YAO\Desktop\codex_store\PPS_TOD\`，是第一次部署最容易卡的点）。
主源码 `pps_tod.c` 为修复后最终版，本仓库 LF 版 md5 = `3f25f24c553094e04f17b01d9bc60a87`
（历史 CRLF 原版 md5 `58e2bdc9d1d7fe35a198e0ed4e7f5987`，功能相同；目录里另有
`pps_tod.c.orig-20260917` 是修复前备份，**别拿错**）。

### 0.4 离线 deb 的精确下载地址（archive.debian.org，PC 执行）

**4 个 deb 已随仓库附带在 `debs\`，可直接用**；需要重新下载时用以下地址：

```
https://archive.debian.org/debian/pool/main/x/xxhash/libxxhash0_0.8.0-2~bpo10+1_arm64.deb
https://archive.debian.org/debian/pool/main/l/lighttpd/lighttpd_1.4.59-1~bpo10+1_arm64.deb
https://archive.debian.org/debian/pool/main/l/lighttpd/lighttpd-mod-openssl_1.4.59-1~bpo10+1_arm64.deb
https://archive.debian.org/debian/pool/main/c/chrony/chrony_3.4-4+deb10u2_arm64.deb
```

Buster 已进归档区，`deb.debian.org` 上没有 buster-backports，别找错镜像。

---

## 1. 板卡凭据与关键参数速查卡

| 用途 | 值 |
|---|---|
| 板卡 SSH | `root@192.168.1.111`，密码 `root` |
| Web 面板 | `https://192.168.1.111`（自签证书，浏览器/curl 加 `-k`） |
| 面板管理员（最终） | `root` / `Testpassword1234@` |
| 面板初始密码（首登强制改密，用完即失效） | `root` / `admin` |
| 板上关键路径 | `/home/www`（前端+`cgi-bin/`）、`/var/db/myapp.db`、`/etc/lighttpd/`、`/usr/local/bin/`、`/run/pps_tod/`、`/var/log/pps_tod/` |
| 授时串口 | `/dev/ttyS7`（uart7-m1 overlay），PPS=`/dev/gpiochip3` line5 |
| ssh 复用选项 | `-o BatchMode=yes`（公钥装好后全程免密） |

---

## 2. Step1：SSH 引导（第一次必须做）

> ⚠️ 新镜像的板卡如果 SSH 握手全部被 RST/超时，九成是**主机密钥文件损坏**
> （auth.log 会报 `invalid format` + `No supported key exchange algorithms [preauth]`）。
> 先在板卡 shell（人工或串口）跑 `ssh-keygen -A && mkdir -p /run/sshd && sshd -t`，再回来。

```bat
:: PC 上，工作区根目录
python deploy\kbdint_ssh.py        :: 板卡密码 root；脚本自动装本机公钥免密
ssh -o BatchMode=yes root@192.168.1.111 "hostname"    :: 应输出 lubancat
```

预期：`[+] password auth OK` → `[+] key install rc=0`。
失败对照：`Authentication failed` = 密码不对/PermitRootLogin；`Connection reset` = 主机密钥损坏（见上）。

## 3. Step2：离线依赖安装

```bat
scp -o BatchMode=yes debs\*.deb root@192.168.1.111:/tmp/
ssh -o BatchMode=yes root@192.168.1.111 "dpkg -i /tmp/libxxhash0.deb /tmp/lighttpd.deb /tmp/lighttpd-mod-openssl.deb"
```

**坑①：chrony 与厂商自带 ntp 冲突**。`dpkg -i chrony.deb` 会报
`chrony conflicts with ntp`，必须先卸 ntp 再装：

```bat
ssh -o BatchMode=yes root@192.168.1.111 "dpkg -r ntp && dpkg -i /tmp/chrony.deb && systemctl is-active chrony"
```

（项目 ACL 机制全部基于 chrony 3.4，不许保留 ntp。）

## 4. Step2：阶段一 HTTPS（lighttpd + 证书）

**上游仓库 3 个坑先修（已修进本仓库 `embeded-web-main\config\`）**：

1. `10-ssl.conf` cipher 列表是 4 段跨行字符串拼接——lighttpd **不支持**，解析直接失败；
2. `10-ssl.conf` HTTP→HTTPS 重定向 host 正则 `.*` 无捕获组，`Location: https:///` 丢主机名，浏览器跳转失败；
3. 主配置 `lighttpd.conf` 会 include `conf-enabled/99-unconfigured.conf`（deb 首装的占位配置），要删掉软链。

```bat
cd embeded-web-main
scp -o BatchMode=yes config\lighttpd.conf root@192.168.1.111:/etc/lighttpd/
scp -o BatchMode=yes config\10-cgi.conf config\10-ssl.conf root@192.168.1.111:/etc/lighttpd/conf-available/
ssh -o BatchMode=yes root@192.168.1.111 "rm -f /etc/lighttpd/conf-enabled/99-unconfigured.conf && ln -sf /etc/lighttpd/conf-available/10-cgi.conf /etc/lighttpd/conf-enabled/ && ln -sf /etc/lighttpd/conf-available/10-ssl.conf /etc/lighttpd/conf-enabled/ && mkdir -p /home/www/cgi-bin /var/cache/lighttpd/uploads /var/log/lighttpd && chown -R www-data:www-data /home/www /var/cache/lighttpd /var/log/lighttpd && openssl req -x509 -newkey rsa:2048 -keyout /etc/ssl/private/server.key -out /etc/ssl/certs/server.crt -days 3650 -nodes -subj '/CN=lubancat.local' 2>/dev/null && cat /etc/ssl/certs/server.crt /etc/ssl/private/server.key > /etc/lighttpd/server.pem && chmod 600 /etc/lighttpd/server.pem && lighttpd -tt -f /etc/lighttpd/lighttpd.conf && systemctl enable --now lighttpd"
```

**验收**（PC 上）：

```bat
curl -kI --max-time 8 http://192.168.1.111/      ← 应 301，Location: https://192.168.1.111/（主机名不能丢）
curl -k  --max-time 8 -o NUL -w "%%{http_code}" https://192.168.1.111/   ← 404 是正常的（前端文件还没部署）
```

## 5. Step3：阶段二——板上编译 13 个 CGI

**README 的坑**：仓库没有 Makefile，构建命令藏在 README 的内联循环里；
且 README 的 `cp control_panel.html ...` 在 `cd /tmp/src` 之后按相对路径执行必然失败
（www 资产 scp 到的是 `/tmp/`）。**不要手打 README 那段，用本仓库脚本**：

```bat
:: 在仓库根目录（git clone 下来的 embeded-web）
tar czf src.tar.gz src
scp -o BatchMode=yes src.tar.gz www\* root@192.168.1.111:/tmp/
scp -o BatchMode=yes deploy\build_on_board.sh root@192.168.1.111:/tmp/
ssh -o BatchMode=yes root@192.168.1.111 "bash /tmp/build_on_board.sh"
```

脚本内容＝README 的编译循环 + 链接**必须 `-lpthread -ldl`**（漏了必炸）＋
从 `/tmp` 显式复制 www 资产 + `db_init admin` 初始化 `/var/db/myapp.db` + 属主 www-data。
**验收**：脚本尾部 `DEPLOY-OK` + 13 个 `.cgi` 列表。

> ⚠️ 板上编译 sqlite3.c 要 1–3 分钟；tar 的 "time stamp in the future" 警告是因为板卡时钟
> 在假时间（2019），无害。

## 6. Step4：登录流验证（顺带设置正式密码）

```bat
python deploy\verify_login.py
```

流程自动完成：root/admin 首登 → 302 强制跳 `/change.html` → 改密为
`Testpassword1234@` → 重登录 → 控制面板（74KB）打开 → 输出 `[RESULT] PASS`。
**这是把正式密码写进数据库的一步，跳过它后续 verify_functions 会登录失败。**

## 7. Step5：系统集成（sudoers / NTP 监控 / 回滚看门狗）

```bat
cd embeded-web-main
scp -o BatchMode=yes chrony_acl_apply.sh ntp_stats_sample.sh ntp_nic_monitor.c ntp-stats.service ntp-stats.timer ntp-nic-monitor.service rollback_watchdog.sh rollback-recover.service root@192.168.1.111:/tmp/
cd ..
scp -o BatchMode=yes deploy\integrate_system.sh root@192.168.1.111:/tmp/
ssh -o BatchMode=yes root@192.168.1.111 "bash /tmp/integrate_system.sh"
```

**无外网板卡必踩的坑**（脚本会被堵住几分钟不动）：
`systemd-time-wait-sync.service` 在无外网、时钟永远无法同步的板上无限等待，
把整个 systemd 启动事务堵死（`systemctl list-jobs` 里全是 waiting）。
**处理**（务必在第一次重启前做）：

```bat
ssh -o BatchMode=yes root@192.168.1.111 "systemctl disable --now systemd-time-wait-sync && systemctl mask systemd-time-wait-sync && systemctl list-jobs"
```

**验收**：`INTEGRATION-OK`；lighttpd/chrony/ntp-nic-monitor/ntp-stats.timer 全 active；
两个 sudoers 文件 `parsed OK`。

## 8. Step6：功能全流程验证（12 步）

```bat
python deploy\verify_functions.py > docs\evidence_functions.txt
type docs\evidence_functions.txt      ← 12/12 steps passed -> PASS
```

**自己手测时的两个坑**（脚本里已避开）：

- `user_create.cgi` 的密码**必须 ≥10 位且含大小写+数字+符号**（策略后端强制，9 位直接拒）；
- 删用户用 `user_id`（从 user_list 的 JSON 里拿），不是 `username`——报 `Missing user_id` 就是字段写错。

## 9. Step7：重启复测（无 HDMI 自恢复证据）

```bat
:: 先存 md5 基线（板卡上，8 个关键文件）
ssh -o BatchMode=yes root@192.168.1.111 "md5sum /etc/ssh/ssh_host_ed25519_key /etc/lighttpd/lighttpd.conf /etc/lighttpd/conf-available/10-ssl.conf /var/db/myapp.db /home/www/cgi-bin/main.cgi /usr/local/bin/ntp_nic_monitor /usr/local/bin/chrony_acl_apply.sh /root/.ssh/authorized_keys > /var/db/md5_baseline.txt; sync; reboot"
:: 等 1–2 分钟，板卡起来后
ssh -o BatchMode=yes root@192.168.1.111 "systemctl is-active lighttpd chrony ntp-nic-monitor ntp-stats.timer; systemctl list-jobs; md5sum -c /var/db/md5_baseline.txt"
python deploy\verify_functions.py
```

判定：SSH ~1 分钟自动恢复、5 服务全自启、`No jobs running`；
**md5 里 `myapp.db` FAILED 是预期**（运行期写入），其余必须全 OK——其余不 OK＝存储损坏，立即停下排查。

## 10. Step8：授时链路（ttyS7 + PPS + chrony，可选）

### 10.1 启用 ttyS7（零重编，野火 overlay 机制）

> 大坑澄清：**不需要**反编译/重编 dtb。镜像自带 `/boot/uEnv/uEnv.txt` + `/boot/dtb/overlay/*.dtbo`。

```bat
scp -o BatchMode=yes deploy\enable_uart7_overlay.sh root@192.168.1.111:/tmp/
ssh -o BatchMode=yes root@192.168.1.111 "bash /tmp/enable_uart7_overlay.sh"
ssh -o BatchMode=yes root@192.168.1.111 "sync; (sleep 2; reboot)"
:: 等 1 分钟后
ssh -o BatchMode=yes root@192.168.1.111 "ls -l /dev/ttyS7"   ← crw-rw---- root dialout = 成功
```

（板上留有备份 `uEnv.txt.bak-before-uart7`，失败可还原。）

### 10.2 部署 pps_tod 授时栈（源码在本仓库 `pps_tod\`，10 个文件）

**先核源码 md5**（防拿错 `.orig-20260917` 修复前备份；仓库 LF 版与历史 CRLF 版功能相同）：

```bat
certutil -hashfile pps_tod\pps_tod.c MD5
:: 期望 3f25f24c553094e04f17b01d9bc60a87（本仓库 LF 版）
:: 历史 CRLF 原版为 58e2bdc9d1d7fe35a198e0ed4e7f5987，二者功能相同
```

```bat
:: 在仓库根目录
scp -o BatchMode=yes pps_tod\pps_tod.c pps_tod\pps_tod_watchdog.sh pps_tod\pps_tod_watchdog.conf pps_tod\pps_tod_watchdog.service pps_tod\pps_tod_rtc_save.sh pps_tod\pps_tod_rtc.service pps_tod\pps_tod_rtc.timer pps_tod\sanitize-drift.sh pps_tod\sanitize-drift.service pps_tod\chrony-restart.conf root@192.168.1.111:/tmp/
scp -o BatchMode=yes deploy\install_timing_stack.sh root@192.168.1.111:/tmp/
ssh -o BatchMode=yes root@192.168.1.111 "bash /tmp/install_timing_stack.sh"
ssh -o BatchMode=yes root@192.168.1.111 "bash /tmp/fix_crlf_and_start.sh"
```

**默认值坑**：`pps_tod.c` 串口默认是 `/dev/ttyS3`（137.100 旧板接线）——
本测试板必须显式 `-t /dev/ttyS7`（已在 install_timing_stack.sh 的 service 单元里写死）。
GPIO 默认 `chip3 line5`＝GPIO3_A5，与接线一致，`-g` 可不传。

**两个工具链坑（脚本已内置修复；仓库源码已统一 LF 行尾，正常情况下 fix 步骤只是兜底）**：

- **CRLF 行尾**：源文件若被 Windows 编辑过，systemd spawn 报
  `Failed at step EXEC ... No such file or directory`＝shebang 变 `/bin/sh\r`。
  `fix_crlf_and_start.sh` 统一 `sed 's/\r$//'` 清洗；
- **`gcc 2>&1 | head -5` 吞退出码**：head 提前关管道 gcc 被 SIGPIPE 杀死，二进制不落地。
  编译一律不接管道，失败要看见。

### 10.3 验收（接好 UT986 后约 2 分钟内）

```bat
ssh -o BatchMode=yes root@192.168.1.111 "cat /run/pps_tod/status; chronyc tracking; chronyc sources; date"
```

| 指标 | 期望值 |
|---|---|
| `/run/pps_tod/status` | `good=1`，`offset_us` 在 ±数百内，`reset_state=OK` |
| `chronyc sources` | `#* PPS` 入选（#* = 当前源；`#?` = 还没收满样本，等 1–2 分钟） |
| `chronyc tracking` | Reference ID `50505300 (PPS)`，System time 在 µs 级 |
| `date` | **真实当前时间**（首次会从 2019 一步跳正，日志有 coarse step） |

不接硬件时：`good=0`/sources `#?` 不可达，属预期，不算故障。

## 11. 踩坑速查总表（按现象查）

| 现象 | 根因 | 修复/预防 |
|---|---|---|
| SSH 握手全部 RST（端口开着、banner 正常） | 板卡 SSH 主机密钥文件损坏（跨断电复现；auth.log 定案） | 板上 `ssh-keygen -A` 重新生成；排除网络前先看 auth.log |
| systemd 事务全 waiting、服务超时、`/run/sshd` 缺失 | `systemd-time-wait-sync` 无外网永远等时钟 | disable+mask，重启前必做 |
| `dpkg -i chrony.deb` 拒装 | 与厂商镜像 ntp 4.2.8 冲突 | 先 `dpkg -r ntp` |
| lighttpd 配置解析报 cipher 行 | 上游 `10-ssl.conf` 跨行字符串拼接不被支持 | 合并单行（本仓库已修） |
| HTTP 跳转 `Location: https:///` | host 正则 `.*` 无捕获组 | 改 `(.*)`（本仓库已修） |
| 阶段二 www 资产复制失败 | README 内联脚本相对路径错误 | 用 `build_on_board.sh`（从 /tmp 显式复制） |
| 用户创建报"密码长度需 10-64" | 临时密码给了 9 位 | ≥10 位含大小写数字符号 |
| 删用户报 `Missing user_id` | delete 端点要 `user_id` 不是 `username` | 从 user_list JSON 取 id |
| cmd 里长命令"语法不正确"/转义坏 | cmd 对长引号串的包装 | **一律脚本落文件 scp 上板执行，不拼长命令** |
| systemd 报 `Failed at step EXEC ... No such file or directory`（文件明明在） | Windows CRLF 行尾，shebang 变 `/bin/sh\r` | sed 清洗行尾（fix_crlf_and_start.sh） |
| systemd 203/EXEC 但文件"明明编出来了" | `gcc | head` 管道：head 关管道→gcc 被 SIGPIPE 杀；退出码被管道吞 | 编译不接管道，或用 `set -o pipefail` |
| chrony `Not synchronised` / sources=0 | 无时间源（非部署失败） | 接 UT986 或外网 NTP；监控栈正常即可确认部署无错 |
| 面板授时 tab 空白 | pps_tod 未部署，`/run/pps_tod/*` 不存在 | 完成第 10 节 |
| 授时栈起来但 `#?` 不收敛 | 接收机未锁星/接线错/波特率不对 | 窗边锁星；TOD 接 RX 脚（TX 接 TX 不出数）；115200 8N1 |

## 12. 一页流水（全部命令的执行顺序）

```
Step1 引导SSH      python deploy\kbdint_ssh.py
Step2 依赖         scp debs\*.deb → dpkg -r ntp → dpkg -i ×4
Step3 HTTPS        scp config → 证书 → enable lighttpd → curl 验收
Step4 CGI          tar src → scp src.tar.gz + www/* + build_on_board.sh → bash
Step5 验证登录     python deploy\verify_login.py（设置 Testpassword1234@）
Step6 系统集成     scp 8 个文件 + integrate_system.sh → bash → mask time-wait-sync
Step7 功能验证     python deploy\verify_functions.py（12/12 PASS）
Step8 重启复测     md5 基线 → reboot → 5 服务自启 + mdsum -c + 功能 12 步再来一遍
Step9 授时链路     enable_uart7_overlay.sh → reboot → install_timing_stack.sh
                   → fix_crlf_and_start.sh → /run/pps_tod good=1 → chronyc #* PPS
```
