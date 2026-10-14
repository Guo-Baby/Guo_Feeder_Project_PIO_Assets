# P3 · **`Phase C-C1` · `HD-1`** —— **Runtime Telemetry Enable Gate 运行时验证记录**

> ## 🧪 `HIL VERIFICATION RECORD` —— **真实设备上板验证**
>
> | 项 | 值 |
> |---|---|
> | **性质** | **运行时（上板）验证记录** —— 闭合 `HD-1` 实施规格 §10 的 `V-C1-12`…`V-C1-19` |
> | **被测实现** | 固件 **`04aca12`**（`feat(telemetry): implement Phase C-C1 HD-1 runtime telemetry enable gate`） |
> | **被测构建** | `GF_TELEMETRY_ENABLED=1`（`.pio/build/c1_gate_on/esp32-s3-devkitc-1/firmware.bin` · **1,393,984 B** · Flash 66.5 %） |
> | **设备** | ESP32-S3 N16R8 · **`device_id = 288485896ce4`** · 端口 **`COM8`**（USB-Enhanced-SERIAL CH343） |
> | **云端链路** | EMQX `n302933b.ala.cn-hangzhou.emqxsl.cn:8883`（TLS）→ topic `guo_feeder/288485896ce4/{up,down,log}` |
> | **上行观测** | 订阅 `guo_feeder/288485896ce4/#`（MQTT 侧），逐帧落盘 |
> | **执行日期** | **`2026-10-13`**（16:20–17:35）· 记录整理 `2026-10-14` |
> | **上游** | 实施规格 → `docs/review/Phase-C-C1-HD1-Runtime-Telemetry-Enable-Gate-Implementation-Specification.md`（§10 矩阵 · §15 实施回填） |
> | **结论** | ✅ **`ACCEPTED`** —— **7 PASS / 1 BLOCKED**（唯一阻塞为**验证能力缺口**，非功能缺陷） |
> | **零代码变更** | 本记录为**验证产物**；未修改 firmware source / config schema / `HD-2` / topic / cloud / D1 |

---

## §1 目标与范围

**目标**：在真实设备上验证 **Runtime Telemetry Enable Gate** 的行为 —— 即「**编译门 `GF_TELEMETRY_ENABLED=1` 的镜像**能否由 `mqtt.telemetry_enable` 在**运行期**控制是否发送 telemetry」。

**In Scope（仅这 8 项）**：`V-C1-12` · `V-C1-13` · `V-C1-14` · `V-C1-15` · `V-C1-16` · `V-C1-17` · `V-C1-18` · `V-C1-19`。
（`V-C1-11` 编译门关闭 / `V-C1-20` 资源复核已在**构建层**闭合，见实施规格 §15.2/§15.3。）

**Out of Scope**：`HD-2`（`interval_ms` 消费）· `HD-1e`（disabled 计数）· `P-3` 修复 · topic / Envelope / Worker / EMQX / D1 任何改动。

---

## §2 验证环境与固件

| # | 项 | 实测值 |
|---|---|---|
| E-1 | 设备身份 | `[Identity] device_id=288485896ce4 (nvs ok)` |
| E-2 | 配置加载 | `[CFG][I] init done, loaded 8/8 modules` |
| E-3 | 网络 / 云 | `WiFi connected` → `[Cloud] MQTT connected`（QoS1 · TLS） |
| E-4 | 时间 | SNTP 同步成功（`STATE_TIME_VALID` = true） |
| E-5 | 门禁 ON 特征 | 镜像内含 `climate.temperature` / `schema_version` 字符串（`grep -ac` = 1/1） |
| E-6 | 传感器可用性 | **`[Weight] Disabled by config (HX711 not installed)`** ⇒ weight 通道**不可用**；climate 通道可用（mi_thermo MAC `A4:C1:38:31:D4:17` · `allow_collect=true`） |

> ⚠️ **E-6 是本记录的判读前提**：telemetry v1 仅 3 条取值通道（`climate.temperature` / `climate.humidity` / `weight.value`）；**weight 全程无效** ⇒ 本轮**只有 climate 通道可产生上行**。所有正例证据均落在 climate；负例（零遥测）窗口**已由正例证明设备确实能发**，故**不是「发不出来」的假阴性**。

---

## §3 方法

### 3.1 烧录（app-only，保留设备配置）

