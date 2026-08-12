# 03: user_create.cgi / user_passwd.cgi 接入策略

- Type: task
- Status: open
- Blocked by: 01, 02

## 任务

1. `user_create.cgi.c`:密码长度校验(6-64)替换为策略校验(10-64 + 四类,见 issue 01);插入时 `password_changed_at = now`。
2. `user_passwd.cgi.c`:同样接入策略;更新时 `password_changed_at = now`(重启计时,ADR-0002 决策 1),并踢掉该用户其他会话(auth_kick_user_sessions)。
3. 错误消息沿用现有 JSON 结构:无法创建时前端显示策略失败原因。

## 验证

- curl 创建:弱密码(如 `abc123`)拒绝并给出中文原因;强密码(如 `Abcd1234!xyz`)成功;
- root 重置为弱密码拒绝;重置成功后目标用户其他会话失效;
- audit_log 出现 create_user / reset_password 记录。
