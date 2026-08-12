# 06: 新 user_change_pass.cgi 自改密

- Type: task
- Status: open
- Blocked by: 01, 02

## 任务

新建 `src/user_change_pass.cgi.c`(POST):

1. 会话校验(不要求 root;任何已登录用户可用;强制改密期间**必须放行**)。
2. 参数:`old_password`、`new_password`(前端另带确认字段,后端以 new_password 为准)。
3. 旧密码验证:`auth_user_login(username, old_password)` 必须成功(防会话劫持者改密)。
4. 新密码:`auth_password_policy_ok` + 与旧密码不同(禁复用,ADR-0002 决策 2)——"与当前密码相同"的判定用新哈希与旧哈希比对(auth_verify_password(new, old_hash)),而非明文比较。
5. 成功后:更新 `password_hash`、`password_changed_at = now`,踢掉该用户其他会话(保留当前),记 `audit_log(change_password)`。
6. 响应 `{"status":"ok","message":"密码修改成功"}`。

## 验证

- 旧密码错误 → 拒绝;新密码弱/含非法字符/与旧相同 → 拒绝并给出中文原因;
- 成功后旧密码登录失败、新密码可登录、其他会话失效、当前会话仍可用;
- 强制改密状态下可正常调用(不被 issue 05 拦截)。
