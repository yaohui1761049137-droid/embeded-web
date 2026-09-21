# 无 HDMI / 无外网板卡部署与排障指南（embeded-web on LubanCat 2N）

> 来源：2026-09-20「无 HDMI 鲁班猫 2N 部署实测」会话（Windows 侧，工作区
> `C:\Users\YAO\Desktop\codex_store\without_HDMI_test\`，WSL 侧路径
> `/mnt/c/Users/YAO/Desktop/codex_store/without_HDMI_test/`）。
> 该次部署从一块 HDMI 物理损毁、无串口、无外网、镜像全新的板卡起步，最终全流程验证通过（12/12），
> 并在断电重启后自动恢复。本文把那次踩过的坑整理成可复用的部署手册：**每节按"现象 → 定案 → 处理 → 预防"组织**。

---

## 1. 适用场景与环境

| 项 | 本次实测值 |
|---|---|
| 板卡 | 鲁班猫 2N（RK3568），镜像 `lubancat-rk3568-debian10-xfce-20250711`，Debian 10.13 / 内核 4.19.232 aarch64 / 2GB 内存 / 7GB rootfs |
| 网络孤立局域网 192.168.1.0/24 | **无外网**；板卡 eth0 = `192.168.1.111`，网关 192.168.1.1 |
| 显示/交互 | HDMI 物理损毁，**无串口接线，SSH 是唯一通道** |
| PC 侧 | Windows 11 + Python 3.10 + paramiko + Windows OpenSSH（**无 git/plink**） |
| 板卡基线（部署前） | gcc 8.3.0 ✅ make 4.2.1 ✅ nmcli 1.14.6 ✅ www-data ✅；缺 lighttpd/chrony；80/443 空闲；磁盘余 3.9G |

**判读基线**：镜像自带 gcc/make/nmcli，可直接板上编译；缺的 4 个依赖用离线 deb 解决（§2 步骤 1）。

## 2. 部署流程（五步，可复跑）

全部脚本与依赖包在 Windows 工作区 `without_HDMI_test\` 下：

```
debs/                      4 个 arm64 离线 deb（PC 从 archive.debian.org 下载后带来）
deploy/kbdint_ssh.py       SSH 引导：keyboard-interactive 认证 + 安装本机公钥（免密）
deploy/build_on_board.sh   阶段二：板上编译 13 个 CGI + db_init + www 资产部署
deploy/integrate_system.sh 系统集成：sudoers/dialout/定时器/守护进程/rollback 服务
deploy/verify_login.py     登录全流程自动化验证（首登强制改密）
deploy/verify_functions.py 功能全流程 12 步自动化验证
docs/evidence_*.txt        部署后/重启后功能证据 + 最终状态证据
docs/diagnosis/            18 个 SSH 排障探针脚本（见 §3.1）
```

1. **依赖安装**（板卡无外网，PC 下载后 scp 上板 `dpkg -i`）：
   `lighttpd 1.4.59-1~bpo10+1`、`lighttpd-mod-openssl`、`libxxhash0 0.8.0-2~bpo10+1`、
   `chrony 3.4-4+deb10u2`。注意 **chrony 会替换厂商镜像自带的 ntp 4.2.8**（项目 ACL 机制基于 chrony）。
2. **SSH 引导**：`python deploy/kbdint_ssh.py`（板密码 root）→ 装好 `id_ed25519` 免密。
   镜像的 sshd 可能拒绝 `password` 方式但允许 `keyboard-interactive`，脚本已处理。
3. **阶段一（HTTPS）**：部署 `config/lighttpd.conf` 与 `10-cgi/10-ssl.conf`；自签证书
   （CN=lubancat.local，10 年，合并 server.pem）；`systemctl enable --now lighttpd`；验证 80→443 301。
4. **阶段二（CGI）**：`src.tar.gz` 上板解压到 `/tmp/src`，`bash deploy/build_on_board.sh`。
   编译命令必须带 **`-lpthread -ldl`**（sqlite3.o 需要，漏了必炸）；产物部署 `/home/www`（属主 www-data）。
5. **系统集成 + 验证**：`bash /tmp/integrate_system.sh`（sudoers 两个白名单 `99-www-nmcli`/`99-www-ntpacl`
   均经 `visudo -cf` 校验、www-data 入 dialout 组、`ntp-stats.timer`、`ntp_nic_monitor`、
   `rollback-recover.service`）；然后 `python deploy/verify_login.py` → `python deploy/verify_functions.py`。

## 3. 排障手册（按现象查）

### 3.1 SSH 握手全部被 RST（连接阶段就失败）

- **现象**：所有 SSH 连接在握手阶段被 RST/超时，无论 paramiko 还是 Windows OpenSSH；端口 22 是开的。
- **排查顺序（重要）**：先用裸 socket 探针排 TCP 问题（`docs/diagnosis/` 里有 banner_grab、kex_probe、
  halfclose_kex、tcp_size_probe 等 18 个），**最后才在板卡 `/var/log/auth.log` 定案**。
- **根因（auth.log 铁证）**：每个连接都报
  `Error loading host key "…": invalid format` + `fatal: No supported key exchange algorithms [preauth]`
  —— 板卡三类 SSH 主机密钥（RSA/ECDSA/ed25519）**文件内容损坏**（跨断电复现；
  同时 eMMC 上有 4 个 libmali GPU 库被截断，ldconfig 可见）。损坏发生在断电/初次上电阶段。
- **处理**：板卡上 `ssh-keygen -A` 重新生成全部主机密钥，重启后复测未再损坏。
- **教训**：握手失败别只盯网络/防火墙，先看服务器端 auth.log；裸 socket 探针只能证明"连接层是好的"。

### 3.2 SSH 能连上但服务全部超时/启动卡死（无外网板卡必踩）

- **现象**：开机后 systemd 事务永远不完（`systemctl list-jobs` 里 multi-user.target、timers.target 全部
  waiting），首启时 `/run/sshd` 都没建；几乎所有服务超时。
- **根因**：`systemd-time-wait-sync.service` 在**无外网、时钟永远无法同步**的板卡上无限等待
  （时钟停在 2019-02-14，无 RTC 电池），堵死整个启动事务。
- **处理**：`systemctl disable --now systemd-time-wait-sync && systemctl mask systemd-time-wait-sync`，
  重启验证 `systemctl list-jobs` → `No jobs running`。
- **预防**：**任何无外网/无 RTC 的板卡部署完先 mask 它再重启**，否则第一次断电重启就复现。

### 3.3 HTTPS 打不开 / 80→443 跳转失败（上游仓库 3 个 bug）

本次部署实测发现并已改本地副本，重新部署前自查这三处：

1. `config/10-ssl.conf`：lighttpd **不支持跨行字符串拼接**，cipher 列表写成 4 段续行字符串导致解析失败 → 合并单行。
2. `config/lighttpd.conf`：HTTP→HTTPS 重定向 host 正则 `.*` 无捕获组 → `Location: https:///`（主机名丢失）→ 改 `(.*)`。
3. README 阶段二脚本 `cd /tmp/src` 后按相对路径复制 www 资产会失败 → 构建脚本从 `/tmp` 显式复制（见 `deploy/build_on_board.sh`）。

