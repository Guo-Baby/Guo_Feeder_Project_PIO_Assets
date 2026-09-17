# LogManager P2 开发进度

> 用途：**会话中断后的续接基线**。每次完成一个模块接入必须更新本文件。
> 上游文档：`docs/LogManager-Integration-Guide.md`（接口规范）· `docs/P2_Log_Integration_Matrix.md`（模块矩阵）· `log模块历史/LogManager-P2接入准备审查0918.md`（前置审查）
> 更新规则：只记「已完成 / 当前基线 / 下一步 / 未决事项」，不记过程细节。

---

## Completed

| 阶段 | 内容 | commit | 状态 |
|---|---|---|---|
| P1.1–P1.5 | LogManager 核心（RAM 环 / Flash 段环 / Cloud / ACK·Retry / Replay / H2 / D1） | `5816b32`、`c29d9d5`、`d1ccdbe`、`65b4523` | ✅ 已提交 |
| 仓库整理 | 凭据文件取消跟踪 + 0917 报告入库 | `1d0b47c` | ✅ 已提交 |
| P2 前置 | 接入矩阵 + P2 接入准备审查（只出文档） | 见下方「待入库」 | ✅ 文档已生成 |
| **阶段 0** | 工作区确认（HEAD、`src/`/`test/` 无未提交改动） | — | ✅ |
| **阶段 1** | **P1.5 上板验证**（187 断言 → 167 OK / 20 MISS，全部 MISS 归因为用例预期缺陷） | `6a82fe1`（用例 op 名修正）、`789f53b`（验证报告） | ✅ 已提交 |
| **阶段 2** | **P2-A Storage bridge 接入**（json_storage 37 + file_storage 30 = 67 处 E/W 由静默变为可观测） | `930ed45` | ✅ 已提交 |
| **阶段 3** | 建立本进度文档 | `6485e25` | ✅ |
| **测试资产修正** | 按首轮归因修正 14 处预期值 + 回归器加 `<BOOT>` 宏 + 重录基线 | `test(log): fix P1.5 case expectations and re-baseline` | ✅ 已提交 |
| **收尾 · BT-1** | 云端 `log_ack`：**Worker 不在本仓库** ⇒ 仅记录接口要求，未改设备协议 | — | 📝 已记录 |
| **收尾 · holes=2** | 代码审查结论：**设计行为**（两个独立来源、相邻才合并）⇒ 更新用例而不改实现 | 见上方提交 | ✅ |
| **阶段 2-B** | **P2-B ConfigManager 接入**：`config_log_bridge` 桥接（关键词分类 + 10 s 去重 + SKIP 防重复）+ 2 个显式语义埋点（`CHANGE_APPLIED` / `SAVE_OK`）+ `cfg_hash32()` | `feat(log): integrate config manager logging` | ✅ 已提交 |
| **BT-9 修复** | **ACK 水位越过未确认补发记录**（at-least-once 破坏）：拆成 `s_cloud_acked_seq`（真实最大，仅观测）+ `s_cloud_gc_seq`（连续可回收，全部判定）+ `s_cloud_gc_floor`（backlog 下界钳制）；新增 `log_ack_gc_watermark()` 纯函数；补发/段回收/淘汰记账/**ACK 重复检测**全部改用连续水位 | `fix(log): prevent ack watermark bypass replay records` | ✅ 已提交 |
| **BT-9 回归** | 全量重跑 **195/195 = 100%，0 MISS**（首轮 167/187 → R1 187/189 → 195/195；**此后 P2-D 夹具加固 ⇒ 现基线 196/196**）；F2-B 两条原 KNOWN-FAIL 断言（`replay=8` / `qused=8`）**由 FAIL 转真实 PASS** | 同上 | ✅ |
| **阶段 2-C** | **P2-C WiFi 接入**（`src/wifi_module.cpp`，唯一改动文件）：5 个埋点（`CONNECT_START` / `CONNECTED` / `CONNECT_TIMEOUT` / `LOST` / `RECONNECT_TRY`）+ 失败循环节流（N 次记 1 次 **AND** ≥60s）+ `wifi_ssid_hash32()` | `feat(log): integrate wifi module logging` | ✅ 已提交 |
| **阶段 2-D** | **P2-D Cloud/MQTT 接入**（`src/cloud_manager.cpp`，唯一改动文件，**纯增量 +233/−0**）：5 个埋点（`CONNECTED` / `DISCONNECTED` / `SLEEP_ENTER` / `PUBLISH_FAIL` 聚合 / `CMD_EXEC_FAILED`）+ 发布失败计数聚合（60s 窗口） | `feat(log): integrate cloud manager logging` | ✅ 已提交 |

---

## Current baseline

```
HEAD:           见 `git log --oneline -1`（每次提交后更新本行）
分支:           wb
P1.5 设备侧:    ✅ 不再 BLOCKED（A–E 全部可跑段落已完成）
回归基线:       ✅ 196/196 = 100%，0 MISS（test/log_fix_tests.txt，P2-D 期间夹具加固后全量重跑）
                   首轮 167/187 → R1 187/189（2 MISS=BT-9）→ BT-9 修复后 195/195 → 本轮 196/196
                   （B 段 65 → 66：F2-B 增加一步 `logt cpush 1` + 一次 `ackauto`，见下方「回归夹具加固」）
合约测试:       ✅ 4/4 ALL PASS（含新增第 ⑧ 组 23 条 BT-9 断言，其中 3 条负向探针）
```

### 已接入 LogManager 的模块

| 模块 | 接入方式 | 事件覆盖 | 提交 |
|---|---|---|---|
| **Storage（json_storage / file_storage）** | `main.cpp` 回调桥接（`json_storage_log_bridge` / `file_storage_log_bridge`） | `LOG_STG_FS_UNAVAILABLE` / `ATOMIC_WRITE_FAILED` / `CRC_FAILED` / `TXN_RECOVERED` / `WRITE_VERIFY_FAILED` / `READ_FAILED` | P2-A |
| **Config（ConfigManager）** | `main.cpp` 桥接（`config_log_bridge`，**双路串口**）+ `config_manager.cpp` 内 2 处显式埋点 | `LOG_CFG_LOAD_DONE` / `MODULE_LOAD_FAILED` / `RECOVERED_FROM_BACKUP` / `COMMIT_FAILED_ROLLBACK` / `VERSION_REBUILT` / `WRITE_REJECTED` / `RESTART_TIMEOUT` + **`CHANGE_APPLIED`** / **`SAVE_OK`** | P2-B |
| **WiFi（`wifi_module.cpp`）** | 无回调接口 ⇒ **直接显式埋点**（5 处，全在既有分支内） | `LOG_WIFI_CONNECT_START` / `CONNECTED` / `CONNECT_TIMEOUT` / `LOST` / `RECONNECT_TRY` | P2-C |
| **Cloud（`cloud_manager.cpp`）** | 复用既有"回调置标志 → `cloud_task()` 消费"机制 ⇒ 埋点挂在**标志被消费处**（loop 上下文，边沿由单槽标志保证） | `LOG_MQTT_CONNECTED` / `DISCONNECTED` / `SLEEP_ENTER` / `PUBLISH_FAIL`（聚合）/ `CMD_EXEC_FAILED` | P2-D |
| 其余 7 个模块 | **未接入** | — | — |

未接入清单（按约定顺序）：**Time → Workflow → Weight → Valve → Dispense → BLE → Command/Event/OLED/Registry**

（未实现的冻结 EventId：`LOG_CFG_FACTORY_RESET`(0x0208) —— 代码中**不存在** factory reset 函数，无宿主，记为未实现。）

### P2-C 验证记录（2026-09-17，COM8，AP `wqs1`）

| EventId | 验证方式 | 结论 |
|---|---|---|
| `LOG_WIFI_CONNECT_START` | 真机开机 + MQTT 解码 | ✅ 通过（解码到 `SSID_HASH=0x7c7a4fc9 ATTEMPT_N=1 WAS=0(IDLE) STATE=1(CONNECTING)`） |
| `LOG_WIFI_CONNECTED` | 真机开机 + MQTT 解码 | ✅ 通过（解码到 `CONNECT_MS=1807 RSSI=-56 WAS=1 STATE=2`；`flash=0` 证明 INFO 不落盘） |
| `LOG_WIFI_CONNECT_TIMEOUT` | 真机（SSID 指向不存在的 AP）+ 补发回传解码 | ✅ 通过（解码到 `TIMEOUT_MS=30000 ATTEMPT_N=1 WAS=1 STATE=3`，WARN 落 Flash） |
| `LOG_WIFI_RECONNECT_TRY` | 同上 | ✅ 通过（解码到 `RETRY_N=1 ATTEMPT_N=1 WAS=3 STATE=1`；实测 4 轮循环只记 1 轮 ⇒ 节流生效） |
| **`LOG_WIFI_LOST`** | **见下方专项说明** | ⚠️ **部分通过**（代码路径审查通过 / 参数语义已确认 / **真机断 AP 验证未完成**） |

#### ⚠️ `LOG_WIFI_LOST` 专项说明

| 项 | 状态 |
|---|---|
| **代码路径审查** | ✅ **通过**。埋点位于 `wifi_task()` 的 `case WIFI_CONNECTED` 内、紧邻 `WiFi.status() != WL_CONNECTED` 判定之后，**未改变任何分支与 return 路径**；与既有 `Serial.println("WiFi lost")` / `event_push(EVENT_WIFI_DISCONNECTED)` 同处一个边沿块，语义一致 |
| **参数语义确认** | ✅ **已确认**：`CONNECTED_MS` = `millis() - wifi_connected_since`；`RSSI` = `state_get_int(STATE_WIFI_RSSI)`（**最后一次采样值**，因断连后 `WiFi.RSSI()` 只会返回 −100）；`CAUSE` = 断连瞬间 `WiFi.status()`（`WL_CONNECTION_LOST` / `WL_DISCONNECTED` / `WL_CONNECT_FAILED`…）；`WAS=2(CONNECTED)` / `STATE=3(DISCONNECTED)` |
| **真机断 AP 验证** | ❌ **未完成 —— 列为后续测试项** |
| 未完成原因 | 当前**没有任何钩子**可以强制断连：`WiFi.disconnect()` 在代码中无调用点；改 SSID 只影响下一次 `WiFi.begin()`，**不会**让已建立的连接掉线。本环境唯一办法是**物理关闭 AP** |
| 后续测试项 | ①（推荐）后续加一个测试钩子（如 `wifi force-lost`）；或 ② 人工关闭 AP 一次后复测。验收标准：MQTT 侧能解码到一条 level=WARN、带 `CONNECTED_MS`/`RSSI`/`CAUSE`/`WAS=2`/`STATE=3` 的记录 |

> 说明：本次**未**为 `LOST` 修改任何生产代码；`BT-10` / `BT-11` 按约定**不处理**，
> 仅在「未决 / 阻塞事项」中记录。

### P2-D 验证记录（2026-09-18，COM8，broker `emqxsl.cn:8883`）

**验证手段（可复用，已落 `.pio/p15run/`，不入库）**：
- `log_decode.py`：把 CBOR 批次解到**记录级**（`event_id` 在偏移 12 / u16 LE；参数从偏移 28 起，
  每项 `{id,type,u32le}` 6 B）⇒ 能直接读到 `event_id` 与每个 `ParamId` 的**真值**。
- `p2d_probe.py`：订阅 `guo_feeder/log` + **用设备自己的 `client_id` 再连一次 broker**（EMQX
  **session takeover** 会把设备踢下线，产生**真实的** `MQTT_EVENT_DISCONNECTED`）
  + 向 `guo_feeder/down` 发**不存在**的 action。**不改固件、不加钩子、不改协议。**

| EventId | 验证方式 | 结论 |
|---|---|---|
| `LOG_MQTT_CONNECTED` | 真机首连 + 重连（各 1 条） | ✅ 通过（`OUTBOX=450/710 WAS=0 STATE=1`；**一次连接一条**） |
| `LOG_MQTT_DISCONNECTED` | session takeover 真实踢下线 | ✅ 通过（`OUTBOX=0 RETRY_N=1 WAS=1 STATE=0`；设备串口同步出现 `MQTT disconnected outbox=0`） |
| `LOG_MQTT_CMD_EXEC_FAILED` | 下发 2 条不存在的 action | ✅ 通过（2 条命令 = 2 条记录，`CMD_ID` 为哈希；未误触发其他事件） |
| `LOG_MQTT_SLEEP_ENTER` | 代码路径审查 | ⚠️ **审查通过，真机未触发** —— 需"连续 30 次断连且期间一次都没连上"（每次成功连接会把 `mqtt_retry_count` 清零，配置 `retry_max=30`）。复测需临时改坏 MQTT `password` 或 EMQX 侧禁 client_id |
| `LOG_MQTT_PUBLISH_FAIL` | 代码路径审查 | ⚠️ **审查通过，真机未触发** —— 上层调用者都先自查 `mqtt_connected` ⇒ 离线时"根本不尝试发"；只有**竞态窗口**（刚断连但标志未更新）或 `enqueue` 返回 −1 才命中，时序敏感、不适合稳定回归 |

> 两项未触发的原因与复测方法详见 `docs/LogManager-Integration-Guide.md` §13.8。
> `LOG_CLOUD_FRAG_FAIL`(0x0706) **未埋点**：`cloud_publish_fragmented()` 全仓库无调用者（死代码），埋点不可达。
> 回归：埋点后全量重跑 **196/196 = 100%，0 MISS**（含夹具加固，见下）。

### ★ 回归夹具加固（P2-D 期间发现，**后续每个模块都会遇到**）

**现象**：P2-D 埋点后 B 段稳定 4 条 MISS（`replay=8` / `qused=8` / `qused=0` / `rarmed=0`）。

**根因**：F2-B 的 BT-9 场景靠"注入 live 批次 ACK ⇒ 队列排空 ⇒ 补发 sweep 推进"。
但注入的 ACK **只覆盖当时的在途批次**；P2-D 新增的 `LOG_MQTT_CONNECTED` 比 WiFi 记录
**晚约 4s 落地**（要等 TLS 握手）⇒ 队列里**必然**多出一条尾巴 ⇒ ACK 后 `used != 0`
⇒ 补发被挡住（**不是没补发，是观察不到**）。此前该尾巴"通常不存在"，故用例看起来是通的。

**加固做法（把时序依赖变成顺序无关）**：在注入 ACK 之前显式 `logt cpush 1` 造一条
确定性的 live 尾巴，再在 ACK 之后补一次 `logt ackauto 0` 排空它。这样
"Boot 期记录落在哪一批"不再影响结果，两种情形收敛到同一路径。
**断言强度未削弱**（`replay=8` 仍为精确值），只是把夹具变成确定性的。

**⚠️ 给后续模块的规矩**：

1. **任何新模块的埋点都会往同一条日志流里加记录**，`log_fix_tests.txt` 里的**绝对计数**
   断言（`evict_inf` / `qtotal` / `replay` / `fdrop` / `total` / `recs` …）都可能被扰动。
   接入前先想清楚"我的埋点会在 Boot 期产生几条"，并在回归里预留/显式构造状态。
2. **不要让用例依赖"某一批恰好包含全部记录"**：只要有一条记录在 `cloud_collect_batch()`
   **之后**入队，它就会成为下一批。夹具要么显式制造尾巴（推荐 `logt cpush`），
   要么显式排空。
3. **MQTT/网络抖动会污染回归**：实测 A 段首轮因 MQTT 写入超时（`Writing didn't complete:
   errno=119`）导致 loop 阻塞 ~8s、串口静默，命令未被及时处理 ⇒ 11 条假 MISS。
   **判据是串口出现 `MQTT error event` / `Writing didn't complete`**；遇到大面积 MISS
   先查这个，再怀疑固件（见 PIO 技能 §10 的排查顺序）。



---

## Next

### 下一步：**Time / RTC 接入**（P2-E）

Cloud 已接入完成（见上），回归 **196/196 全绿**。下一个按约定顺序是 **Time**（`src/time_manager.cpp`，879 行）：

1. 关键点（矩阵 §6.2）：`time_init()` 完成 / SNTP 同步成功 / SNTP 失败 / RTC 探测
   （`rtc_present`）/ 时间回退或跳变。
2. ⚠️ **SNTP 陷阱**（项目铁律）：IDF v4.4 的 SNTP 在 `COMPLETED` 后会**瞬时**回落到 `RESET`，
   "从未同步"也是 `RESET` ⇒ **禁止用 `status != IN_PROGRESS` 判成功**，必须用
   「callback 通知 + loop 延迟确认」。埋点要挂在**确认成功**的那一处，不要挂在 SNTP 回调里。
3. ⚠️ 上板 `RTC` 仍 `rtc_present=false` ⇒ RTC 相关埋点只能走"探测失败"分支，成功分支无法上板验证。
4. ⚠️ 时间跳变（SNTP 校正）会产生**时间倒退/跳跃** ⇒ 埋点不要依赖"时间单调"，
   用 `LOG_P_UNIX` / `LOG_P_DRIFT_MS` 表达（已有 ParamId）。
5. 提交主题建议：`feat(log): integrate time manager logging`

### 之后（严格一次一个模块）

`Workflow → Weight → Valve → Dispense → BLE → Command/Event/OLED/Registry`

⚠️ **Workflow 接入前必须先评审** `workflow_terminate()` 的 Critical Op release 收口路径
（项目铁律：release 不能放在会中途 return 的函数里）。详见
`log模块历史/LogManager-P2接入准备审查0918.md` §7 R-7。

---

## 未决 / 阻塞事项

| ID | 事项 | 阻塞什么 | 状态 |
|---|---|---|---|
| **BT-1** | **云端从未回送 `log_ack`**（`guo_feeder/down`）⇒ `acked_seq` 不前进 ⇒ Flash 段永不回收 | 线上回收闭环；P2 接入后日志量上升会加速暴露（496 条写满后开始淘汰未确认记录） | 🔴 **未解决**（外部依赖）。**Cloud Worker 代码不在本仓库**（本仓库只有 `cloud_protocol.md`）⇒ 按约定**只记录接口要求，不改设备侧协议**。接口要求见 §"BT-1 接口要求" |
| ~~**BT-9**~~ | ~~live ACK 越过旧未确认补发记录 ⇒ 永久跳过~~ | — | ✅ **已修复**（`fix(log): prevent ack watermark bypass replay records`）。方案＝**状态语义分离**：`acked_seq`（真实最大，仅观测）/ `gc_seq`（连续可回收，全部判定）/ `gc_floor`（钳制下界）。详见 Guide §11 与 Board-Test-Report 附录 R2 |
| **BT-10**（新） | 云队列溢出淘汰时**只给本 Boot 记录**登记空洞（`evict_boot == s_boot_seq`）⇒ 补发进来的上一 Boot 记录被淘汰后无空洞保护 | 复合低概率残余项（需同时：队列满且队首补发记录未发出 + 之后有更高 ACK + 触发段回收） | ⏳ **未修**（超出本次范围）。修法＝去掉该条件；代价＝空洞表压力上升（补发记录 seq 稀疏难合并，`LOG_HOLE_MAX=8` 可能溢出⇒退化为本 Boot 停止回收）。**F1 的 `holes=0` 断言正钉住当前行为** ⇒ 需单独评审 |
| **BT-11**（新，P2-C 期间发现） | `s_cloud_give_up_seq` 是**单点水位**：较新的批次先 give-up（实测 INFO 批次 seq 3073-3075 ⇒ `give_up_seq=3075`）后，seq 更小、仍在 Flash 等待补发的旧记录（seq≈1800）被 `log_ack_should_replay(seq <= give_up_seq)` 跳过 ⇒ **本 Boot 补发不到**（下次 Boot 归 0 后恢复，故**不丢数据**，属延迟） | 长离线后恢复时的补发**及时性** | ⏳ **未修**（P2-C 明令"不改 ACK/replay/GC 逻辑"）。与 BT-9 同源（单点水位越过旧记录），需在 LogManager 侧单独评审 |
| BT-4~BT-8 | ~~`test/log_fix_tests.txt` 中 20 条 MISS 的预期值修正~~ | P1.5 回归"全绿"基线 | ✅ **已修正 14 处**；回归现为 **196/196 = 100%，0 MISS** |
| P2-A 未验证项 | Storage bridge 的 **ERROR / CRITICAL 分支**与 **10 s 抑制窗口**无法用现有钩子触发 | 桥接层完整性 | ⏳ 待"结构化回调 + 故障注入"一并解决 |
| F2 决策 | 字符串参数是否改结构化回调（现为哈希/枚举化） | Storage 路径、Config key、Workflow id/action id 等 7 个 ParamId 的可用性 | ⏳ 已按"暂不扩 API"执行，未来可评审 |
| F6 基线 | P1.5 之前的既有回归集（P1.2 核心 66 / Flash 92 / F5 恢复 34 / F7-F9 43 / 交接 56）**未复跑** | P2 首个模块接入前的回归基线 | ⏳ 建议尽快补跑 |

---

## BT-1 接口要求（Worker 不在本仓库，仅记录要求）

云端需要在 `guo_feeder/down` 上实现 `log_ack` 回执。设备侧**已具备**接收能力并已在真机验证
（`[Cloud LOG] ack rx` 可被接收；**不带 `i` 也须被处理**；协议级消息必须旁路命令去重缓存）。

| 项 | 要求 |
|---|---|
| 命令名 | `"c": "log_ack"`（常量 `LOG_ACK_COMMAND`） |
| 参数键 | `"p": { "b": <boot_seq>, "f": <seq_from>, "t": <seq_to> }` |
| `i` 字段 | **可选**。不带 `i` 必须被正常处理（不得回 "Missing id"） |
| `b` | 必须回**设备上报的 boot_seq**（`b` 不匹配 ⇒ 设备 IGNORE） |
| `f` / `t` | 确认的 seq 闭区间；`f<=t` 且都非 0。覆盖整个批次 ⇒ ACCEPT；只覆盖前缀 ⇒ PARTIAL |
| **幂等义务** | 设备侧是 **at-least-once**（重启后 `acked_seq` 归 0，会重发）⇒ **云端必须按 `(device_id, boot_seq, seq)` 幂等**，重复 ACK 无副作用 |
| 触发时机 | 收到 `guo_feeder/log` 批次并**成功入库**后回一次 ACK（建议按批次回 `f/t` = 该批次的 seq 区间） |
| 不 ACK 的后果 | 设备走满 6 次超时 + 2/4/8/16/32 s 退避（≈152 s）后 give-up，**不推进水位、不删段** ⇒ 存储泄漏 + 重复补发 |

> ⚠️ 注意：**BT-9 未修之前，即使云端实现了 `log_ack`，旧未确认记录仍可能被越过**。
> 建议 BT-1 与 BT-9 一起解决（先修 BT-9 语义，再让云端闭环）。

---

## 本阶段对文档的产出

| 文档 | 内容 |
|---|---|
| `docs/LogManager-Integration-Guide.md` §9 | **新增**：Storage 回调桥接（注册方式 / 语义还原 / 参数映射 / 高频抑制 / 生命周期 / 上板结果） |
| `docs/LogManager-Integration-Guide.md` §10 | **新增**：ConfigManager 接入（与 Storage 的 3 点差异 / 关键词映射 / 2 个显式埋点 / 哈希 / 上板结果） |
| `docs/LogManager-Integration-Guide.md` **§11** | **新增**：**watermark 语义分离（FIX-BT9）** —— 根因 / 两个标量 / 为何 classify 也要改 / `gc_floor` 生命周期 / 刻意未改的部分 / 残余 BT-10 / 观测 / 验收 |
| `docs/LogManager-Integration-Guide.md` **§12** | **新增**：**WiFi 接入** —— 无回调接口⇒显式埋点 / 5 个埋点表 / 失败循环节流（N 次记 1 **AND** ≥60s）/ 为何重连不发 CONNECT_START / WAS·STATE 编码 / 参数语义 / 上板验证 / 已知限制 |
| `docs/LogManager-Integration-Guide.md` **§13** | **新增**：**Cloud/MQTT 接入** —— 复用既有单槽标志做边沿 / 5 个埋点表 / 发布失败计数聚合 / WAS·STATE 编码 / 刻意未做 / 上板验证（含 session takeover 触发技巧）/ 两项未触发的复测方法 |
| `docs/LogManager-P1.5-Board-Test-Report0918.md` | **新增**：P1.5 上板验证报告（环境 / 初始化 / MQTT / ACK / replay / F0–F8 / 已知问题）；**附录 R1**（用例修正与基线重录）；**附录 R2**（BT-9 分析·修复·验证 + 新基线 195/195） |
| `docs/P2_Log_Integration_Matrix.md` · `log模块历史/LogManager-P2接入准备审查0918.md` | 前置审查 —— 已随 `85c88b5` 入库 |

### 入库状态（已核实 `git ls-files`）

| 文件 | 说明 |
|---|---|
| `docs/P2_Log_Integration_Matrix.md` | ✅ 已入库（`85c88b5`） |
| `log模块历史/LogManager-P2接入准备审查0918.md` | ✅ 已入库（`85c88b5`） |
| `AI_RULES.md` | 已加「§9 Build/Test/Cleanup Rules」，但该文件**已被 `.gitignore` 忽略**（见 commit `1d0b47c`）⇒ 改动不会入库 |

---

*最后更新：2026-09-18（P2-C WiFi 接入完成并补登验证记录；进行中：P2-D Cloud/MQTT）*
