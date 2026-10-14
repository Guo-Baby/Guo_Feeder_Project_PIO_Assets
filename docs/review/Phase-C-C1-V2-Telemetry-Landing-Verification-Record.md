# P3 · **`Phase C-C1` · `V-2`** —— **Telemetry Landing 遥测落库验证记录**

> ## 🧪 `HIL VERIFICATION RECORD` —— **真实设备端到端验证（telemetry → EMQX → Worker → D1）**
>
> | 项 | 值 |
> |---|---|
> | **性质** | **端到端（真实设备）验证记录** —— 闭合 `Phase C` 集成上线的 `V-2` 判据（`docs/architecture/P3-History-D1-Plan.md` §Acceptance） |
> | **被测实现** | 固件 **`04aca12`**（`feat(telemetry): implement Phase C-C1 HD-1 runtime telemetry enable gate`）· **`GF_TELEMETRY_ENABLED=1` 构建** |
> | **设备** | ESP32-S3 N16R8 · **`device_id = 288485896ce4`** · 端口 **`COM8`**（USB-Enhanced-SERIAL CH343） |
> | **云端链路** | EMQX `n302933b.ala.cn-hangzhou.emqxsl.cn:8883`（TLS）→ 规则 A `r-n302933b-cf0cf2` → 动作 `a-n302933b-126ec6` → Pages 中转 → Worker `POST /api/history/ingest` → D1 `telemetry_history` |
> | **上行观测** | 订阅 `guo_feeder/288485896ce4/#`（MQTT 侧）· `wrangler tail`（Worker 侧）· `wrangler d1 execute --remote`（D1 侧）· 三路同步取证 |
> | **执行日期** | **`2026-10-14`**（观测窗口 16:35:34–16:47:05 · 负窗口 16:51:49–16:58:34）· 记录整理同日 |
> | **上游** | 前置 `HD-1` 上板验证 → `docs/review/Phase-C-C1-HD1-Runtime-Verification-Record.md`（门开 + 开关行为 = `ACCEPTED`） |
> | **结论** | ✅ **`ACCEPTED`** —— **`V-2.1`…`V-2.6` 全 `PASS`**（真实遥测落库增量 **+8 行**） |
> | **零变更** | 本记录为**验证产物**；**未修改** firmware source / config schema / Worker / EMQX rule / migration；**未修复 `P-3`**；**未提交代码** |

---

## §1 目标与范围

**目标**：在**真实设备**上闭合 **telemetry 端到端落库链路** —— 即「当前设备开启 `mqtt.telemetry_enable=true` 后，其产生的遥测信封能否经 **EMQX 规则 A → Worker `/api/history/ingest` → D1 `telemetry_history`** 正确落库」，并逐项验证 `V-2` 判据。

**In Scope**：`V-2.1`（信封格式）· `V-2.2`（Worker `accepted`）· `V-2.3`（D1 落库）· `V-2.4`（`mqtt_messages` 无异常增长）· `V-2.5`（鉴权保持）· `V-2.6`（其他链路回归）。

**Out of Scope**：`V-3`（`/log` → `log_history`，见 `docs/review/Phase-C-C2-Log-History-End-to-End-Review.md`）· `P-3` 修复 · `HD-2` / `HD-3` · `weight` 通道（硬件延期 `R-2`）· 任何代码 / schema / EMQX / migration 改动。

---

## §2 验证环境与固件

| # | 项 | 实测值 |
|---|---|---|
| E-1 | 设备身份 | `[Identity] device_id=288485896ce4 (nvs ok)` · EMQX `clients` 侧 `dev_288485896ce4` `subscriptions=1`（订阅 `/down` qos1） |
| E-2 | 端口 | **`COM8`**（无占用进程；探测可开关） |
| E-3 | 固件 | **`04aca12`** · **`GF_TELEMETRY_ENABLED=1`**（编译门 ON）· `.pio/build/c1_gate_on/…/firmware.bin`（1,393,984 B） |
| E-4 | 网络 / 云 | MQTT 连接 EMQX Serverless（TLS 8883）· 设备在线 |
| E-5 | 时间 | SNTP 有效（信封 `ts` = 真实 Unix 秒，**非 0**） |
| E-6 | 传感可用性 | **`weight.enable=false`（HX711 未装）** ⇒ weight 通道取值恒 `0`（但信道**可产生**，见 §5）· climate 通道可用 |
| E-7 | 观测起点基线 | `config_query mqtt` ⇒ `telemetry_enable = false`（module **v9**）· D1 `telemetry_history = 7`（**HD-1 遗留行**，见 §4 注） |
| E-8 | 代理 | Worker 侧 `wrangler` 走代理 `http://127.0.0.1:7890`（`wrangler tail` / `d1 execute`） |

