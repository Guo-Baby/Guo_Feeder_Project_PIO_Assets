# Guo Feeder 累积问题清单（OPEN ISSUES）

> **最后更新**：2026-10-30 15:38（GMT+8）
> **配套**：`HANDOVER.md`（状态快照）· `SOURCE_INDEX.md`（资料索引）· `NEXT_SESSION.md`（任务书）
> **路径约定**：`WS` = `D:\Guo_Feeder_Project`；路径相对 `WS`。

## 0. 状态词表（严禁混用）

| 词 | 含义 |
|---|---|
| `OPEN` | 已确认存在，未解决 |
| `BLOCKED` | 需外部条件 / 授权 / 硬件才能推进 |
| `NEEDS_REVIEW` | **尚未检查代码或证据**，现状未知，不得当成已实现 |
| `UNKNOWN` | 证据不足，无法判断 |
| `MANUAL_PENDING` | 只能人工验收（如 Turnstile 登录后目视） |
| `NEEDS_USER_AUTHORIZATION` | 纯待用户授权，非技术阻塞 |
| `IMPLEMENTED` | 代码已写，但未真机验证 |
| `AUTOMATED_TEST_PASS` | 自动化测试通过（≠ 真机通过） |
| `DEVICE_VERIFIED` | 真机证据已取得 |
| `CLOSED` | 已解决且有证据 |

> **铁律**：未取得真机证据的，不写 `DEVICE_VERIFIED`；未检查代码的，不写"已实现"。  
> **区分四类失败**：① 用户实际验收失败 ② 自动化测试失败 ③ 仅待真实设备验证 ④ 纯待授权。

---

## 1. 米家设备（GF-MI）

### GF-MI-01 · 米家喂食器真实局域网状态查询
- **状态**：`OPEN`（已获一次真机成功，稳定性/语义待确认）
- **类型**：待真实设备验证
- **现象**：控制页需要读 `online / out_status` 才能决定能否出粮。早期"点了没反应"，后定位为前端静默吞错。
- **证据**：2026-10-29 20:27 真机（device `001122334455`）查询返回 `xiaomi_feeder_online = true`、`xiaomi_feeder_out_status = 0`；`log_history` 同步有 2305/2307 采集事件。
- **已尝试**：EMQX API 下发 `query_state` 真机帧验证（设备回 `s=0`）；前端 `queryState` 失败改为抛错并在控制区持久显示。
- **根因**：前端吞错**已确认并修复**；"查询是否每次都稳定成功"**未确认**（仅一次真机成功样本）。
- **下一步**：连续多次（≥5 次、跨 10 分钟）只读查询，统计成功率与 `*_last_err` 分布；核对 `out_status=0` 的语义（是否=空闲）。
- **验收标准**：连续 5 次查询均 `online=true` 且 `last_err=1`（正常），UI 状态与 D1/串口一致；`out_status` 语义有文档依据。

### GF-MI-02 · 智能插座真实局域网状态查询及 offline 根因
- **状态**：`OPEN` / `BLOCKED`（物理原因需现场）
- **类型**：待真实设备验证 + 待调查
- **现象**：控制页插座显示"不可达/离线"，`xiaomi_plug_power=false`。
- **证据**：2026-10-29 真机查询 `xiaomi_plug_online = false`、`xiaomi_plug_power = false`；固件启动即把 `plug_power` 置 false（`xiaomi_lan.cpp` 约 807 行），只有 LAN 查询成功才更新（约 534 行）⇒ 该 false 表示**"没取到"而非"已断电"**。
- **已尝试**：确认 NVS `p_ip/p_tok` 独立存在且写入生效；UI 已改为"离线不显示已断电"。
- **根因**：**未确认**。候选：① 插座物理断电/未上电 ② 不在同一网段 ③ DHCP 换了 IP ④ Token 错 ⑤ 设备被停用。现有证据**不足以**排除任一。
- **下一步**：设备侧只读诊断（`xiaomi_plug_last_err` / `recv` / `badsum` / `timeout` 计数）；确认插座物理供电与 Wi-Fi；回读 `p_ip/p_tok` 并与米家 App 核对；同网段 ping/UDP 探测。**不得改生产配置**。
- **验收标准**：`xiaomi_plug_online=true` 且 `last_err=1`，控制页显示"在线"；或明确给出物理不可达的现场证据并转为 `BLOCKED`。

