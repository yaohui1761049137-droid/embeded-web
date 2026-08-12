# 02: DB 迁移——users 表加 password_changed_at

- Type: task
- Status: open
- Blocked by: 01

## 任务

1. `src/db_init.c` 建表语句:`users` 加 `password_changed_at INTEGER NOT NULL DEFAULT 0`。
2. 板子存量库迁移脚本(部署时执行):
   `ALTER TABLE users ADD COLUMN password_changed_at INTEGER NOT NULL DEFAULT 0;`
   (SQLite 对已有行自动填 0 → 全部视为存量过期,首登强制改密,符合 ADR-0002 决策 4)
3. `db_init` 新建的 root/admin 密码也写 0 → 首登强制改密(验收标准 7)。

## 验证

- 新库建表字段存在;旧库 ALTER 后查询各列正常,旧行 `password_changed_at == 0`;
- 登录流程能读到该列(与 issue 01 的到期原语联动)。
