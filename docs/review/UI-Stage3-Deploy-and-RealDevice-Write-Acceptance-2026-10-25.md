# 阶段三（P4 配置 UI 化）部署 + 真机写入验收 —— 2026-10-25

> 对应任务书「Guo Feeder：阶段三部署与真机验收 → 阶段四多设备实施」的 **A 段**（部署 + 生产只读验收）与 **B 段**（真机写入验收 T1–T7）。
> C 段（阶段四多设备）见 `docs/review/UI-Stage4-P5-MultiDevice-Delivery-2026-10-25.md`。
>
> 贯穿全文的判定纪律：**部署成功 ≠ 生产验收成功；自动化测试通过 ≠ 真机验收通过。**

---

## 0. 结论速览

| 段 | 结论 | 数字 |
|---|---|---|
| A · 阶段三部署 | ✅ 已部署（Worker + Pages） | Worker `a874c516`，Pages `fa8e6f9d`，**门禁保持关闭** |
| A · 生产只读验收 | ✅ 通过 | **49 PASS / 0 FAIL**（`scripts/verify_stage3_prod.mjs`） |
| A · 写窗口 | ✅ 已开、已测、已关 | 窗口 Worker `1a2469d7` → 关闭后 `fdfddd54` |
| B · 真机写入 | ✅ T1–T7 可判定项全过 | **13 PASS / 0 FAIL / 0 NOT RUN** |
| B · 还原 | ✅ 已还原并复核 | `time.sync_interval_sec=86400`、version 3、boot_seq 100 |
| B · 最终开关态 | ✅ 关闭（未翻默认） | `config_write.enabled=false`，写命令实测 403 + 零下行帧 |
| B · P4-3 破坏性演练 | 🚫 **NOT RUN**（需真机故障注入） | ⇒ **写配置门禁继续保持关闭**，这是硬前置 |

**阶段三未宣称"完全验收"**：R15（配置失败/回滚处理）的破坏性路径与 R30（写配置全面开放前置）仍 🚫，理由就是 P4-3 演练未做。

---

## A. 阶段三部署与生产只读验收

### A-1 部署前自查发现并修复的**真缺陷**（不在任务书清单内，但必须先修）

| 编号 | 缺陷 | 为什么是真缺陷 | 处置 |
|---|---|---|---|
| **X15** | 写通道门禁只写在**目录**里，`command-api.js` **从不校验** | 目录只是给 UI 看的"提示"。浏览器 MQTT 直连、或任何直接打 `/api/command` 的调用方**完全绕过门禁** ⇒ 所谓"默认关闭"其实是"只有 UI 关着" | 新增 `resolveConfigWrite(env)` + `configWriteAllowed(env, deviceId, module, key)`，服务端 403；三级：开关 → 设备白名单 → 键白名单 |

> 这是本轮**最重要**的一条：如果不修就部署，等于在生产上写配置只靠前端自觉。

### A-2 部署记录

| 项 | 内容 |
|---|---|
| 部署前 | Worker `a5a030a7`（阶段二）／ Pages `0d960fb4`（阶段二） |
| **阶段三 Worker** | `a874c516-bdf2-4938-b9e7-33721cafcfb0`（回滚目标 `a5a030a7`） |
| **阶段三 Pages** | `fa8e6f9d-2f92-4754-9216-80d57241ee93`（回滚目标 `0d960fb4`） |
| 差异范围 | Worker 3 文件（`capability-catalog.js` / `command-api.js` / `index.js`）+ Pages 3 文件；**零 migration、零 EMQX 变更** |
| 首次部署的门禁态 | **`config_write.enabled=false`**（未因"要真机测试"就对所有设备开放写） |
| 代码提交 | 云仓 `a49304a` |

### A-3 生产只读验收：49 PASS / 0 FAIL

`scripts/verify_stage3_prod.mjs`（只读：只发 GET；唯一一次 POST 是**故意打给写通道、预期被 403** 的门禁复核）。

关键项（★ = 判据级）：

