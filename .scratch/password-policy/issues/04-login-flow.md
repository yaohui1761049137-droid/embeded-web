# 04: login.cgi 到期判定与强制改密跳转

- Type: task
- Status: open
- Blocked by: 01

## 任务

`login.cgi.c`:

1. 登录成功后查 `password_changed_at` 判定"需强制改密"(==0 或超 90 天,见 issue 01 到期原语)。
2. 需改密 → `Location: /change.html`(Set-Cookie 照常下发,会话已创建);否则维持 `Location: /cgi-bin/main.cgi`。
3. GET(会话检查)分支同样处理:已登录但需改密 → 302 到 change.html。
4. 距到期 ≤7 天且未过期 → 会话正常,但需向主控页传递预警信息(实现方式见 issue 07,可经 session 或用户状态接口)。

## 验证

- 新用户(存量,changed_at=0)登录 → 302 change.html;改密前访问任意其他 CGI 被拦截(issue 05);
- 正常用户登录 → 302 main.cgi;
- 剩余 7 天内的用户登录 → 主控页出现预警横幅。
