# G-4 采样窗口终裁报告（2026-10-21 22:06）

> 门禁 `G-4`：`loop()` P99 / 内存（连续块 + 栈 HWM / DRAM 85%）长时稳定性验证。
> 本报告 = 2 h 30 min 远程采样窗口（19:36:41 – 22:06:04）的终裁，数据源为设备回帧
> （`system.loop` + `system.memwatch`，只读不 reset），非推断。

## 1. 窗口元数据

| 项 | 值 |
|---|---|
| 采集器 | `Cloudflare_Assets/scripts/g4_remote_collect.py --run --minutes 150 --interval 300` |
| 设备 | `288485896ce4` |
| 窗口 | 2026-10-21 19:36:41 → 22:06:04（实际 2 h 32 m 17 s，150 min + 收尾） |
| 计划/实得样本 | 31 / 29（缺 seq 2、3、9，见 §4） |
| 覆盖 uptime | 56,485 ms → 8,881,294 ms（设备在窗口开始前 62 s 由步骤 3 的 `config_save` 重启，窗口捕获干净新 boot） |
| 期间设备重启 | **0 次**（uptime 单调递增，无归零） |
| 固件 | 与 `94ba711c` 烧录版实质等价（此后 src 仅 `670ed16` loop_stats 预算常量 10→15 µs 一条非语义变更） |

## 2. 终裁五项判定

### 2.1 loop P99 —— ✅ **PASS**

| 指标 | 值 | 门禁 | 余量 |
|---|---|---|---|
| 窗口增量 Δn | **207,521,476** | ≥ 10,000 | 2 万倍 |
| P99 | **0–200 µs**（bucket 0） | < 20 ms | **100 倍** |
| P50 | 0–200 µs | — | — |
| mean | 42 µs | — | — |
| valid / reasons | `true` / `[]` | — | — |
| anomaly | **0** | 无 60 s 级停顿 | — |

`valid=true`、`invalid_bits=0`、`p99_in_overflow=false`、`p99_lt_gate=true`。
全窗口 29 个有效回帧的 P99 始终落在 bucket 0，无一次例外。

### 2.2 loopTask 栈 HWM —— ✅ **PASS**

| 指标 | 值 | 说明 |
|---|---|---|
| 栈 HWM（全程最小） | **9,740 B** | 口径 = 字节（`stack_unit: bytes`，`stack_unit_verified: true`） |
| 栈实际大小 | **16,384 B** | `platformio.ini` `-DARDUINO_LOOP_STACK_SIZE=16384`（非 Arduino 默认 8 KB） |
| 占用率 | **59.4%** | 剩余 6,644 B |

口径校验（防 8 KB 误判的算术排除）：若 9,740 为字数则 = 38,960 B > 16,384 B ⇒ 不可能；且 `stack_unit_verified=true` 由固件自证。

### 2.3 DRAM 连续块 / 碎片 —— ✅ **PASS**

| 指标 | 值（全程最小或终值） |
|---|---|
| `dram_largest_free_block` | **53,236 B**（全程最小，无下降趋势） |
| 当前 DRAM | free 63,812 / used 185,032 / total 248,844 |
| 碎片量 | free − largest = 10,576 B（16.6%），全程稳定 ⇒ 无碎片化趋势 |
| PSRAM 最大空闲块 | 7,864,308 B（`min_free_since_boot` 7,984,551 = 95.2% 空闲） |

### 2.4 DRAM 85% —— ✅ **PASS**

- 采样点峰值 `dram_used_pct_peak` = **74.44%**；
- 自 boot 最差 = (248,844 − 46,984) / 248,844 = **81.12% < 85%**
  （分母只取 `internal.total` = 248,844，遵循 S-2 铁律，不用 327,680）；
- `min_free_since_boot` 在窗口内无再创新低（终值 46,984 与窗口中期一致）。

### 2.5 综合

**G-4 = PASS**。三项待验项（loop P99 / 连续块+栈 HWM / DRAM 85%）全部取得有利证据，
余量分别为：P99 100 倍、栈 40.6% 空闲、DRAM 3.9 pp。

## 3. 窗口内新发现（不影响本次判定，记录备查）

### 3.1 OLED 同步 I2C 刷新 = 6.4 ms 周期性 loop 阻塞（既有设计）