### 3.4 CGI 编译/运行问题速查

- 链接必须 `-lpthread -ldl`（sqlite3.o 需要）——漏了会报未定义引用。
- 13 个 CGI + `db_init` 全部要链接公共目标：`common.c auth.c gate.c users.c nmcli.c timesync.c ntpmon.c sha256.c sqlite3.o`。
- `db_init admin` 初始化 `/var/db/myapp.db`，属主必须是 www-data（`chown -R www-data:www-data /var/db`）。

### 3.5 Windows PC 侧陷阱

- **复杂命令直接贴 cmd 会被转义搞坏**（本次实测"命令语法不正确"、内联长命令报"系统找不到指定的路径"）→
  复杂操作写成 `.sh`/`.py` 文件上板执行，**不要拼超长一行命令**。本次部署全程走"脚本落文件"路线：
  `build_on_board.sh` / `integrate_system.sh` 上板执行，SSH 引导用 `kbdint_ssh.py`，板上探测用
  `_pps_tod_check*.sh`。
- **跨网段访问走 Windows 侧包装**：WSL mirror 模式对本局域网回包有已知缺陷（实测连板卡 TCP 22 都建不起来），
  WSL 不直达板卡网段时，由 Windows PowerShell 脚本包装 ssh/scp（本会话此前用的 `bscp.ps1`/`bsh.sh` 包装
  即此模式），WSL 侧只负责文件与产物整理。