- ★ 目录自陈 `config_write.enabled=false`，且**服务端**写命令实测 **403 `config_write_disabled`** + **零下行帧**（S3-11a/11c）——证明"命令根本没发到设备"，不是"发了没生效"。
- ★ 设备 `config_query` **真读**通过：10 个模块、顺序 = 固件 `kModuleNames`、带 `versions`（S3-10f/10g/10h）。
- ★ 设备实际字段 ⊆ 目录键（S3-10i，目录没漏固件真有的键）；目录有而 Flash 未存的 17 个键记为 NOTE（固件走 getter 兜底，UI 显示"未读取"，**不编造值**）。
- ★ 敏感项出厂值不下发（S3-12a/12b）；WiFi SSID `wqs1` 记为**已接受风险** NOTE（非密码类）。
- 鉴权边界未削弱：无令牌 401、Pages 代理 401、未授权 admin 拒（S3-x）。
- 阶段一/二回归：审计端点老签名 200、family 筛选生效、日志/遥测端点 200、生产产物仍含 `filterLogRows`/`auditRiskView`/重启六段（S3-13a..f）。

**已更正的历史结论**：旧记录写"生产未配 Turnstile"——实测**已配**（无 token 登录 401 `captcha_failed`、连续失败 429 限流 600 s）。⇒ 会话级页面**自动化拿不到会话**，只能人工验收（R1 由用户人工验收完成）。

---

## B. 真机配置写入验收（T1–T7）

### B-1 为什么选 `time.sync_interval_sec`

| 筛选条件 | 该键的情况 |
|---|---|
| 固件**真支持**（不是云端臆造的键） | ✅ `time` 模块真实键，`config_query` 能读回 |
| 低风险 | ✅ 只影响对时间隔，**不碰供水/电机/阀门**（无水安全风险） |
| 可观测 | ✅ 写 RAM 后能立即读回；落盘后 version+1；重启后值仍在 |
| 可还原 | ✅ 原值 86400 已记录，可写回并落盘 |
| 不需要重启才生效 | ✅（但**可**用于验证"重启后仍生效"，即 T3） |

**没选**任何 water/valve/stepper 键 —— 那些牵动供水与电机，真机误改有物理风险。

### B-2 写窗口（最窄化）

| 维度 | 取值 |
|---|---|
| 开关 | `CONFIG_WRITE_ENABLED=1` |
| 设备白名单 | `CONFIG_WRITE_DEVICES=288485896ce4`（**只有这一台**） |
| 键白名单 | `CONFIG_WRITE_KEYS=time.sync_interval_sec`（**只有这一个键**） |
| 时间窗 | 2026-10-25T17:32Z – 17:43Z（约 11 分钟，测完立即关闭） |
| 未放宽的 | 鉴权（仍需 Bearer）、设备白名单、版本校验（`expect_version` 照常生效） |
| 承载版本 | Worker `1a2469d7-0d8e-4384-a932-cef33a1fe380` |

关闭方式：**重部署去掉这些环境变量** ⇒ Worker `fdfddd54-3c27-4e03-90f2-b410289f99e0`；随后实测目录 `enabled=false` + 写命令 **403** + 零下行帧。
**默认常量 `CONFIG_WRITE_ENABLED_BY_DEFAULT` 仍为 `false`，从未改成 `true`。**

### B-3 脚本（`scripts/hw_config_write_test.mjs`）

按任务书要求逐条落实：

- 必须 `--device <12hex>` + `--confirm-gate` 才跑（**不枚举设备、不批量**）；
- 启动即读目录，窗口没开 / 设备不在白名单 / 键不在白名单 ⇒ **直接退出**；
- 前置（PRE）：设备在线（最新遥测 < 15 min）+ 原值/版本/boot_seq 取证；
- 每一步**分列记录**：`http`（受理）／`terminal`（云端终态）／`frames`（设备回帧）／`deviceReadback`（设备实测值）；
- 判定纪律写进代码：**202=受理、ACK=收到、`s=1`=入队，只有设备 result 帧 `s=0` 才算成功**；无回帧 = **未知**，不当成功；
- 失败即停；支持 `--restore` / `--verify-only`；
- 输出机器可读 `result.json` + 人读 `report.md`。

### B-4 结果：13 PASS / 0 FAIL / 0 NOT RUN

证据：`docs/evidence/config-write-realdevice-2026-10-25/{result.json,report.md,final-recheck.json}`