直方图呈双峰：bucket 0（0–200 µs）2.088 亿 + **bucket 32（6.4–6.6 ms）15.6 万**。
时段追踪证明**不是启动瞬态**——全程稳定 **~18 次/秒**（boot 后 56 s 即存在，
窗口内 26 个采样区间的增量速率 15.8–18.5 次/秒，无趋势）。

归因（读码确认）：`src/app/oled.cpp` `oled_task()` 按 `oled.frame_delay`（默认 **50 ms**，
`data/config.json`）调 `oled_show_frame()` → `oled.sendBuffer()`，即 SSD1306 **同步** I2C
传输 1 KB（128×64×1bpp）。实测单次 6.4 ms，50 ms + 调度开销 ≈ 55.6 ms 周期，与 18 Hz 吻合。

判定：
- 这是**既有设计行为**，与本次启用/变更无关（`GF_TELEMETRY_ENABLED` 编译门关闭时亦然）；
- 单次 6.4 ms < 20 ms 门禁 ⇒ **不影响 G-4**；
- 但与「非阻塞」铁律存在张力，且位于 P99.9 分位（0.075%）。若后续要压 P99.9 或引入
  更严格的时序任务（如 WP-5 的控制环），需把 OLED 刷新移出 loopTask 或改异步/分帧传输。
  已记为待办观察项，**不**构成当前任何门禁的阻塞。

### 3.2 ≥ 25.6 ms 的极稀有 loop：窗口内 69 次（0.000033%）

- `over` 口径 = `loop_stats.h` `OVER_US = RANGE_US = 128 × 200 µs = 25.6 ms`（超出直方图
  量程的样本，已含在 n 内）；窗口内 7 → 76（+69）；
- 增量节奏规律：多数 5 min 区间 +2（≈ 每 125 s 一次）；
- `max_us` = **395,474 µs（395 ms）**。因 `loop_stats` 为 **boot 累积口径**
  （`scope: "uptime"`），无法定位其发生时刻；窗口首个样本已是 boot 后 56 s，
  故 395 ms 可能发生在窗口未覆盖的 boot 初期（如 WiFi 首连 / LittleFS 挂载），
  **无法证明亦无法排除**其在窗口内——如实记录，不做有利假设；
- 候选归因（未逐一定位，按可能性排序）：① WiFi/MQTT 网络栈同步写（`cloud_task` 队列
  publish 走同步 TCP write）；② `log_task()` 的 LittleFS 写/擦除；③ NTP 同步；
- `anomaly = 0` 证明全程**无 60 s 级 loop 停顿**（该阈值来自 `ANOMALY_MAX_US = 60 s`）。

## 4. 样本缺口归因（3 个，均已定位，非设备问题）

| seq | 现象 | 归因 |
|---|---|---|
| 2、3 | loop 帧缺失 | 我 19:46–19:54 的两次**错误 Pages 部署**（从父目录用位置参数部署导致 functions 丢失，`/api/*` 回落 index.html）⇒ 约 8 分钟回帧未达 D1。设备 uptime 连续证明期间无重启。已在 `docs/webhook-observe-2026-10-21.md` §6 诚实记录 |
| 9 | memwatch 帧缺失（loop 帧正常） | 单帧漏记（`mqtt_messages` 已知会漏记）。同区间 loop 数据完整，不影响任何判定 |

## 5. 对下游门禁的影响

- **WP-5**（原待 G-4）的前置条件已满足 ⇒ 可进入 Homie 上板阶段的设计评审；
- `C-3` 顺序不变：G-4 → WP-5 → Homie 上板 → C-3；
- 烧录 `GF_TELEMETRY_ENABLED=1` 镜像会重启设备、清零 loop 累积计数 ⇒ **G-4 终裁取本窗口数据，
  烧录后如需再验需重开窗口**（本次已 PASS，无需重开）；
- §3.1 OLED 阻塞不阻塞 WP-5，但若 WP-5 引入 < 6 ms 级时序要求，须先处理 OLED。

## 6. 数据存档

- 原始样本：`Cloudflare_Assets/.wrangler/g4_remote_20261021-193641/samples.json`（29 样本）
- 采集器 stdout 汇总（`loop_final_window` / 峰值 / 最小值全量）随本报告一并归档于
  `docs/evidence/G4-Window-Verdict-2026-10-21/g4_summary.txt`