- **中文乱码**：Windows 控制台/部分工具按 GBK 显示 UTF-8 文件，面板 JS 里的中文会显示成
  `鍒涘缓涓?` 一类乱码——这是**查看端编码问题，不是文件损坏**，用 VS Code/WSL 工具链核实后再下结论。
- 本机没有 git/plink 时，全部传输/执行用 **paramiko**（scp 与 exec 两类封装都在 deploy 脚本里）。
- SSH 认证被拒时先试 `keyboard-interactive`（见 `deploy/kbdint_ssh.py` 的 6 次重试逻辑）。

### 3.6 chrony 显示 Not synchronised / Stratum 0 —— 先分清"部署失败"还是"没有时间源"

该会话实测结论（可直接引用）：

- **正常的部分**：`chronyd` active、`ntp_stats.timer` 每分钟采样（`/var/db/ntp_stats.csv` 持续写入）、
  `ntp_nic_monitor` 每秒更新 `/var/db/ntp_nic.json`、面板能读 `/etc/chrony/acl-web.conf` 的 ACL、
  eth0–3 的 iptables NTP 计数规则就位。
- **不正常的部分**：`chronyc tracking` → Not synchronised / Stratum 0 / 参考时间 1970；
  `chronyc sources` → sources=0。原因：**测试台架上没有任何时间源**——无外网 NTP，也没接 UT986
  GNSS 接收机，厂商镜像的 `pps_tod` 守护进程也没在跑（`/dev/pps*`、`/dev/gnss*` 不存在）。
- **结论**：这是**预期现象，不是部署失败**。时钟不同步不影响部署验证结论，但有两个连带影响：
  日志时间戳是 2019 年、90 天密码有效期计时跑在假时钟上。要测真实授时，接 UT986（PPS+TOD）或通外网配 NTP 上游。

### 3.7 授时链路硬件未配置：/dev/ttyS7 与 PPS 需要设备树 / 内核 / 用户态三层补齐

**现象**：面板"时间服务校验设置"tab 空白——它指向的 `/run/pps_tod/*` 状态文件不存在，
因为授时守护进程没在跑；板上查 `/dev/ttyS7`、`/dev/pps*` 全部没有。

该测试板（192.168.1.111，厂商镜像 `20250711`）实测现状（与本仓库 192.168.137.100 那块已接 UT986 的板对比）：

| 检查项 | 测试板实测结果 |
|---|---|
| `/dev/ttyS7` | ❌ 不存在 |
| 设备树串口节点 | ❌ uart0–9 **全部未启用**；内核控制台走 `console=ttyFIQ0`（FIQ 调试器） |
| 内核串口驱动记录 | ❌ dmesg 除 ch341-uart（USB 串口）注册外无任何 uart 探测 |
| `/dev/pps*` | ❌ 不存在 |
| **GPIO3_A5（pin 101）** | ⚠️ 引脚完好但**完全空闲**：`pin 101 (gpio3-5): (MUX UNCLAIMED) (GPIO UNCLAIMED)` |
| GPIO3 控制器 | ✅ 正常（fe760000.gpio，gpiochip3 base 96；GPIO3_A5=101 无误） |
| 内核 PPS 支持 | ❌ `/lib/modules` 无任何 pps 模块（无 `CONFIG_PPS_CLIENT_GPIO` 产物） |
| `pps_tod` 守护 | ❌ 镜像里没有（无 service、无 init.d、无 `/var/log/pps_tod`） |