---

## §3 方法

### 3.1 执行序列（`真机方案 ①` · 零重烧）

1. **前置采集**：`config_query mqtt`（基线 `false`）+ D1 三表计数基线。
2. **开启**：MQTT 下发 `config_set mqtt telemetry_enable true`（新 `cmd_id` `v2set1-20261014`）→ `config_save`（`v2save1-20261014`）。
3. **观测 ≥10 min**：单进程订阅 `guo_feeder/288485896ce4/#` 逐帧落盘；同步 `wrangler tail`（Worker）+ 中途与终态 D1 回读。
4. **负窗口**：`config_set … false`（`v2setoff-20261014`）+ `config_save`（`v2saveoff-20261014`）→ 观测 **>5 min**（覆盖 1 个 climate 抽样点）确认零遥测。
5. **终态取证**：MQTT 全量日志 + `wrangler tail` 全量事件 + D1 增量行。

### 3.2 观测窗口长度依据

**climate 抽样兜底周期 = `TP_CLIMATE_PERIOD_MS` = 5 min**（`src/services/telemetry_policy.h`）· 产出器 **每轮 ≤1 条**。
⇒ 正例窗口取 **≥10 min**（覆盖 ≥2 个抽样点）；负例窗口取 **400 s**（覆盖 1 个抽样点），确保「零遥测」非「还没到点」。

### 3.3 运行器

- `.pio/hd1_run.py`（MQTT 订阅 + 文本计划执行器 · 语法 `MARK` / `PUB` / `WAIT`；位于 `.pio/`，属 gitignore，未入库）。
- 计划文件：`.pio/v2run/plan_v2.txt`（开 + 观测）· `.pio/v2run/plan_v2_off.txt`（关 + 负窗口）。

---

## §4 MQTT `/up` 证据

### 4.1 开关下发与回包（全部 `{"s":0,"e":0}`）

| 时刻 | 命令（新 `cmd_id`） | 回包关键字段 |
|---|---|---|
| 16:35:38 | `config_query mqtt`（`v2q1`） | `v=9` · **`telemetry_enable=false`** |
| 16:35:48 | `config_set mqtt telemetry_enable true`（`v2set1`） | `m:"field updated, restart required"` |
| 16:35:53 | `config_save`（`v2save1`） | `data:{"saved":true,"dirty":false}` ⇒ 设备重启（`online` 帧 16:36:10） |
| 16:46:55 | `config_query mqtt`（`v2q2`） | `v=10` · **`telemetry_enable=true`** |
| 16:51:46 | `config_set mqtt telemetry_enable false`（`v2setoff`） | `m:"field updated, restart required"` |
| 16:51:52 | `config_save`（`v2saveoff`） | `data:{"saved":true,"dirty":false}` ⇒ 设备重启（`online` 帧 16:52:09） |
| 16:58:34 | `config_query mqtt`（`v2q3`） | `v=11` · **`telemetry_enable=false`** |

> 注：`config_set` 回包措辞 `restart required` 只描述**落盘需求**；运行期已**即时生效**（`HD-1` 发现 ① / 两层语义）。

### 4.2 `guo_feeder/288485896ce4/up` 遥测信封（逐字摘录 · 共 10 条）