### GF-MI-03 · 米家设备配置与启用开关 UI 结构
- **状态**：`AUTOMATED_TEST_PASS` / 待用户验收
- **类型**：待用户验收
- **现象**：需要两台设备各自独立的 enable / IP / Token / 保存 / 显隐。
- **证据**：`config-spec.js` 新增 `xiaomi_lan` 模块（`hasEnable:false`，两张 `cards`：`feeder_enable` / `plug_enable`）；`gf-ui-core.js::configModuleCards()` 支持 `cardKey`；`ui_responsive_n.mjs` N4 断言 2 enable / 2 Token / 2 IP / 2 保存 / 2 显隐，Token 为 `password` 且不落 localStorage — **PASS**。
- **根因**：不适用（功能实现）。
- **下一步**：部署后由用户在生产 UI 目视确认两张卡与开关行为。
- **验收标准**：生产 UI 中两张卡独立可编辑、Token 默认隐藏、保存后回读一致。

### GF-MI-04 · System State 中两台米家设备状态展示
- **状态**：`AUTOMATED_TEST_PASS` / 待用户验收
- **类型**：待用户验收
- **现象**：需要在一个面板看到喂食器/插座/诊断键，且空值不伪造。
- **证据**：`gf-ui-core.js` System State 新增「米家喂食器」「米家智能插座」「米家局域网诊断」分组 + `enumMap`；`ui_responsive_n.mjs` N3 断言分组存在且"未查询不伪造 0 ℃" — **PASS**。
- **根因**：不适用。
- **下一步**：部署后目视；确认 9 个键（online / out_status / plug_online / plug_power / last_err ×2 / last_ok_ts ×2 / autooff_sec）都有合理空态文案。
- **验收标准**：生产 UI 三个分组可见，未知值显示"未查询/未知"而非 0 或空白。

### GF-MI-05 · 米家喂食器控制页出粮份数及二次确认
- **状态**：`IMPLEMENTED` / `AUTOMATED_TEST_PASS` / `NOT_RUN`（真机）
- **类型**：待真实设备验证 + 待授权
- **现象**：需要 1–10 份选择 + 二次确认 + 延时锁，且非幂等不重试。
- **证据**：`gf-xiaomi.js` 受限 1–10 份下拉 + `XIAOMI_FEED`；`ui_responsive_n.mjs` N5 断言"出粮弹出二次确认 / 确认键初始禁用 / 取消不发命令" — **PASS**。
- **根因**：不适用。
- **下一步**：**需用户逐次授权**后执行一次真实出粮，核对设备实际出粮份数与 result 帧。
- **验收标准**：真机出粮份数与选择一致；取消不产生命令；结果未知时 UI 不显示"已完成"且不自动重试。

### GF-MI-06 · 智能插座普通通电 60 秒安全限制
- **状态**：`IMPLEMENTED` / `NOT_RUN`（真机）
- **类型**：待真实设备验证 + 待授权
- **现象**：普通通电需设备端 60 秒后自动断电，不依赖浏览器。
- **证据**：`XIAOMI_PLUG_ON_60S` + `armAutoOff(kPlugAutoOffSec)`；**时钟无效时拒绝执行**（`time(nullptr) <= 0` ⇒ 报 BUSY，因无法保证自动断电）；`xiaomi_plug_autooff_sec` 暴露剩余秒。
- **根因**：不适用。
- **下一步**：授权后真机验证：通电 → 60 s 后设备自动断电；关页面仍生效；时钟无效时确被拒绝。
- **验收标准**：设备端计时准确（±2 s），断电由设备执行而非浏览器；时钟无效时命令被拒且有明确错误码。

### GF-MI-07 · 智能插座强制通电与断电
- **状态**：`IMPLEMENTED` / `NOT_RUN`（真机）
- **类型**：待真实设备验证 + 待授权
- **现象**：强制通电无自动断电；断电任何时候都可点。
- **证据**：`XIAOMI_PLUG_ON` / `XIAOMI_PLUG_OFF`；U I 上"断电"仅受 `busy` 约束（不因状态未知禁用）；`ui_responsive_n.mjs` N5 断言"状态未知时断电仍可用" — **PASS**。
- **根因**：不适用。
- **下一步**：授权后真机验证三动作顺序与断电兜底。
- **验收标准**：强制通电持续到手动断电；断电在状态未知时仍成功；无自动断电。