**判读**：不是板卡坏了——HDMI 损毁没有波及串口/GPIO 子系统，GPIO 侧干净空闲；
是**厂商镜像出厂就没配授时硬件**。embeded-web 原作者是在自己配好设备树的板上开发的
（`docs/gps-pps-time-sync.md` 即那套配置的记录；接线图见 `docs/timing-wiring-2026-09-20/`）。

**打通真实授时链路（UT986 → TOD/PPS → chrony）的三层工作清单**：

1. **设备树层**：**这块镜像有完整的 overlay 机制，不需要反编译/重烧 dtb**（实测确认：
   `/boot/dtb/overlay/` 共 139 个 dtbo；`/boot/uEnv/uEnv.txt` → `uEnvLubanCat2N-V3.txt`
   含 `enable_uboot_overlays=1` 与 `#overlay_start … #overlay_end` 块；`boot.scr` 里
   `dtfile ${fdt_addr_r} ${fdt_over_addr} /uEnv/uEnv.txt …` 即启动时应用 overlay 的逻辑）：
   - **uart7 是预置 overlay**：`#40pin` 区里已写好被注释的一行
     `#dtoverlay=/dtb/overlay/rk356x-lubancat-uart7-m1-overlay.dtbo`（M1 = GPIO3_C4/C5 = pin 35/37，
     与接线图一致）。启用 = 备份 uEnv.txt 后**去掉行首 `#`** → reboot → `ls -l /dev/ttyS7`、
     `dmesg | grep -i fe6a0000`（uart7 控制器 serial@fe6a0000 被探测）。
   - **回滚 = 把 `#` 加回去**，无需 SD 卡重刷；动手前先 `cp /boot/uEnv/uEnvLubanCat2N-V3.txt{,.bak-$(date +%s)}`。
   - **PPS 无现成 overlay**（139 个 dtbo 中无 pps-gpio 节点）——但**路线 1 根本不需要 DT 的 PPS 节点**：
     `pps_tod` 走 `/dev/gpiochip3` 用户态边沿事件，前提已满足。仅路线 2 需要自编 pps-gpio overlay/内核模块。
2. **内核层**（取决于路线，**先定路线再动手**）：
   - **路线 1（推荐，192.168.137.100 板实证）：零内核改动**。`pps_tod` 走 gpiochip 字符设备边沿事件
     （内核时间戳），不需要 `/dev/pps0` 和 `pps_gpio.ko`——GPIO3_A5 目前"UNCLAIMED 空闲态"
     恰好是理想条件，`/dev/gpiochip3` 已确认存在。
   - **路线 2（SOP 方案 A）：需要内核模块**。厂商内核无 pps 驱动产物，须交叉编译 `pps_gpio.ko`
     （或换带 `CONFIG_PPS_CLIENT_GPIO` 的内核），且源码树版本须与板上 4.19.232 一致；
     另有串口 DCD 方案 B（`ldattach`）依赖 8250 驱动的 DCD 中断支持。**选这条前先确认内核源码树/交叉编译环境。**
3. **用户态层**（两条路线共同，即"授时栈"部署）：
   - `pps_tod` 守护进程（独立交付，源码在 `C:\Users\YAO\Desktop\codex_store\PPS_TOD\`，含 watchdog、
     RTC 持久化、实施记录；部署记录见 `docs/feature-implementation-report-2026-09-17.md` §4）：板上编译，
     运行参数 `pps_tod -D -b 115200 -t /dev/ttyS7`，写 SysV SHM(0x4e545030)；
   - 配套 `pps_tod_watchdog`、`pps_tod_rtc.timer`（RTC 每 6h 持久化）、按天日志；
   - chrony 侧：`refclock SHM 0` 配置 + `chronyc allow/deny` ACL（**SIGHUP 是退出信号绝不能发**，
     见 `docs/handoff-2026-09-17.md`）；
   - Web 侧：`timesync.cgi` 只写 ttyS7（www-data 在 dialout 组）；
   - 验证：`/run/pps_tod/status` 出现 `good=1`，`chronyc sources` 出现 `#* PPS`（Reference ID 50505300）。