| # | 时刻 | 信封 | 归宿 |
|---|---|---|---|
| 1 | 16:35:48 | `{"ts":1791966947,"type":"climate.temperature","value":24.4,"seq":1,"schema_version":1}` | ✅ 落库 |
| 2 | 16:35:48 | `{"ts":1791966947,"type":"climate.humidity","value":49.6,"seq":2,"schema_version":1}` | ✅ 落库 |
| 3 | 16:35:48 | `{"ts":1791966947,"type":"weight.value","value":0,"seq":3,"schema_version":1}` | ✅ 落库 |
| 4 | 16:41:10 | `{"ts":1791967270,"type":"climate.temperature","value":null,"seq":1,"schema_version":1}` | ❌ **记录级拒收（`P-3`）** |
| 5 | 16:41:10 | `{"ts":1791967270,"type":"climate.humidity","value":null,"seq":2,"schema_version":1}` | ❌ **记录级拒收（`P-3`）** |
| 6 | 16:46:10 | `{"ts":1791967570,"type":"climate.temperature","value":24.4,"seq":3,"schema_version":1}` | ✅ 落库 |
| 7 | 16:46:10 | `{"ts":1791967570,"type":"climate.humidity","value":49.7,"seq":4,"schema_version":1}` | ✅ 落库 |
| 8 | 16:51:10 | `{"ts":1791967870,"type":"climate.temperature","value":24.4,"seq":5,"schema_version":1}` | ✅ 落库（窗口后、关闭前） |
| 9 | 16:51:10 | `{"ts":1791967870,"type":"climate.humidity","value":49.7,"seq":6,"schema_version":1}` | ✅ 落库 |
| 10 | 16:51:10 | `{"ts":1791967870,"type":"weight.value","value":0,"seq":7,"schema_version":1}` | ✅ 落库 |

> **V-2.1 ✅**：信封恒为 **5 字段** `{ts,type,value,seq,schema_version}`，**无** `device_id`（由 topic 承载）· **无** `source`；数值 = JSON number（`weight=0` 亦为 number）· 无效值 = JSON `null` · `ts` = 真实 Unix 秒（**非 0**）。

### 4.3 负窗口（关闭后 · 16:51:49–16:58:34 · 400 s）

- 窗口内 `/up` **遥测条数 = 0**（跨 1 个 5-min 抽样点）；仅出现 `/log` 信封与 `online` 帧。
- 终态 `config_query` ⇒ **`telemetry_enable=false`**（v11）⇒ 设备回**安全态**。

---

## §5 Worker `tail` 证据

> `wrangler tail --format json`（经代理 `127.0.0.1:7890`）· 覆盖 **≈20 min** · 共 **91 个事件**。

| 端点 | 次数 | 状态码 | 说明 |
|---|---|---|---|
| `POST /api/history/ingest` | **13** | **200 ×11** / **401 ×2** | 11×200 = 设备 10 条遥测 + 我方 1 次 null 回放；2×401 = 我方的无 key / 错 key 探针 |
| `POST /`（legacy `/api/webhook`） | **71** | 全 200 | 全为设备 `/log` 信封 + `/up` 命令回包（**遥测 0 次**） |
| `POST /api/presence/event` | **7** | 全 200 | 设备重启产生的上下线事件 |

- **V-2.2 ✅**：所有设备遥测 ingest 均返回 **200**；其中 2 条 `value:null` 由 Worker **记录级拒收**（`accepted` 不含）。
- **V-2.5 ✅**：**无 key / 错 key ⇒ 401**（鉴权未放宽）；正确 key ⇒ 200。设备侧请求**全部携带 `X-Webhook-Key`**。
- **归因核对**：事件 #2（`16:46:5x` 无 key 探针）与 #64（`16:49:30` 错 key 探针）为**我方**探针，**非设备流量异常**；事件 #63 = 我方 null 信封回放（200，被记录级拒收 ⇒ **未产生 D1 写入**）。

---

## §6 D1 `telemetry_history` 增量证据

### 6.1 计数变化

| 时刻 | `telemetry_history` | max id | `log_history` | `mqtt_messages` |
|---|---|---|---|---|
| 基线 16:33 | **7** | 27 | 0 | 3072 |
| 窗口末 16:47 | 12（**+5**） | 32 | 0 | 3123 |
| 终态 16:59 | **15（+8）** | 35 | 0 | 3162 |

### 6.2 新增 8 行逐条（`id > 27`）