### GF-MI-08 · 自动化新增米家设备动作的完整闭环
- **状态**：`NEEDS_REVIEW`（**未检查** workflow 是否已接入 `XIAOMI_*`）
- **类型**：未检查
- **现象**：Workflow 引擎是否能引用米家动作（出粮/通断电）尚不明确。
- **证据**：目前只确认 `capability_registry` / `workflow` 存在；**未核对** `XIAOMI_FEED` / `XIAOMI_PLUG_*` 是否在 workflow 动作表内。
- **根因**：`UNKNOWN`。
- **下一步**：检查 `src/automation/capability_registry.cpp`、`workflow.cpp` 与云端 `capability-catalog.js` 是否包含米家动作；若缺，评估是否需要（需用户确认产品需求）。
- **验收标准**：明确回答"是/否支持"，并给出代码行号；若支持则有一条端到端（配置→触发→result）证据。

### GF-MI-09 · 米家登录会话与 2FA（补充项）
- **状态**：`IMPLEMENTED` / `NEEDS_REVIEW`（生产已开启，未回归）
- **类型**：待回归
- **现象**：米家云登录（含 2FA）用于同步历史；生产 `XIAOMI_LOGIN_ENABLED=1`。
- **证据**：`migrations/0012_xiaomi_auth_pending.sql`；`src/xiaomi/` 同步模块；`docs/reports/2026-10-28-phaseC/phaseD/phaseE` 有登录报告。
- **根因**：不适用。
- **下一步**：只读核对当前会话有效性；2FA 渠道以 `identity/list` 的 options 为准（4=手机 8=邮箱），发码成功≠校验通过。
- **验收标准**：登录态可查、过期可识别、失败有明确错误码。

---

## 2. UI / 前端（GF-UI）

### GF-UI-01 · 多余温湿度计状态模块与 System State 数据不一致
- **状态**：`CLOSED`（2026-10-29 已删除）
- **类型**：已解决
- **现象**：设备页曾同时有"温湿度计状态"小节与 System State 面板，两者刷新时机不同，出现"有值但显示无效"的自相矛盾。
- **证据**：`gf-xiaomi.js` MOUNTS 已移除 `thermo` 小节（代码注释保留原因）；`ui_responsive_n.mjs` N3 断言 `[data-xj="thermo"] === null` 且 System State 含「温湿度」分组 — **PASS**。
- **根因**：重复展示 + 双数据源，**已确认并消除**。
- **备注**：若用户所指为另一处（非设备页 thermo 小节），请重新开启并附截图。

### GF-UI-02 · 首页不可靠出粮颗数显示
- **状态**：`NEEDS_REVIEW`
- **类型**：未检查
- **现象**：用户报告首页"出粮颗数"不可靠（具体表现未记录）。
- **证据**：本轮**未定位**对应 DOM/字段；`weight.value` 遥测存在（15 min 节拍），但"颗数"换算逻辑未核对。
- **根因**：`UNKNOWN`。
- **下一步**：定位首页该字段的渲染来源（`gf-app.js` 首页渲染 + `/api/read/ui/*`）；确认它是"颗数估算"还是"重量"，是否在 HX711 未接时伪造。
- **验收标准**：该字段有明确数据来源与空态；无数据时显示"未取到"而非 0；不把重量/估算当确定颗数。

### GF-UI-03 · 生产登录后的真实 UI 遥测目视验收
- **状态**：`MANUAL_PENDING`
- **类型**：待人工
- **现象**：Turnstile 人机验证无法自动化，登录后首页遥测只能人工看。
- **证据**：D1 已有遥测（10-29 新 id 1484+，22.3/51.1 → 22.1/52.8）；线上 UI 仍 `778a6768`。
- **根因**：不适用（流程限制）。
- **下一步**：用户登录生产 Pages 后确认：首页温湿度 ≈ 22.x / 52.x、时间戳新鲜、控制页三态正常、米家两卡存在。
- **验收标准**：人工截图或描述确认数值与 D1 一致，且无"NaN/undefined/0 ℃"破相。

---

## 3. 硬件 / 现场（GF-HW）

### GF-HW-01 · 插座局域网物理可达性及设备自答 offline 的根因
- **状态**：`OPEN`（与 GF-MI-02 同源，此处记录物理侧）
- **类型**：待现场确认
- **现象**：设备自答 `plug_online=false`，但插座是否真的断电/断网未知。
- **证据**：同 GF-MI-02。
- **根因**：**未确认**，不得臆断。
- **下一步**：现场确认插座指示灯/供电、Wi-Fi 连接、路由器 DHCP 租约；必要时在路由器侧查该 MAC。
- **验收标准**：能明确回答"插座是否在同一网络且上电"；若否，记录为环境问题并解除软件嫌疑。

---

## 4. 真机定时与发布（GF-V / GF-DEP）

