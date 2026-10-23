# UI v1 阶段 4（Workflow 编辑器安全边界版）+ cmd_id 可辨识 — 实现与全面自查

日期：2026-10-24 · 范围：cmd_id 自增辨识 / Workflow CRUD 写通道 / 编辑器 UI / 全面自查

---

## 1. 本轮交付

### 1.1 cmd_id 可辨识自增（用户要求）
- UI 每条命令生成 `ui-<6位序号>-<动作名>`（如 `ui-000001-VALVE_OPEN`、`ui-000005-workflow_list`）。
  - 序号按设备存 localStorage **严格递增** ⇒ 天然满足 Worker 30 s cmd_id 去重（同 id 静默丢弃）；
    localStorage 不可用（隐私模式）⇒ 时间戳兜底，仍唯一。
  - slug：非 `[0-9a-zA-Z_-]` 折叠为 `_`、≤40 字符（Worker 规则 `{8,64}`，总长 ≤50 留余量）。
  - 实现落点：`gf-app.js nextCmdId()` + `gf-ui-core.js cmdSlugFor()`（纯函数，可测）。
- 效果：D1 `command_audit` / 日志里一眼认出「UI 发的第 N 条、什么命令」。

### 1.2 Worker 新增 `wfadmin` 命令族（Workflow CRUD 写通道）
- `capability-catalog.js`（v1.1.0）：
  - `WF_ADMIN_COMMANDS` 白名单**恰 3 条**：`workflow.create / workflow.set / workflow.delete`；
    **`workflow.save` 永不进目录**（固件 restart 默认 true ⇒ 触发设备安全重启）。
  - `validateWorkflowObject()` 镜像固件 `workflow_validate_workflow_json`：id 必填（1..64）、
    steps ≤ 16、step.type 必填 ∈ {trigger, action}、单步 params ≤ 8、参数值类型白名单
    （string/number/bool）、载荷 ≤ 6144 B。
  - **比固件更严的一处**：无 id 的 step 直接拒绝（固件 validate 放行但 apply **静默跳过**
    ⇒ "成功"提交丢数据）。
- `command-api.js`：family 白名单 + `wfadmin` 分支（校验/风险 DANGER/透传帧
  `{c:"workflow.create"|"workflow.set"|"workflow.delete", i, p}`，不需要 registry target）；
  `STALE_AFTER_MS.wfadmin = 90 s`。
- 契约依据（固件 `command_manager.cpp` 实读）：
  - create：`p = {"workflow":{…}}` 仅嵌套形态（平铺拒绝，防 set 语义误清空）；固件自动找空闲 slot；
  - set：`p = {"id":<slot 0..15>, "workflow":{…}}` **整体替换**；
  - delete：`p = {"id":<slot>}`，幂等（已 invalid 即 no-op 仍 success）；
  - CRUD 在 running/dirty 事务冲突时固件拒（s=4），云端如实透传终态。

### 1.3 UI Workflow 编辑器（安全边界版）
- workflows 页：列表行新增 编辑/删除；页脚「＋ 新建流程」；编辑器 = JSON 文本域 +
  **双端同源校验**（`validateWorkflowJson` 前置，坏 JSON 本地拦截不发命令）。
- 危险语义全部显性化：set=整体替换（steps 缺省=清空，确认框明说）、删除幂等未落盘、
  **只改内存账本（dirty）不落盘；本版不提供 workflow.save（会重启设备）**。
- 写成功后自动刷新列表；回帧展示 slot/stable_id/variant/dirty。

## 2. 全面自查（新代码 vs 旧代码）

### 2.1 与旧代码的匹配性（逐项核对固件/Worker 实码）
| 核对项 | 结论 |
|---|---|
| workflow.get 回帧 `{status, workflow:{…}, stable_id, valid, slot}` | ✅ 编辑器取 `frame.workflow` |
| workflow.list 回帧 `{registry_version, count, dirty, workflows:[{slot,id,variant,valid,stable_id}]}` | ✅ 列表/运行/编辑全部字段对上 |
| workflow CRUD 载荷形态（workflow_pick_object 仅嵌套） | ✅ Worker 校验镜像 + UI 同源 |
| `/api/ui/command` Bearer 链路 + Pages 代理 `...body` 透传 | ✅ cmd_id 原样到达 Worker 并回显 |
| LEGACY_KEYS（c/k/v/ob）拒绝 | ✅ 新 payload 不含旧键 |
| 旧 UI（index.html v1.1）与 catalog | ✅ 新字段 `wf_admin`/`wf_limits` 纯增量 |
| registry 漂移 | ✅ CRUD 触发固件 rescan ⇒ registry_version 变化；UI 既有 e=3 自恢复路径覆盖 |