| id | ts | type | value | seq | created_at |
|---|---|---|---|---|---|
| 28 | 1791966947 | `climate.temperature` | `"24.4"` | 1 | 1791966951 |
| 29 | 1791966947 | `climate.humidity` | `"49.6"` | 2 | 1791966951 |
| 30 | 1791966947 | `weight.value` | `"0"` | 3 | 1791966952 |
| 31 | 1791967570 | `climate.temperature` | `"24.4"` | 3 | 1791967571 |
| 32 | 1791967570 | `climate.humidity` | `"49.7"` | 4 | 1791967571 |
| 33 | 1791967870 | `climate.temperature` | `"24.4"` | 5 | 1791967870 |
| 34 | 1791967870 | `climate.humidity` | `"49.7"` | 6 | 1791967871 |
| 35 | 1791967870 | `weight.value` | `"0"` | 7 | 1791967871 |

- **V-2.3 ✅**：**+8 行**，与 8 条实值遥测逐条对应（2 条 `null` 被拒收）；`device_id`=topic 第 2 段 · `value` 存 **TEXT**（十进制字符串）· `source=NULL`（信封无 source · `K-7` 预期）· `schema_version=1` · `created_at` 由 D1 生成（≈ `ts`+1~4 s）。
- ⚠️ **基线更正**：`telemetry_history` 实测为 **7（非早期文档所述 0）** —— 系 **`HD-1` 上板验证（`2026-10-13`）的遗留行**（同设备 · `ts=179188xxxx` · 与 `HD-1` 记录逐字吻合）。⇒ **`V-2` 判据以「增量」为准。**

### 6.3 `mqtt_messages` 隔离核对（`V-2.4`）

| 项 | 值 |
|---|---|
| 增量归属（`device_id=288485896ce4, id>3050`） | `/log` **75 行** · `/up` **13 行**（命令回包）· **遥测 0 行** |
| 遥测泄漏检测（`raw_payload LIKE '%schema_version%' OR '%climate.%'`） | **`leak = 0`** |

- **V-2.4 ✅**：遥测**未泄漏**进 `mqtt_messages`；`mqtt_messages` 的 `+90`（3072→3162）增量**全部可归因**既有 legacy 路径（`/log` + `/up` 命令回包 + presence 旁证）。

---

## §7 判定汇总（`V-2.1` … `V-2.6`）

| 判据 | 结果 | 证据 |
|---|---|---|
| `V-2.1` 信封格式 | ✅ **PASS** | 5 字段 · 类型正确 · `ts≠0` · `schema_version=1` |
| `V-2.2` Worker `accepted` | ✅ **PASS** | `/api/history/ingest` 设备遥测全 **200**；`null` 记录级拒收 |
| `V-2.3` D1 落库 | ✅ **PASS** | **+8 行**，列值逐项符合（`value`=TEXT · `source=NULL`） |
| `V-2.4` `mqtt_messages` 无异常增长 | ✅ **PASS** | **`leak=0`**；增量可全部归因 legacy 路径 |
| `V-2.5` 鉴权保持 | ✅ **PASS** | 无/错 key ⇒ **401**；不放宽；设备请求均带 key |
| `V-2.6` 其他链路回归 | ✅ **PASS** | `/log` 持续落 `mqtt_messages` 且 200 · presence 7×200 · 命令链路 ack/result 正常 |
| 关闭复位 | ✅ **PASS** | 负窗口 **0 遥测** · `telemetry_enable=false`（v11） |

> **无明显 FAIL / BLOCKED。** 设备已恢复 `telemetry_enable=false` **安全态**。

---

## §8 发现项（**仅登记 · 不修复 · 不属本任务范围**）

### 发现 ①（`P-3` 复现）：NaN → JSON `null` 静默丢样

- **现象**：**重启后首批**遥测出现 `"value": null`（本次窗口 **2/10 条被拒**）。设备侧计发送成功，**云端记录级拒收** ⇒ 静默丢样。
- **根因**：`tele_fixed2()` 对 **NaN / 非有限值** 输出 JSON `null`；Worker `normalizeTelemetry()` 对 `value:null` 拒收（HTTP 仍 200）。
- **归属**：**既有缺陷**（`Phase C-C1` 审查 `P-3`），**非 `V-2` 引入**；**本任务不修复**，另行处置。
- **对 `V-2` 的影响**：**无** —— 判据取「增量 ≥1 且为非 null 值」，已由 id 28–35 满足。