**现状自检命令**（板上执行，配合 §3.5"脚本落文件"原则写成脚本上板）：

```bash
ls /dev/ttyS* /dev/pps* 2>/dev/null                                   # 串口/PPS 设备
ls /sys/firmware/devicetree/base/ | grep -iE 'uart|pps|gnss'          # DT 节点
for u in /sys/firmware/devicetree/base/uart*; do
  echo "$u: $(cat "$u/status" 2>/dev/null)"
done
grep -oE 'console=[^ ]+' /proc/cmdline                                # 控制台去向（ttyFIQ0）
zcat /proc/config.gz 2>/dev/null | grep -E 'CONFIG_PPS|CONFIG_SERIAL' # 内核 PPS 支持（无 gz 则查模块目录）
ls /lib/modules/$(uname -r)/kernel/drivers/pps/ 2>/dev/null
grep -A2 'pin 101' /sys/kernel/debug/pinctrl/*/pinmux-pins            # GPIO3_A5 认领状态
ls -l /dev/gpiochip3                                                   # 路线 1 前提
```

## 4. 验证清单（部署完必跑）

1. **登录流**（`deploy/verify_login.py`）：admin 首登 → 302 强制跳 `/change.html` → 改密
   `{"status":"ok"}` → 新密码登录 → 控制面板（74KB）打开。最终凭据：root / `Testpassword1234@`。
2. **功能 12 步**（`deploy/verify_functions.py`，部署后 + 重启后各一次）：
   登录 / main 面板 / network（应返回 eth0 实际 IP）/ timesync / ntpmon（ACL 读取）/ log /
   user_list / 增用户 → 删用户（id 闭环）→ 登出。
3. **重启复测**：`reboot` 后约 1 分钟 SSH 自动恢复；5 个服务
   （lighttpd / chrony / ntp-nic-monitor / ntp-stats.timer / rollback-recover）全部自启；
   `systemctl list-jobs` 无残留事务；HTTPS 登录再过 12 步。
4. **md5 基线**：部署完存 `/var/db/md5_baseline.txt`（覆盖 SSH 主机密钥、配置、CGI 二进制、
   authorized_keys 等 8 个关键文件）；重启后比对——**`myapp.db` 出现差异是预期**（运行期正常写入：
   改密时间戳/会话/审计日志），其余必须全 OK。

## 5. 凭据与关键信息速查

| 用途 | 值 |
|---|---|
| 板卡 SSH / 控制台 | root / root（`192.168.1.111`，eth0） |
| Web 面板 | `https://192.168.1.111`（自签证书，`-k` 忽略告警）；root / `Testpassword1234@` |
| 面板初始密码（首登即失效） | root / admin |
| 依赖 deb | `debs/`：lighttpd + mod-openssl（1.4.59 bpo10）、libxxhash0（bpo10）、chrony 3.4-4+deb10u2 |
| SSH 引导 | `python deploy/kbdint_ssh.py`；公钥 `C:\Users\YAO\.ssh\id_ed25519.pub` |

## 6. 遗留观察项（来自该会话，未结案）

- 板卡 eMMC 曾出现文件损坏（SSH 主机密钥跨断电复现 + 4 个 libmali GPU 库截断）；重启复测未再损坏，
  但损坏原因未定（怀疑 HDMI 损毁波及存储/供电）。建议：长稳压测，或修复 HDMI 焊接后复测存储健康度。
- 板卡无 RTC 电池：断电后回到出厂假时间（2019-02-14），日志时间戳与 90 天密码有效期均失真——
  真实授时依赖 UT986 PPS+TOD 链路（§3.6/§3.7）或外网 NTP。
