# 05: 强制改密期间后端拦截所有 CGI

- Type: task
- Status: open
- Blocked by: 01

## 任务

1. 在 auth.c 提供统一入口,如 `int auth_require_password_current(const SessionInfo *s)`:
   查 users.password_changed_at,判定需改密 → 返回 0。
2. 所有受保护 CGI(排除 `user_change_pass.cgi`、`logout.cgi`)在 `auth_session_verify` 之后调用:
   - `network.cgi`(get/set/confirm)、`user_create.cgi`、`user_delete.cgi`、`user_list.cgi`、`user_passwd.cgi`、`user_toggle.cgi`、`action.cgi`、`main.cgi` 等;
   - 拦截时返回 `{"status":"error","message":"密码已过期,请先修改密码"}`(HTTP 200,与现有错误 JSON 一致,前端已能处理 error 分支)。
3. 前端可据此在任意页面被拒时跳转 change.html(issue 07 处理)。

## 验证

- 存量用户登录(需改密)后,逐个 curl 上述 CGI 全部返回过期错误;改密成功后全部恢复;
- 正常用户不受影响;
- logout.cgi 与 user_change_pass.cgi 不被拦截(否则死锁)。
