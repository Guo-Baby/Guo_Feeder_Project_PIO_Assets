# UI v1 · 阶段 5 完成报告：遥测曲线 + 审计 + 移动端（含配额约束说明）

> 日期：2026-10-25 · 前序：`UI-v1-Stage4-Tail-Restart-and-EstopVerify-2026-10-25.md`
> 用户需求基准：`docs/ui-v1/16-user-feedback-backlog-2026-10-25.md`（走查反馈全量记录，
> 待用户命令后反查 UI 与计划差异）

## 1. 交付内容（云仓 `d2c0716`+`a492586` · Worker `d2c53768` · Pages `40dbd0bf`）

### S5-01 遥测三条曲线 ✅
- 数据源：既有 `GET /api/read/history/telemetry`（keyset/type 过滤，零新端点）
- 设备页新增「遥测曲线（最近 200 点）」：重量 g / 温度 °C / 湿度 %，**纯 SVG 折线**
  （零外部图表依赖，`telemetryToPoints` + `svgLineChart` 纯函数可测）
- 渐进渲染（每条数据到即刷）+ 代际守卫（切页不回写）+ 单点/空数据占位
- 生产数据实测可用：weight 694 / temp 445 / humid 456 条（配额耗尽前核实）

### S5-02/03 日志解码 ✅（WP-A 已交付，本轮无改动）
- `?decode=1` 正常样本/非法样本降级显示均有既有测试覆盖（log_decode 29 项）

### S5-04 审计页（按 cmd_id 检索）✅
- 新端点 `GET /api/history/command`：cmd_id/device_id/state 过滤 + keyset 翻页
  （默认 100/上限 500）
- 记录页第三 tab「审计」：检索 + 状态 chip + 翻页 + 脱敏口径自陈文案

### S5-05 审计导出（CSV，脱敏）✅
- 客户端生成 CSV（BOM + CRLF，Excel 友好）
- **双道脱敏防线**：端点白名单列（`payload_hash`/`payload_note` 永不返回）
  + `auditRowsToCsv` 只认白名单列（防未来字段泄漏进导出）
- **公式注入防护**：前导 `=+-@` 包引号（CSV 打开不被当公式执行）

### S5-06 保留策略 ✅（核查，零改动）
- `retention.js` 冻结口径：telemetry_history **90 天** / log_history **180 天**，
  cron `0 3 * * *` 每日清理，分批+确定性谓词+预算不变量 —— 与 S5-06 要求对齐
  （S5-06 原文"90 天"对应 telemetry；log 180 天为 L-A 统一窗口的冻结设计）

### S5-08 移动端适配 ✅
- viewport meta / max-width（非 min-width）既有；新增 `.tscroll` 横向滚动容器
  （宽表格 375/390/414 宽度内不撑破页面），审计表已包

### S5-07 回归不退化 ✅
- 全量 **9 套件 775 PASS / 0 FAIL**（273+104+22+29+33+18 新+65+152+79）

## 2. 约束与诚实口径

- **生产实测 PENDING**：用户报告 Cloudflare D1 免费配额（读 50 万耗尽 / 写 10 万已用
  84%）于 UTC 24 日 0 点（北京 08:00）重置 ⇒ 本轮只做了本地验证 + 部署，
  生产端点实测（`/api/history/command`、曲线数据路径）**待配额恢复后补跑**，不预标通过
- 曲线数据可用性 = 配额耗尽前的 D1 直查证据（三类型均有真实数据）

## 3. 待用户命令的后续队列（backlog 摘要）

1. **★B1 本地缓存 + 版本对应**（配额事件后升为最高优先级）
2. A1/A2 首页快捷操作可编辑 + 模块排序 · A3 指定步数出粮 · A4 已删流程隐藏
3. A5 设备别名 · A6/A7 System State 全量状态面板（固件已支持，纯 UI）
4. B2 在线状态走 EMQX（Worker 代理 Management API，已核实可行）· B3 全量配置读取核实
5. C 模块配置 UI 化（中文/校验/GPIO 警示；config_set 固件已存在）