### GF-V-01 · V-1 至 V-8 真机定时项验证
- **状态**：`BLOCKED`（需烧录 + 逐包批准）
- **类型**：待授权
- **现象**：V-1~V-8 为一组定时/调度真机验收项，历史多次标记待验。
- **证据**：`docs/reports/2026-10-27-*` 与 `2026-10-29-phase*` 中多次出现 `NOT_RUN`；本轮**未逐条核对** V-1~V-8 定义（在历史报告/台账中）。
- **根因**：不适用（流程/授权阻塞）。
- **下一步**：从 `docs/reports/2026-10-29-phaseK-task-ledger.md` 取回 V-1~V-8 定义，逐项列验收脚本；需用户批准烧录与逐包执行。
- **验收标准**：每项有 PASS/FAIL 证据（串口 / D1 / 设备行为），未做的标 NOT_RUN。

### GF-DEP-01 · 是否部署当前 Pages 工作区修改
- **状态**：`NEEDS_USER_AUTHORIZATION`
- **类型**：纯待授权
- **现象**：工作区含阶段 O 后续 UI 改动 + 本轮 `gf-xiaomi.js` 反模式修复，线上仍是 `778a6768`。
- **证据**：三仓 dirty（PIO 26 / CF 55 / EMQX 2）；本轮未部署。
- **根因**：不适用。
- **下一步**：用户明确授权后，用 `guo-feeder-pagesdev/tools/deploy_pages.sh` 白名单部署，再做线上冒烟 16 项 + 登录目视。
- **验收标准**：部署后 `deployments status` 出现新版本；`/.dev.vars` 等敏感路径仍不可匿名访问；线上 UI 含新控制动作。

### GF-DEP-02 · Worker 入站校验 `observe` → `enforce`（补充项）
- **状态**：`OPEN`（`S3-ENFORCE-PENDING`）
- **类型**：待决策
- **现象**：`ingest-validate.js` 当前 `observe`，非法报文只计数不拒绝。
- **证据**：`docs/architecture/G4-S1-S6-Execution-Report-2026-10-21.md`；`invalid_total` 为 per-isolate，生产值 `NOT MEASURABLE`（不是 0）。
- **根因**：不适用（策略选择）。
- **下一步**：需真实流量下多 isolate 采样均为 0，再单独授权切换。
- **验收标准**：切换后合法报文零误拒，非法报文被拒并计数可读。

---

## 5. 测试与文档（GF-TEST / GF-DOC）

### GF-TEST-01 · 测试用例数在不同报告中不一致（补充项）
- **状态**：`CLOSED`（已澄清为"用例持续新增"，非缺陷）
- **现象**：历史日志出现 `xiaomi_ui 121/134/146/178`、`ui_responsive_n 130/133/142/143` 等不同数字。
- **证据**：测试文件持续新增断言（L3~L10、N7/N8）；本轮实跑为 `xiaomi_ui 178`、`ui_responsive_n 142`、`ui_app_runtime 134`、Worker `run_all` 全通过。
- **根因**：**已确认**——用例数随开发增长，非测试失效。
- **备注**：引用测试结果时必须注明"哪一轮、哪个文件版本"，不得跨轮混用。

### GF-DOC-01 · 交接文档缺失 / 过期
- **状态**：`CLOSED`（本轮创建）
- **现象**：`HANDOVER.md / OPEN_ISSUES.md / SOURCE_INDEX.md / NEXT_SESSION.md` 此前不存在；`HANDOFF.md` 停在 2026-10-21。
- **证据**：本轮在 `PIO_Assets/Guo_Feeder_Project/docs/handover/` 创建四份文件。
- **根因**：**已确认**——历史交接只维护单个 `HANDOFF.md`，未拆分。
- **备注**：以后以 `docs/handover/` 为准；`HANDOFF.md` 仅作沿革。

---

## 6. CLOSED 速览（历史已解决，仅留索引）

| ID | 结论 | 关键证据 |
|---|---|---|
| GF-UI-01 | 重复温湿度小节已删 | `ui_responsive_n` N3 PASS |
| GF-TEST-01 | 用例数漂移已澄清 | 本轮实跑数字 |
| GF-DOC-01 | 四份交接文件已建 | 本目录 |
| （历史）遥测断更 | 编译门未开 + 运行时键缺失 ⇒ 已修 | 固件 `71556b9c` + D1 16:39:59 落库 |
| （历史）`.pio/build` 丢失 | PlatformIO auto-clean | `clean_build_dir()` 源码 + 归档脚本 |

> 说明：`（历史）` 条目无独立 ID，仅防止重复调查；细节见 `docs/reports/2026-10-29-telemetry-stop-forensic-investigation.md` 与 `2026-10-29-phaseO-wrapup-verification.md`。