```bash
python D:/platformIO/packages/tool-esptoolpy/esptool.py \
  --chip esp32s3 --port COM8 --baud 921600 \
  --before default_reset --after hard_reset \
  write_flash -z 0x10000 .pio/build/c1_gate_on/esp32-s3-devkitc-1/firmware.bin
# → Wrote 1393984 bytes (893558 compressed) · Hash of data verified.
```

- **不执行 `uploadfs`** ⇒ 设备端 `/config/*.json`（含 wifi / mqtt 凭据）**原样保留**。
- 构建与 `04aca12` 同源（工作树 clean · `HEAD = aab59c1` 含 `04aca12`）。

### 3.2 命令下发与观测（MQTT 单通道）

- 命令：下发 `{"c":"system","i":"<唯一>","p":{"o":"config_*",...}}` 到 `guo_feeder/288485896ce4/down`；设备回 **ack + result** 于 `/up`。
- 观测：单进程订阅 `guo_feeder/288485896ce4/#`，逐帧带时间戳落盘（含 telemetry 信封、命令回包、`/log`）。
- 运行器：`.pio/hd1_run.py`（MQTT 订阅 + 文本计划执行器；位于 `.pio/`，属 gitignore）。

### 3.3 观测窗口长度依据

**climate 抽样兜底周期 = `TP_CLIMATE_PERIOD_MS` = 5 min**（`src/services/telemetry_policy.h:33`）。
⇒ 每个「**期望零遥测**」窗口**必须 > 5 min**，否则无法排除「只是还没到抽样点」。
本记录所有负例窗口均 **≥ 320 s**（多数 350 s，窗口起点在重启后 ≈ 30–70 s），**覆盖至少一个抽样点**。

---

## §4 逐项结果（`V-C1-12` … `V-C1-19`）

| ID | 场景 | 结果 | 证据（摘要） |
|---|---|---|---|
| `V-C1-12` | 门开 + 开关 **false / 缺键** | ✅ **PASS** | ① 键**缺失**（初始态）：350 s 窗口 **0 遥测**；② **显式 `false`**：见 `V-C1-18` 窗口 **0 遥测** |
| `V-C1-13` | 门开 + 开关 **true** | ✅ **PASS** | `/up` 出现 5 字段信封：`climate.temperature`=24.6 · `climate.humidity`=53.4（`seq` 1/2 · `schema_version`=1） |
| `V-C1-14` | **配置缺失** | ✅ **PASS** | `config_query mqtt keys_only` 返回键表**不含** `telemetry_enable` ⇒ 350 s **0 遥测** |
| `V-C1-15` | **配置损坏** | ⛔ **BLOCKED** | 见 §7 —— 固件/命令层**无注入点** |
| `V-C1-16` | **重启持久化** | ✅ **PASS** | 硬复位（RTS 脉冲）后仍发送（`seq` 1/2）；`config_query` 返 `telemetry_enable = true` |
| `V-C1-17` | 未 `save` 的**临时值** | ✅ **PASS** | 不 `save` 即**立即生效**；≈5.5 min 后**自动** save + 重启（`online` 帧 17:05:08）⇒ 值持久 |
| `V-C1-18` | **关闭回退** | ✅ **PASS** | `set false` + `save` ⇒ 窗口 **0 遥测**（**免重烧**）；`config_query` 返 `false` |
| `V-C1-19` | **异常类型** | ✅ **PASS** | 删键后写**字符串** `"yes"` + `save` ⇒ **0 遥测**（**fail-closed**）；查询回显 `"yes"` |

> ★ **正负成对**：`V-C1-13`/`-16`/`-17` 证明「**该发时确实发**」；`V-C1-12`/`-14`/`-18`/`-19` 证明「**不该发时确实不发**」。二者缺一不可 —— 仅负例无法排除传感器不可用导致的假阴性。

---

## §5 实际配置状态变化（设备端 `mqtt` 模块）

| 阶段 | 操作（经 `cm`/MQTT） | `telemetry_enable` | 模块 version |
|---|---|---|---|
| 烧录后初始 | — | **不存在**（缺键） | v4 |
| `V-C1-13` | `config_set true` + `config_save` | `true`（bool） | v5 |
| `V-C1-18` | `config_set false` + `config_save` | `false`（bool） | v6 |
| `V-C1-19` | `config_delete` → `config_set "yes"` + `config_save` | `"yes"`（**string**） | v7 |
| `V-C1-17` | `config_delete` → `config_set true`（**无 save**）→ 自动保存 | `true`（bool） | v8 |
| **恢复安全态** | `config_set false` + `config_save` | **`false`** ✅ | v9 |