### 发现 ②（`Phase C-C2` 缺口 `F-C2-*` 实时印证）：

- **现象**：设备 `/log` 信封（JSON · 含 `device_id`/`boot_seq`/`seq`/`ts`/`level`/`payload_base64`）**正在持续产生**，全部经 **规则 B** 落 `/api/webhook` → `mqtt_messages`；**`log_history` 恒 0**（窗口内 75+ 行 `/log` 全进错表）。
- **归属**：`Phase C-C2` 待裁决项 `HR-C2-1/2/3`（**EMQX 缺 `/log` 分流 · 规则配额 `4/4`**）。详见 `docs/review/Phase-C-C2-Log-History-End-to-End-Review.md`。
- **本任务不处理**（`V-3` 范畴）。

### 发现 ③（`BT-1` 现场可见）：

- **现象**：`/log` 信封出现 **`ts:0`** 且同一 `seq` 反复重投（例：`boot_seq=94, seq=108289` 连发多次 · 见 `mb`/观测日志）。
- **归属**：`BT-1`（`G-4 BLOCKED` 关联项：云端 `log_ack`）· `HD-3`（`ts=0` + `seq` 归零 ⇒ 幂等碰撞）。
- **本任务不处理**。

### 发现 ④（环境事实）：`weight` 通道取值恒 `0`

- `weight.enable=false`（HX711 未装）⇒ `weight.value` 恒 `0`；本次窗口 **观察到 weight 信道确实产生**（seq 3 / 7）。
- **不构成缺陷**；`weight` 通道完整端到端属**硬件延期节点 `R-2`**，本记录不涵盖。

---

## §9 结论

✅ **`V-2` 遥测落库端到端验证 = `ACCEPTED`（`V-2.1`…`V-2.6` 全 `PASS`）**

| 维度 | 结论 |
|---|---|
| **全链路** | 真实设备 telemetry **→ EMQX 规则 A → Worker `/api/history/ingest` → D1 `telemetry_history`** 逐段取证、**闭环成立**（D1 **+8 行**） |
| **零变更** | 无 firmware / config schema / Worker / EMQX / migration 改动；**未修 `P-3`**；**未提交代码** |
| **成本** | 唯一动作 = 真机 `config_set` 开关 + 观测 ⇒ **零重烧**（`HD-1` 运行时门收益实证） |
| **回归** | `/log` · `presence` · 命令链路均无回归；`mqtt_messages` **零遥测泄漏** |
| **遗留** | `P-3`（NaN→null 静默丢样）· `V-3`（`/log` 分流，`HR-C2-1/2/3` 待裁）· `HD-2` / `HD-3` · `weight` 硬件 `R-2` |

---

## §10 复现材料

| 用途 | 命令 / 路径 |
|---|---|
| 开启 + 观测（≈11.5 min） | `python .pio/hd1_run.py 288485896ce4 .pio/v2run/plan_v2.txt .pio/v2run/mqtt_v2.log` |
| 关闭 + 负窗口（≈7 min） | `python .pio/hd1_run.py 288485896ce4 .pio/v2run/plan_v2_off.txt .pio/v2run/mqtt_v2_off.log` |
| Worker tail | `wrangler tail --config guo-feeder-api/wrangler.toml --format json`（经代理 `127.0.0.1:7890`） |
| D1 查询 | `wrangler d1 execute guofeeder --remote --command "SELECT … FROM telemetry_history …"` |
| 原始日志（临时 · `.pio/v2run/` · 未入库） | `mqtt_v2.log` · `mqtt_v2_off.log` · `tail.jsonl` · `tail_events.txt` · `parse_mqtt.py` · `parse_tail.py` · `d1_*.json` · `plan_v2*.txt` |

---

*本记录于 **`2026-10-14`** 整理，为 **`Phase C-C1 · V-2`** 的**真实设备端到端落库验证记录**。*
*被测实现 = 固件 **`04aca12`**（`GF_TELEMETRY_ENABLED=1` 构建）· 设备 `288485896ce4` @ `COM8`。*
***本记录不含任何代码改动、配置 schema 变更或生产操作**；设备已恢复至 `telemetry_enable=false` 安全态。*