| # | 项 | 结论 | 证据（设备实测） |
|---|---|---|---|
| T1 | RAM 改写 | **PASS** | 设备 `s=0`；值 **86400 → 90000**；version **1 → 1**（固件 `version++` 只在 `save_module` 落盘时发生 ⇒ 不增才是对的） |
| T2 | Flash 持久化 | **PASS** | `config_save` `s=0 saved=true`；回读 90000；version **1 → 2** |
| T3 | 重启后生效 | **PASS** | boot_seq **98 → 99**；回读仍 90000（Flash 真生效） |
| T4 | 版本冲突 | **PASS** | `expect_version=51`（实际 1）⇒ `s=3 e=7 m="version mismatch"`，RAM **未被污染**（回读仍 86400）；落盘后再用旧号（1 vs 2）**仍被拒** |
| T5 | 错误结果识别 | **PASS** | ① 类型错/未知键/死参数 ⇒ **400** + **零下行帧**（落在参数校验层）；② 未注册设备 ⇒ **403 `config_write_device_not_allowed`** + 零帧 ⇒ 判"未知/未执行"，**不当成功** |
| T6 | 取消重启语义 | **PASS** | `config_cancel_restart` ⇒ `s=0 "config auto-restart timer cancelled"`；等满 **330 s** 后 boot_seq **98 → 98**、值仍 90000（RAM 改动保留、重启未发生） |
| T7 | 失败恢复 | **PASS（可安全复现的两条）/ 🚫 NOT RUN（破坏性）** | 已验：版本冲突 ⇒ 拒绝 + RAM 未污染 + 未启动重启；取消重启 ⇒ 倒计时清零、未重启。**未做**：Flash 写失败、启动失败、非计划重启回滚、commit 卡 PENDING —— 需真机故障注入，本轮禁止向生产设备做 ⇒ **不阻塞阶段四** |
| R-1 | 还原 | **PASS** | 写回 86400 + 落盘；version **2 → 3** |
| F-1 | 最终取证 | **PASS** | `--verify-only` 复核：**值 86400 / version 3 / boot_seq 100** |

**踩过的坑（已修正，值得记住）**：F-1 一度显示 `boot_seq=98`，看起来像"没重启"。真实原因是 **R-1 后的重启是新 boot，而新 boot 的日志要等设备上行才入库**，那一刻 `/history/log?limit=1` 取到的仍是上一 boot 的行。脚本加了 `waitBootAbove()`（等新 boot 日志落地再采样）后复核得 **100**。⇒ **重启类判据必须等新 boot 数据落地，否则会得出假结论。**

### B-5 最终开关态

| 项 | 状态 |
|---|---|
| `config_write.enabled` | **false**（默认常量未翻，窗口已关） |
| `CONFIG_WRITE_*` 环境变量 | **已移除** |
| 生产写命令实测 | **403 `config_write_disabled`** + 零下行帧 |
| 设备 `time.sync_interval_sec` | **86400**（原值）；version 3；boot_seq 100 |

---

## C. 与阶段四的交界

- 任务书明确：**不要求先做完所有硬件故障演练再进阶段四**。⇒ B-7 的 NOT RUN **不阻塞**阶段四。
- 但 **R30（写配置全面开放）仍 🚫**：P4-3 破坏性演练未做 ⇒ 门禁保持关闭。阶段四**没有**也没有理由打开它（阶段四生产验收实测仍是 403）。

---

## D. 本轮未做 / 不得当作已完成

| 项 | 状态 | 理由 |
|---|---|---|
| P4-3 破坏性失败演练 | 🚫 NOT RUN | 需真机故障注入，本轮禁止 |
| 写配置全面开放（R30） | 🚫 | 以上为硬前置 |
| R15 破坏性回滚路径 | 🚫 | 同上 |
| 会话级页面自动化验收 | 🚫 | 生产 Turnstile ⇒ 自动化拿不到会话；R1 已由用户人工验收 |
| stepper / HX711 真机 | 🚫 | DRV8825 坏 / HX711 未接 |
| 多设备真实验收 | 🚫 | 生产仅 1 台真机（见阶段四报告） |

---

## E. 证据索引

| 主题 | 位置 |
|---|---|
| 生产只读验收脚本 | `Cloudflare_Assets/scripts/verify_stage3_prod.mjs`（49 PASS / 0 FAIL） |
| 真机写入脚本 | `Cloudflare_Assets/scripts/hw_config_write_test.mjs` |
| 真机证据（机器可读） | `Cloudflare_Assets/docs/evidence/config-write-realdevice-2026-10-25/result.json` |
| 真机证据（人读） | 同目录 `report.md` |
| 还原后复核 | 同目录 `final-recheck.json` |
| 写通道门禁实现 | `guo-feeder-api/src/capability-catalog.js`（`resolveConfigWrite` / `configWriteAllowed`）· `src/command-api.js`（403 落点） |
| 门禁测试 | `tests/api/config_write_command.test.mjs` 66 PASS（含 `[G]` 组 17 条服务端门禁） |