返回码：全部 `{"s":0,"e":0}`（执行成功）；`config_set` 首次成功时回 `"field updated, restart required"`（**该措辞只描述落盘需求**，见 §8 发现 ①）。

---

## §6 telemetry 上行证据（`guo_feeder/288485896ce4/up`）

**正例信封（逐字摘录）**

| 时刻 | 信封 | 归属阶段 |
|---|---|---|
| 16:27:59 | `{"ts":1791880079,"type":"climate.temperature","value":24.6,"seq":1,"schema_version":1}`<br>`{"ts":1791880079,"type":"climate.humidity","value":53.4,"seq":2,"schema_version":1}` | `V-C1-13`（`config_set` **同秒** · 见发现 ①） |
| 16:33:32 | `climate.temperature` `value:null` · `climate.humidity` 53.5 | `V-C1-13` save-重启后首帧（见发现 ②） |
| 16:41:30 | `climate.temperature`/`climate.humidity` 均 `value:null` | `V-C1-16` 硬复位后首帧（见发现 ②） |
| 16:59:50 | `24.6` / `53.7`（`seq` 1/2） | `V-C1-17` 未 `save` 即刻生效 |
| 17:04:51 | `24.6` / `53.7`（`seq` 3/4） | `V-C1-17` 抽样兜底（+5 min） |

**负例窗口（遥测条数 = 0）**

| 阶段 | 窗口 | 遥测数 |
|---|---|---|
| `V-C1-12` / `V-C1-14`（缺键） | 350 s | **0** |
| `V-C1-18`（显式 `false`，含 save+重启） | 350 s | **0** |
| `V-C1-19`（字符串 `"yes"`） | 350 s | **0** |
| 恢复 `false` 后 | 320 s | **0** |
| `V-C1-13`/`-16`/`-17` 同一时段 | — | **>0**（对照） |

---

## §7 `V-C1-15` BLOCKED 说明（**验证能力缺口，非功能缺陷**）

- **现状**：固件**未提供**任何「配置损坏」注入点 —— 无文件写入类命令、控制台无 `corrupt` 钩子（`grep` 确认 `src/` 无匹配），且**禁止 `uploadfs`**（L-3）⇒ **无法从外部损坏 `/config/mqtt.json`**。
- **该行为本身未被 HD-1 触碰**：「损坏 ⇒ `config_init()` 走 Active → Backup 回退 ⇒ 均失败则模块未加载 ⇒ getter 走默认值 `false`」属 **ConfigManager 既有语义**（实施规格 §4 / §8 E-3/E-4）。
- **等价覆盖**：HD-1 关心的后果（**模块/键不可用 ⇒ `false` ⇒ 不发送**）已由 **`V-C1-14`（缺键）** 上板验证。
- **处置建议**：① 接受「与 `V-C1-14` 等价覆盖」（**推荐**）；或 ② 后续新增**测试专用**损坏钩子后再补测（本轮约束禁止）。

---

## §8 发现项

### 发现 ①（**已并入规格 §1 / §5 / §7**）：运行期开关**即时生效**，不等待重启

- **现象**：`config_set mqtt telemetry_enable true` 与**首批遥测同秒**出现（16:27:59）；`V-C1-17` 未 `save` 时 ≈18 s 内即发；`V-C1-18` `set false` 后**立即**停止。
- **根因**：`HD-1c = ① 每轮现读` —— 判定位于 `telemetry_task()` **最前置**且每轮读 getter，`config_set` 改写的是 ConfigManager 的 **RAM 缓存** ⇒ **下一 loop 即生效**。
- **与冻结的关系**：`F-7`（"配置改动重启生效"）只约束**在 `init` 期一次性读配置**的模块；本产出器不受其约束。
- **动作**：实施规格 §1/§5/§7 已由「重启后生效」更正为**两层语义**（**运行期 RAM** / **持久化**）。
- **附加含义（正向收益）**：想「**马上**停/开始发送」只需 `config_set`（**免 `save`、免重启**）；想「**重启后仍保持**」才需 `config_save`。

### 发现 ②（**仅记录 · 不修复 · 不属 HD-1 范围**）：`P-3` —— NaN → JSON `null` 静默丢样

