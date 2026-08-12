# Spec: 高强度密码策略与 3 个月有效期

- 状态:待实现
- 来源:/grill-with-docs 拷问定案(2026-08-11),决策见 [ADR-0002](../../docs/adr/0002-password-policy.md)
- 词汇:密码策略、密码有效期、强制改密、存量用户、踢会话(见 CONTEXT.md 词汇表)

## 目标

在用户创建/密码管理功能上落地高强度密码策略:10+ 字符、四类组合、每次修改后 90 天有效期,到期登录后强制改密。

## 验收标准

1. **创建用户**(user_create.cgi):新密码不满足策略 → 拒绝并返回中文错误(指明缺哪类/长度/含非法字符)。
2. **root 重置密码**(user_passwd.cgi):同上;成功后该用户 `password_changed_at` 置为当前时间,该用户其他活跃会话被踢。
3. **用户自改密**(新 user_change_pass.cgi + change.html):需输入当前密码验证;新密码须满足策略且与当前密码不同;成功后重置计时、踢掉其他会话(保留当前)。
4. **到期判定**:登录时 `password_changed_at == 0` 或 `now > password_changed_at + 90天` → 判定"需强制改密"。
5. **强制改密流程**:到期/存量用户登录成功 → 302 到 change.html;期间除改密 CGI 外所有受保护 CGI 返回 "密码已过期,请先修改密码"(HTTP 200 + JSON error),直到改密成功。
6. **7 天预警**:密码距到期 ≤7 天且未过期时,登录后的主控页显示"密码将于 X 天后到期"横幅(不阻断)。
7. **存量迁移**:策略上线时现有用户 `password_changed_at` 一律置 0;`db_init` 新建的 root/admin 同样置 0 → 首次登录强制改密。
8. **内置 root 无豁免**:root/admin 同样受强度与到期约束;root 密码过期时走同一强制改密流程(不锁死)。
9. **审计**:创建/重置/自改均记 `audit_log`(自改动作 `change_password`,重置沿用 `reset_password`)。
10. **测试**:test_suite.sh 新增用例覆盖:合法密码、缺类拒绝、长度拒绝、中文/空格拒绝、与旧密码相同拒绝、到期强制、改密后恢复、踢会话。

## 范围外(本期不做)

- 密码历史表(最近 N 次禁复用)——见 ADR-0002 备选 C。
- 常用/弱密码黑名单、登录失败锁定、验证码。
- 到期判定不引入 NTP 依赖;板载时钟漂移导致的误判风险文档明示即可。

## 改动面

| 文件 | 改动 |
|---|---|
| `src/auth.c` / `auth.h` | 新增策略校验、到期状态查询、踢会话;`SessionInfo` 或查询接口返回强制改密标志 |
| `src/db_init.c` | users 建表加 `password_changed_at` 列(默认 0) |
| `src/user_create.cgi.c` | 接入策略校验 + 写 `password_changed_at` |
| `src/user_passwd.cgi.c` | 接入策略校验 + 重置计时 + 踢会话 |
| `src/login.cgi.c` | 到期判定;POST 成功按需 302 到 change.html;GET 会话检查同理 |
| 各受保护 CGI(`network.cgi`/`user_*.cgi`/`action.cgi` 等) | 会话校验后增加强制改密拦截检查 |
| `src/user_change_pass.cgi.c`(新) | 自改密:验证旧密码 → 策略检查 → 更新 + 计时 + 踢会话 |
| `www/change.html`(新) | 自改密表单(旧/新/确认) |
| `www/control_panel.html` / `index.html` | 控制面板"修改密码"入口;到期预警横幅;登录后跳转逻辑 |
| `test_suite.sh` | 策略与到期用例(见验收 10) |
| `README.md` / `QUICKSTART.md` | 记录策略与迁移说明 |

## 部署与迁移

- 板子已有数据库:先执行 `ALTER TABLE users ADD COLUMN password_changed_at INTEGER NOT NULL DEFAULT 0;`(存量行自动为 0),再替换 CGI 二进制。
- 跨编译方式沿用现有流程(sysroot + aarch64-linux-gnu-gcc),勿用静态链接。
