# 01: auth.c 密码策略核心(校验/到期/踢会话)

- Type: task
- Status: open

## 任务

在 `src/auth.c` / `auth.h` 增加密码策略与到期的核心原语:

1. `int auth_password_policy_ok(const char *pw, char *err, int err_max)` — 校验:
   - 长度 10-64(现有上限 64 不变);
   - 四类各 ≥1:大写、小写、数字、ASCII 可见标点;
   - 拒绝中文/空格/控制字符(>0x7E 或 <0x21 视为非法,注意 ASCII 标点即 0x21-0x7E 中非字母数字者);
   - 中文错误消息(指明缺哪类/长度/非法字符)。
2. 到期原语:`int auth_password_expired(const char *pw_hash, int64_t changed_at)` 或直接查询 users 表:
   - `changed_at == 0` → 过期(存量);
   - `now > changed_at + 90*24*3600` → 过期;
   - 返回剩余天数接口供 7 天预警用。
3. 踢会话:`void auth_kick_user_sessions(int user_id, const char *keep_sid)` — `DELETE FROM sessions WHERE user_id=? AND sid != ?`。

## 验证

- 板子或本地单测:合法/非法密码样例(缺类、中文、空格、长度边界)返回预期错误消息;
- 到期原语用构造的 changed_at 验证 0 / 边界 / 90 天前后;
- 踢会话后旧 sid 失效、当前 sid 存活。