- **现象**：**重启后首批**遥测出现 `"value": null`（温度、湿度均出现）。
- **根因**：`tele_fixed2()` 对 **NaN / 非有限值** 输出 JSON **`null`** ⇒ 该信封虽由设备成功发出，但 Worker `history-ingest` 会做记录级校验并**拒收**（HTTP 仍可能 200）⇒ **设备侧计成功、云端未落库**（静默丢样）。
- **归属**：**既有缺陷**（`Phase C-C1` 审查 `P-3`），**非 HD-1 引入**；**不纳入 HD-1 代码范围**，另行处置。
- **对本次验证的影响**：**无** —— 门禁行为（发 / 不发）不依赖 `value` 取值；信封**确实发出**即证明门已打开。

### 发现 ③（环境事实）：weight 通道全程无遥测

- `weight.enable=false`（HX711 未装）⇒ `STATE_WEIGHT_VALUE` 无效 ⇒ weight 通道不产生。**不构成缺陷**，但决定本轮仅 climate 可观测（见 §2 E-6）。

---

## §9 恢复与最终设备状态

- **恢复动作**：`config_set mqtt telemetry_enable false` + `config_save` → 自动重启。
- **恢复验证**：安全窗口 **320 s 内 0 遥测** ✅ · `config_query` 返 **`false`**（v9）✅。
- **最终设备状态**：**`GF_TELEMETRY_ENABLED=1` 固件 + `telemetry_enable=false`** ⇒ **零上行（安全态）**。
  > ⚠️ 设备当前为**功能构建 + 开关关闭**，安全但**非默认镜像**（默认构建 = 编译门 OFF）。是否刷回默认镜像由人工裁决。

---

## §10 结论

✅ **`HD-1` 运行时验证 = `ACCEPTED`（7 PASS / 1 BLOCKED）**

| 维度 | 结论 |
|---|---|
| **功能** | 运行时开关按要求工作：缺键 / 非 bool / `false` ⇒ **一律不发送**；`true` ⇒ 正确上行 5 字段信封 |
| **fail-closed** | 4 条独立路径验证（缺键、显式 false、字符串类型、模块/键不可用）—— **均收敛到「不发送」** |
| **持久化** | `save` + 重启（含硬复位）后保持；漏 `save` 由 5 min 兜底自动保存 |
| **回退** | `set false` 即停（运行期）/ + `save` 跨重启保持 —— **免重烧**（本规格核心收益，已实证） |
| **代码修正需求** | **无**（唯一 BLOCKED 为验证能力缺口；发现 ① 属文档更正；发现 ② 属既有 `P-3`） |
| **遗留** | `V-C1-15` 建议按等价覆盖接受 · `HD-1e` / `HD-1f`（其余漂移）/ `HD-2` · `P-3` 另批 |

---

## §11 复现材料

| 用途 | 命令 / 路径 |
|---|---|
| 构建（门禁 ON） | `PLATFORMIO_BUILD_DIR=.pio/build/c1_gate_on PLATFORMIO_BUILD_FLAGS="-DGF_TELEMETRY_ENABLED=1" pio run` |
| 烧录 | `esptool.py --chip esp32s3 --port COM8 --baud 921600 write_flash -z 0x10000 <build>/esp32-s3-devkitc-1/firmware.bin` |
| 观测 + 计划执行 | `python .pio/hd1_run.py 288485896ce4 <plan.txt> <log>`（`.pio/` 内 · 计划语法 `WAIT` / `PUB` / `MARK`） |
| 关键命令 | `config_query`（`keys_only` / `module`+`key`）· `config_set` · `config_delete` · `config_save`（均 `c=system`） |
| 原始日志（临时） | `.pio/hd1_vc112.log` · `_vc113` · `_vc116` · `_vc117` · `_vc118` · `_vc119` · `_restore`（`.pio/` 内，未入库） |

---

*本记录于 **`2026-10-14`** 整理，为 **`Phase C-C1 · HD-1`** 的**运行时（上板）验证记录**。
被测实现 = 固件 **`04aca12`**（`GF_TELEMETRY_ENABLED=1` 构建）· 设备 `288485896ce4` @ `COM8`。
**本记录不含任何代码改动、配置 schema 变更或生产操作**；设备已恢复至 `telemetry_enable=false` 安全态。*