### 2.2 自查发现并当场修复
1. **无 id step 静默丢数据**（P1，固件 apply 语义）⇒ 云端+UI 双端拒绝 `step_id_missing`。
2. **编辑器注记缺失**：补「无 id 的 step 会被固件静默跳过——每步都写 id」。

### 2.3 明确不做 / 已知限制（非 bug）
- `buildDeviceFrame` 的 wfadmin 分支信任调用方（内部函数，唯一入口 submitCommand 已校验）；
- workflow.save 在设备侧仍可通过串口/固件自身触发——云端 UI 只是不提供，不是固件封锁；
- 设备离线时 wfadmin 命令照常排队超时（四态条显示 unknown/timeout，与旧族一致）。

## 3. 测试与部署基线

- **全量 8 套件 691 PASS / 0 FAIL**：
  ui_security 273 · pages_auth 104 · pages_webhook 22 · log_decode 29 · history_frames 33 ·
  **wfadmin 55（新）** · ui_app 129（+12）· ui_app_runtime 46（+13）。
  - wfadmin 55 项：白名单/save 防线、固件校验镜像逐条、slot 边界、透传帧形态、
    submitCommand 集成（假 EMQX/D1：202+回显 cmd_id+恰 1 帧发布）。
  - runtime 46 项：DOM 桩驱动真实 gf-app.js——cmd_id 格式/递增/可辨识、
    编辑器打开/预填/坏 JSON 拦截/wfadmin 提交载荷/提交后刷新。
- **生产只读+防线实测**（`verify_wfadmin_prod.py`）：
  P-1 目录 3 条且无 save ✅ · P-6 `workflow.save ⇒ 400 unknown_wfadmin` ✅（Worker 防线）；
  **P-2 设备离线**（last_seen ~31 min）⇒ P-3~P-5（真机 create/get/delete 往返）如实标
  **NOT VERIFIED**，不伪造。设备恢复在线后重跑脚本即可补验。
- 部署：Worker **`717d2a76-e435-4e7e-b4fd-075e9df4217f`** · Pages **`b45407a6`**
  （`Compiled Worker successfully` + `Uploading Functions bundle` 判据齐全）。
- 排查插曲：脚本被 Cloudflare 1010（urllib UA 封禁）拦 ⇒ 加 UA 解决；快照端点
  Worker 直连路径是 `/api/admin/device/:id/snapshot`（`/api/read/*` 是 Pages 代理专有前缀）。

## 4. NOT VERIFIED / BLOCKED（滚动口径）

- **NOT VERIFIED**：wfadmin 真机往返（P-3~P-5，等设备在线）；workflow 真实运行首跑（继承）。
- **BLOCKED 不变**：HX711 假阳性（#6）、DRV8825、GPIO 电气、Turnstile 真验、
  D1 故障路径、config_query 二帧超时。
- 整机验收风险：编辑器写链路已有 Worker 级实测 + 双端校验，但**真机上尚未走过一次
  create→run→delete 闭环**；建议设备在线后先跑 `verify_wfadmin_prod.py` 再人工走查。

---

## 5. 追记（同日）：底部导航真机故障（用户实测发现）

**现象**：首页正常，点底部「控制/流程/记录/设备」全部无反应。

**根因**：`gf-app.js` 只有 `window hashchange` 监听 + `onRoute()`，**没有任何代码
去改 `location.hash`** —— nav 按钮从未接点击事件。冒烟测试直接改 `loc.hash`
再手动触发 hashchange，恰好绕过这一层 ⇒ 桩测试绿 ≠ 真机通（与登录死循环同型）。

**修复**（云仓 `27ffc53` · Pages `56551fbd`）：nav 按钮接 onclick ⇒ 改 hash；
同页再点 = 手动刷新。回归锁：测试桩 hash setter 补真实浏览器语义（赋值即触发
hashchange）；T-12 五项真实点击断言（无接线必失败）+ S-12-12 结构断言。

**口径更新**：全量 **696 PASS / 0 FAIL**（273+104+22+29+33+55+130+51）。
教训进铁律：交互入口（点击/提交）必须有一条「从真实事件出发」的测试路径，
不能只测事件处理函数本身。
