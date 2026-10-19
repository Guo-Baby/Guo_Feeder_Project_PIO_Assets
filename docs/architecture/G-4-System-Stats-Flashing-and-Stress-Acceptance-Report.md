# G-4 · `system.stats` 烧录与一次性压力验收报告

> 执行窗口：`2026-10-19T16:21Z – 2026-10-19T17:09Z`（本地 `2026-10-20 00:21 – 01:09`）
> 采样与判读窗口：`16:29:29Z`（启动）– `16:50:50Z`（静置终态），共 **1 280 s**
> 设备：`288485896ce4`（ESP32-S3 N16R8，COM8 / CH343）
> 授权：一次核验 + 一次烧录 + 必需重启 + 启动健康检查 + **一轮分级受控压力测试**
> 证据目录：`docs/evidence/G-4-Stats-2026-10-20/`（索引见该目录 `README.md`）
> 相关报告：`G-4-Root-Cause-Investigation-Report.md`（前置根因）、`BT-1-Acceptance-Report.md` §6.4/§6.5（门禁定义）

---

## 0. 结论先行

| 项 | 结果 |
|---|---|
| 源码可追溯 | ✅ 由归档 diff 复原，`apply` 后净增 **+158 行 / 5 文件**，与历史记录逐项一致 |
| 构建可复现 | ✅ RAM `132744 B (40.5%)`、Flash `1402305 B (66.9%)`、`firmware.bin` **+1872 B**，三项与历史记录完全吻合 |
| **原始构建产物** | ⚠️ **未留存**（隔离 worktree 已删除）⇒ 本次烧录的是**可复现重建**的固件，非历史原件。详见 §A-2 披露 |
| 烧录 | ✅ 一次成功，`Hash of data verified`；`boot_seq 21 → 22`，**恰好一次**重启，无额外复位 |
| 启动健康 | ✅ `boot validated` / WiFi / MQTT / 订阅 / 命令 / 日志上行 / ACK / `system.stats` 全部正常 |
| 压力测试 | ✅ 四级 105 条只读命令全部被设备受理（**105/105 ACK**）；零崩溃、零复位、零丢弃 |
| **G-4 门禁** | ❌ **不能解锁**：`drop_unacked` / `hovf` / 段删除频率 **PASS**；内部 DRAM 峰值 **FAIL**（77.16% vs 阈值 70%，且**基线就已 77.08%**）；`loop()` P99 **NOT VERIFIED**（无埋点） |
| 状态维持 | `BT-1 = CLOSED` · `G-4 = BLOCKED` · `WP-5 = 未解锁` · `STOP-R1 = 无解除文件` |

**不得由本报告推出**："测试期间没崩 ⇒ 不存在溢出风险"。本次只覆盖 ~21 分钟、单一 boot 周期、105 条只读命令、单一负载形态。

---

## A. 烧录前核验与证据封存

### A-1 封存对象（时间戳 `2026-10-19T16:21:40Z`）

| 项 | 值 |
|---|---|
| 固件仓 `wb` | `7f6b1fe03d77b666f4894305f9790f8ee494e848`（`5 M + 12 ??`，均为文档类历史改动） |
| 云端仓 `main` | `de4932594083a4fc26212fa88721832c9ea5d9ba` |
| 消息仓 `main` | `ec2364fa60d1ce10849fb8a4f9546e94dd555100` |
| 设备在跑固件 | `firmware.bin` 1 400 800 B · `sha256=a7ad2d24…efd49be`（已备份为 `pre-flash-firmware.bin`） |
| 设备 boot | `boot_seq=21`，`log_history` 最新 `seq=5215`（`16:07:17Z`） |
| watermark | `max_boot_seq=21` · `epoch=2` · `prev_epoch_max=117` · `rewind_count=0` · `obs_count=498` · `status=open` · `last_rebase_id=rb-20261019-0001` |
| EMQX | `connected=true` · `connected_at=2026-10-18T20:21:42Z` · `recv_cnt=1547` · `send_cnt=1585` · `subscriptions_cnt=1` · `mqueue_dropped=0` |
| G-4 方案 A 原始材料 | 报告 `G-4-Uplink-Load-Assessment-Report.md`、证据目录 `docs/evidence/G-4-2026-10-19/`、G4-A1/A2 结论（`G-4-Root-Cause-Investigation-Report.md`）均为只读引用，本次未改动 |

### A-2 源码与构建产物核验（⚠️ 含一处必须披露的缺口）

**源码：可复原。** `system.stats` 的完整差异以补丁形式归档于
`docs/evidence/G-4-RootCause-2026-10-19/p2stats-implementation.diff`（8 901 B，`sha256=d9b899ab…2bf3baa`）。
本轮 `git apply --check` 对 `7f6b1fe` 工作区**干净通过**，应用后：

```
src/log/log_manager.cpp        18 +    0 -
src/log/log_manager.h          14 +    0 -
src/services/command_manager.cpp 22 +  0 -
src/services/system_command.cpp  89 +  0 -
src/services/system_command.h    15 +  0 -
5 files changed, 158 insertions(+)          ← 与历史记录「+158 行 / 5 文件」逐字节一致
```

已提交为 `82123a3b7b03dc2b8a6a4232c27e49c5bba6a647`（**仅代码，未动任何文档**）。

**构建产物：原件缺失，以"可复现重建"替代。** 隔离 worktree `D:\Guo_Feeder_Project\.p2stats`
已不存在（`ls D:/Guo_Feeder_Project/` 无此项），历史 `firmware.bin` **未归档** ⇒
**无法"取回"当时的二进制**。处置：以归档 diff + 同一基线 commit + 同一 `platformio.ini` 重建，并用三项**互相独立**的指标验证等价：

| 指标 | 历史记录 | 本次重建 | 判定 |
|---|---|---|---|
| `RAM`（`.dram0.data+.bss`） | 132 744 B（40.5%） | **132 744 B（40.5%）** | 完全一致 |
| `Flash`（ELF text+data） | 1 402 305 B（66.9%） | **1 402 305 B（66.9%）** | 完全一致 |
| `firmware.bin` 体积 | 1400800 + **1872** = 1 402 672 | **1 402 672 B** | 完全一致（+0.1337%） |
| 编译告警/错误 | 0 | `grep -c "error:"` = **0** | 一致 |

新符号在二进制中的正/负对照（`grep -ac`，本沙箱无 `strings`）：
`hole_overflow` / `seg_evicted_unacked` / `drop_unacked` 在**新固件各命中 1**，在**烧录前固件命中 0**。

> **披露（原文必须保留）**：本次烧录的是"由归档源码差异确定性重建、并与历史三项指标逐项吻合"的固件。
> 授权文件要求"源码、构建配置、固件文件和 SHA-256 可追溯且相互匹配"——**源码与配置可追溯、构建可复现成立**；
> **"取回历史原件"不成立**（原件已在删除 worktree 时丢失）。本次判定为**不触发停止条件**，
> 但此缺口登记为 `S-2`（证据管理缺陷：构建产物未随 diff 一起归档），后续凡"隔离 worktree 出固件"必须同时归档 `.bin` + `sha256`。

**本次烧录固件**：1 402 672 B · `sha256=a2c8a0388e1e4b621a0e2e7d28a4b2269e1d84f96044211ace96532e61040b4f`

### A-3 接口设计审查（字段 / 生命周期 / 临界区 / `available:false`）

| 审查点 | 实现事实 | 评价 |
|---|---|---|
| 快照一致性 | `log_stats_get()`：`memset(&out,0)` → `!s_ready` 即 `return false` → **单个** `portENTER_CRITICAL(&s_mux)` 内 `out = s_stats`（纯 POD）→ 退出 | ✅ 字段自洽，无撕裂；锁段极短；不产生 Log（不递归）、不分配堆 |
| 计数生命周期 | `s_stats` 为 `log_manager.cpp` 内 `static LogStats`，**进程启动零初始化**；`log_stats_reset()` 存在但**无命令暴露** | ✅ 语义 = **本 boot 周期累计**；烧录重启后归零（故 `minimum_free` 与计数都不可跨 boot 比较） |
| 持久量 vs 易失量 | `boot_seq` / `flash.*` 来自 meta + 段扫描（持久）；`queue.*` / `counters.*` 为 RAM 现值 | ✅ 已在报告中分别标注 |
| `available:false` 结构 | 仍输出 `available:false` / `uptime_ms` / `queue` / `boot_seq` / `flash`，**`counters` 键整段缺席** | ✅ 结构上杜绝"不可用"被误读为"全零"；调用方须以**键存在性**判可用 |
| 只读性 | 不写配置、不触发硬件、不重启、不分配堆；栈开销 ≈ `LogStats(≈160 B)+LogCloudInfo(≈60 B)+LogFlashInfo(≈50 B)` | ✅ 符合"只读查询"要求 |

**发现的接口缺陷（新登记 `S-1`，观测级）**：
`counters.cloud_records_sent` **全仓库无自增点**（仅 `cloud_batch_sent` 在 `log_manager.cpp:3662` 自增），
实测 `cloud_batch_sent=162` 而 `cloud_records_sent=0` 恒成立 ⇒ 该字段**恒为 0**，
极易被读成"一条记录都没发出去"。本次不修改（授权禁止改 `system.stats` 之外/之内的功能），
**判读时明确忽略该字段**，并建议后续"接线或删除并注明"。

### A-4 门禁五项的可观测性逐项确认（★ 不得假设接口已全覆盖）

| `GATE-WP5` ② 指标 | 本接口能否直接测 | 依据 / 缺口 |
|---|---|---|
| `loop()` P99（劣化 **<20%**） | ❌ **不能** | 固件**无任何 `loop()` 计时埋点**；`uptime_ms` 只是 `millis()`，不构成延时分布。⇒ 本次保留 `NOT VERIFIED` |
| 内部 DRAM **峰值**（**<70%**） | ✅ **能** | `system.memory` → `internal.{total,free,used,largest_free_block,minimum_free}`。`minimum_free = ESP.getMinFreeHeap()` 是**分配器维护的单调历史最低值**，不依赖采样 ⇒ **峰值 = `total − minimum_free`**（本项不在 `system.stats` 内，需并行发 `system.memory`） |
| `drop_unacked`（**=0**） | ✅ **能** | `counters.drop_unacked`（= `cloud_flash_drop`）；另有 `counters.seg_evicted_unacked` 单列段环淘汰来源 |
| `hovf`（**=0**） | ✅ **能** | `queue.hole_overflow` |
| 段删除频率（**≤1/10min**） | ✅ **能** | `counters.seg_deleted`（= `flash_segment_deleted`），两次快照差分 / 时间窗 |

> 结论：`system.stats` 补齐了 4 项中的 **3 项**（`drop_unacked` / `hovf` / 段删除），
> 加上既有的 `system.memory` 使 **4/5 可测**；`loop()` P99 **仍是结构性缺口**，
> 修它需要新增 loop 计时埋点（超出本次授权）。

---

## B. 烧录与启动健康检查

### B-1 烧录（`16:29:0xZ`）

```
esptool --chip esp32s3 --port COM8 --baud 921600 --before default_reset --after no_reset \
        write_flash -z 0x10000 firmware.bin          # 只写 app 分区：不碰 nvs / LittleFS
Wrote 1402672 bytes (898354 compressed) at 0x00010000 in 15.5 seconds
Hash of data verified.
Staying in bootloader.
```

- 用 `--after no_reset` 是**刻意的**：把"启动"这一步交给串口会话触发，从而**整场只发生一次复位**（不做预防性复位）。
- 只写 `0x10000` ⇒ NVS（`gfid`/`gfcred`）与 LittleFS（设备端 `config/`）**未被触碰**；下方启动日志实证配置未回滚。

### B-2 唯一一次重启与启动日志（`16:29:29Z`）

```
ESP-ROM:esp32s3-20210327
rst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)
[Identity] device_id=288485896ce4 (nvs ok)
[Log] recovery=NVS boot_hw=21 -> boot_seq=22 reserved=5376 corrupt=1 (nvs=1 slot=1 legacy=0 seg_max=0)
[Log] seq: boot_seq=22 base=5377 limit=5632 reserved=5632 reliable=1 corrupt=1 src=0 failclosed=0
[CFG][I] boot validated
WiFi connected
[Cloud] MQTT client_id=dev_288485896ce4
[Cloud] MQTT subscribe=guo_feeder/288485896ce4/down
[Cloud] MQTT connected / MQTT subscribed msg_id=12160
{"cmd":"system","id":"online","src":"device"}
[Time] SNTP sync OK (2026-10-20 00:29:37)
```

| 检查项 | 结果 |
|---|---|
| `boot_seq` | **22**（21 → 22，**与预期一致**；未做 rebase、未改云端数据） |
| 序号连续性 | `base=5377`（上一周期末 `5215`）⇒ 区间不重叠、不回退；`seq_reliable=1`、`failclosed=0`、无隔离 |
| 配置代际 | `boot validated` ⇒ **未发生配置回滚**；`client_id` / `subscribe` / `publish` / `log` topic 均为 V3（`guo_feeder/<id>/…`） |
| 联网 / MQTT | WiFi connected → MQTT connected → subscribe（`subscriptions_cnt=1`，EMQX 侧确认 `connected_at=16:29:35Z`） |
| 命令响应 | `system.stats` / `system.memory` 均返回 **ack + result 双帧**，`data` 在顶层 |
| 日志上行 | `[Cloud LOG] OK topic=…/log` + 云端 `log_ack` → 设备 `ack ok covered=1/1 acked=<seq> gc=… r=2` |
| 崩溃/看门狗 | 全程 `ESP-ROM` 出现 **1 次**（即本次烧录启动）；`panic` / `Guru Meditation` / `abort()` **0 次** |
| **固件版本识别** | ★ 设备**不打印版本串**（仅 `ESP-ROM Build:Mar 27 2021`）⇒ 版本只能由三件事共同确定：① 烧录日志 `Hash of data verified`（1 402 672 B）；② 本地 `sha256=a2c8a038…`；③ **`system.stats` 能返回 `available:true`** —— 该指令在上一版固件中**不存在**，旧固件必回 `unknown`，故"能应答"本身就是"新固件在跑"的行为证据 |

**与历史启动的对照（排除"新增故障"）**：`/littlefs/log/*.log does not exist, no permits for creation`
与 `[Log] RAM ring ready … (boot_seq=1)` 两处"刺眼"输出，在 `2026-10-19 04:10` 的 BT-1 启动日志
（`.pio/hd3_b3_boot.log` 行 18–29）中**完全同样出现** ⇒ **既有行为，非本次固件引入**。
（`boot_seq=1` 是**环初始化时的初值**，随后由 `recovery=NVS` 校正为 22。）

### B-3 压力测试前基线（`T0`，`uptime=158 s`）

| 项 | 值 |
|---|---|
| `queue` | `used=0` · `capacity=128` · `inflight=0` · `acked_seq=5387` · `hole_overflow=0` |
| `counters` | `emit_total=11` · `ring_drop=0` · `drop_overflow=0` · **`drop_unacked=0`** · `seg_evicted_unacked=0` · `cloud_batch_sent=36` · `cloud_retx=1` · `cloud_ack_ok=35` · `cloud_ack_timeout=1` · `cloud_ack_lost=0` · `replay_records=24` · `give_up=0` · **`seg_deleted=0`** |
| `internal` | `total=251 060` · `free=66 648` · `used=184 412`（**73.45%**）· `largest_free_block=51 188` · `minimum_free=57 544` ⇒ **峰值占用 77.08%** |
| `external`(PSRAM) | `total=8 385 719` · `free=7 993 863` · `used=391 856`（4.67%）· `minimum_free=7 984 551` |

---

## C. 受控压力测试（四级，逐级决策）

方法：复用**已验过**的只读 `system.config_query valve`（`.pio/g4_burst.py`，每条命令 ID 唯一以避免 30 s 去重），
按固定间隔**异步**发起形成真实突发；每级完成后立即做健康检查 + `system.stats` + `system.memory` 快照，
再决定是否进入下一级。串口全程**单会话**捕获（`.pio/g4s_serial.py`），不重开端口（避免额外复位）。

| 级别 | 目标条数 | **实际投递** | 时间窗（UTC） | 设备 `UP-ACK` | `emit` Δ | `retx` Δ | `ack_timeout` Δ |
|---|---|---|---|---|---|---|---|
| 基线 T0 | — | — | `16:30:03` | — | — | 0 | 0 |
| L1 | 10 | **10** | `16:32:25.6–16:32:34.6` | **10/10** | +12 | 0 | 0 |
| L2 | 20 | **20** | `16:33:51.5–16:34:10.5` | **20/20** | +22 | +2 | +2 |
| L3（方案 A 参照量） | 35 | **35** | `16:35:39.1–16:36:13.2` | **35/35** | +37 | +3 | +3 |
| L4 | 50 | **40** ⚠️ | `16:39:35.1–16:40:14.4` | **40/40** | +42 | +3 | +3 |
| 静置期 `+435 s` | — | — | `16:50:50Z`（`uptime=1 280 s`） | — | +2 | **0** | **0** |

> ⚠️ **L4 的实际载荷是 40 条，不是 50 条**：10 条并发请求被**本机代理**以
> `curl (35) schannel: failed to receive handshake, SSL/TLS connection failed` 拦下，**命令从未到达云端**
> （`burst-L4.json` 逐条可查：10 条 `http=0` 且 `elapsed≈5.18 s`）。
> 这是**测试端瓶颈**（单本地代理并发 TLS 握手），**不是设备或 Worker 故障**；设备侧 `UP-ACK=40` 与实际投递数**精确相等**。
> 未重发这 10 条（避免为凑数追加写入）；L4 仍比方案 A 参照量高 **14%**。

### C-1 内存（`system.memory.internal`）

| 阶段 | `free` | `minimum_free` | `largest_free_block` | **峰值占用** `(total−min)/total` |
|---|---|---|---|---|
| T0（158 s） | 66 648 | 57 544 | 51 188 | **77.08%** |
| L1 后（244 s） | 66 700 | 57 544 | 51 188 | 77.08% |
| L2 后（349 s） | 64 700（−2 000） | 57 544 | 51 188 | 77.08% |
| L3 后（504 s） | 64 732 | **57 356（−188，新低）** | 51 188 | **77.15%** |
| L4 后（845 s） | **66 956（已回补）** | 57 356 | 51 188 | 77.15% |
| 静置后（1 281 s） | **66 960** | 57 356 | 51 188 | 77.15% |

读法（必须同时看三列，只看 `free` 会误判）：
- `free` 的 −2 000 B 是**突发期瞬时**下探，L4 后**完全回补**（甚至高于基线 +312 B）；
- `minimum_free` 全程只被压低 **188 B**，对应峰值占用 **+0.09 个百分点**；
- `largest_free_block` **自启动起 51 188 B 未变** ⇒ **无碎片化趋势**；
- PSRAM 三项（total/free/minimum）**逐字节未变**。
- ⚠️ 该 `minimum_free` **属于 boot 22 这一个周期**，不可与上一周期（`61 124 → 43 168`）直接比较，更不能据此宣称"此前内存问题已解决"。

### C-2 队列 / 丢弃 / ACK（全部来自 `system.stats`）

| 阶段 | `queue.used` | `inflight` | `hole_overflow` | `drop_unacked` | `seg_evicted_unacked` | `drop_overflow` | `ring_drop` | `ack_lost` | `give_up` | `seg_deleted` | `self_degraded` |
|---|---|---|---|---|---|---|---|---|---|---|---|
| T0 | 0 | 0 | **0** | **0** | 0 | 0 | 0 | 0 | 0 | **0** | 0 |
| L1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| L2 | 3 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| L3 | 10 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| L4 | **0** | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 静置（1 281 s） | **0** | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | **0** | 0 |

**记账恒等式（本次最强的一致性判据）**：

```
acked_seq = 5377 + emit_total − 1
静置期：5 506 = 5 377 + 130 − 1   ✅  逐次快照均成立
```
⇒ 本 boot 周期共产生 **130 条**记录，**130 条全部被云端 ACK 覆盖**（`ack_lost=0`、`give_up=0`、`drop_*` 全 0）。

**排空耗时**：`queue.used` 峰值仅 **10/128**（L3 采样点，`t=504 s`），在**下一个采样点（`t=593 s`，+89 s）已归 0**；
L4 采样点（`t=845 s`）已为 0。即**四级突发的积压始终远低于 128 槽容量，且均在两个采样点内排空**。
全程 `cloud_batch_sent=162`、`cloud_ack_ok=154`、`cloud_retx=9`、`cloud_ack_timeout=9`、`ack_ignored=7`。

**ACK 重试明细**（串口原文，共 9 次，**全部止步于 `retry=2/5`**）：

```
[Log Cloud] ack timeout retry=1/5 backoff=2000ms     ← 启动期，1 次
[Log Cloud] ack timeout retry=1/5  →  2/5 backoff=4000ms   ← L2，2 次
[Log Cloud] ack timeout retry=1/5  →  2/5                  ← L3，3 次
[Log Cloud] ack timeout retry=1/5  →  2/5                  ← L4，3 次
```
⇒ 每次超时后**均被后续 `ack ok covered=1/1` 收尾**，**从未触及 `5/5` 上限**，且**静置期 `retx` 冻结在 9（Δ=0）**
⇒ **重发阶梯会终止**，不存在"后台持续重发"。

### C-3 交叉核对（串口 / 设备计数 / 云端三方，不以 `mqtt_messages` 为唯一证据）

| 维度 | 数值 | 说明 |
|---|---|---|
| 实际投递命令 | **105**（10+20+35+40） | `burst-L1..L4.json` 中 `http=200` 计数 |
| 设备侧 `UP-ACK` | **105 / 105** | 串口 `[Cloud] UP-ACK id=g4L…` 逐条可数（L1 10 / L2 20 / L3 35 / L4 40） |
| 设备侧结果帧 | **105 / 105** | 串口 `[Cloud UP JSON] {"s":0,…"data":…}` |
| 设备侧记录 ACK 覆盖 | **132 / 132** | `acked_seq = 5377 + emit_total − 1`（快照时刻 130/130；快照自身产生的 2 条随后亦被覆盖 ⇒ 终态 132/132）+ `ack_lost=0` |
| 云端 `mqtt_messages`（下行） | **105 个不同 `msg_id`**（210 行） | 与投递数**精确一致** |
| 云端 `mqtt_messages`（上行 result） | **96 个不同 `msg_id`**（185 行） | **缺 9 个（8.6%）**，且**行数近乎 2×**（重复写入） |
| 云端 `mqtt_messages`（上行 ack） | **95 个不同 `msg_id`**（96 行） | **缺 10 个（9.5%）** |
| 云端 `log_history`（boot 22，终态） | `seq 5377–5508` 共 **132 行**，`seq` 连续 | **`5377 + 132 − 1 = 5508`，与设备 `emit_total=132` 精确相等 ⇒ `log_history` 链路零丢失**（漏记只发生在 `mqtt_messages`，两者不是同一条写入路径） |
| EMQX 会话 | `recv_msg=503` / `send_msg=276` / `mqueue_dropped=0` / `inflight=0` | 新会话 `connected_at=16:29:35Z` |

> **结论**：**设备侧 105/105 全部正确处理**；云端 `mqtt_messages` 在此次突发窗口**再次漏记 ~9%**，
> 并**新增发现重复行**（185 行 / 96 ID）。这与 `G4-R1` 一致并扩大了观察面 ⇒
> **再次确认：凡以 `mqtt_messages` 为证据的突发验收，必须用串口或设备计数交叉验证。**

---

## D. 硬停止条件逐条核查（全程未触发）

| 条件 | 本次观测 | 判定 |
|---|---|---|
| 意外重启 / panic / 看门狗 / 启动序号异常 | `ESP-ROM` 全程 **1 次**（烧录启动）；`panic`/`Guru Meditation`/`abort()` **0**；`boot_seq` 恒 22 | 未触发 |
| 明确的内存分配失败 / 堆损坏 / 栈溢出 | 无 `alloc failed` / `malloc` / `stack overflow` 迹象；`largest_free_block` 恒定 | 未触发 |
| 可用堆 / 最大连续块持续恶化 | `free` 先降 2 000 B 后**完全回补**；`minimum_free` 仅 −188 B 且**自 L3 后不再变**；`largest_free_block` 全程 51 188 B | 未触发 |
| 命令 / MQTT / 日志 / ACK 持续性故障 | 105/105 ACK；`ack_lost=0`；`queue` 归 0；MQTT 未断（`connected=1` 全程） | 未触发 |
| 非预期硬件动作 / 配置改变 | 未发放任何执行类命令（仅 `config_query` / `stats` / `memory` 只读）；`boot validated` 未回滚 | 未触发 |

**停止判据的建立（按授权要求，测试前明确）**：本次未临时发明阈值，而是采用
① **项目已定义的门禁**（`drop_unacked=0` / `hovf=0` / 段删除 ≤1/10min）；
② **结构性红线**（`boot_seq` 变化、`panic`、`alloc failed`、`largest_free_block` 单调下降、`ack_lost>0` 持续）。
因 `loop()` P99 无埋点，本次**未进入"最高负载级别"**（L4 已到 40 条实投后停止，未继续上调）。

---

## E. G-4 五项门禁逐项判定

判定口径一律取自 `BT-1` 计划 §6.4/§6.5 `GATE-WP5` ②（**五项**），**不修改任何阈值，不缩小任何定义**。

| # | 门禁指标 | 阈值 | 实测 | 判定 | 原始证据 / 计算口径 |
|---|---|---|---|---|---|
| 1 | `loop()` P99 劣化 | < 20% | **无法测量** | **`NOT VERIFIED`** | 固件无 `loop()` 计时埋点，`uptime_ms` 仅为 `millis()`。**不得**用"命令响应正常/HTTP 200"冒充。 |
| 2 | 内部 DRAM 峰值占用 | < 70% | **77.16%**（基线 77.08%） | **`FAIL`** | `(251 172 − 57 356) / 251 172`。★ 分母 = `ESP.getHeapSize()`（`internal.total`，本 boot 在 251 060–251 172 B 间浮动），**非** 327 680（那是 SRAM 总量，PlatformIO 的 40.5% 用的是它）。★ **基线即 77.08% > 70%** ⇒ **该阈值在本设备从未满足**（`G4-R4`），属阈值/口径缺陷，需人工裁决；就"突发是否使内存恶化"而言：**+0.09 pp / +300 B，且 `largest_free_block` 未变**。 |
| 3 | `drop_unacked` | = 0 | **0**（6 次快照全 0；`seg_evicted_unacked` 亦 0） | **`PASS`** | `counters.drop_unacked`；并以 `acked_seq = 5377 + emit_total − 1` 恒等式佐证 **130/130 全部 ACK** |
| 4 | `hovf`（空洞表溢出） | = 0 | **0**（6 次快照全 0） | **`PASS`** | `queue.hole_overflow` |
| 5 | 段删除频率 | ≤ 1 次 / 10 min | **0 次 / 21.3 min** | **`PASS`** | `counters.seg_deleted` 全程恒 0（差分 0 / 1 280 s） |

**同表附带（非门禁项，供参考）**：`drop_overflow=0` · `ring_drop=0` · `ack_lost=0` · `give_up=0` ·
`self_degraded=0` · `ack_ignored=7`（均为重复/迟到 ACK，已被状态机安全忽略）· `cloud_retx=9`（最大 `retry=2/5`）。

**加权结论**：**3 PASS / 1 FAIL / 1 NOT VERIFIED** ⇒ 五项**未全绿**。

---

## F. 最终裁定

### F-1 是否满足 G-4 全部解锁条件

**否。** `G-4 = BLOCKED` **维持不变**。阻塞项两条，性质不同：

1. **`loop()` P99 = `NOT VERIFIED`（能力缺失）** —— 不是"不合格"，是**测不了**。
   要闭环必须新增 `loop()` 耗时埋点（采样直方图或 P99 累积器）并再次烧录，
   **超出本次授权**（本次仅授权 `system.stats` 这一次核验/烧录）。
2. **内部 DRAM 峰值 = `FAIL`（口径缺陷）** —— 按 `BT-1` §6.4 的 `<70%` 口径，
   **本设备在任何负载下都不可能满足**（空载基线 77.08%）。这不是本次压力测试造成的退化
   （突发增量仅 +0.09 pp）。**必须由人工裁决改阈值或改口径**（`G4-R4`），
   在裁决前该项只能记为 `FAIL`，不得改成"通过"或"未测量"。

### F-2 状态维持

| 门禁 / 任务 | 状态 |
|---|---|
| `BT-1` | `CLOSED`（不变；本次未触及 BT-1 语义） |
| `G-4` | **`BLOCKED`**（由"条件② 无观测能力"收窄为"**1 项缺埋点 + 1 项阈值待裁决**"） |
| `WP-5` | **未解锁**（随 `G-4`） |
| `STOP-R1` | **无解除文件**（本次不产生解除依据） |

### F-3 尚存风险（不得因本次"零崩溃"而淡化）

1. **单 boot 周期 / 单负载形态**：全程 ~21 分钟、仅只读命令、无配置写入、无硬件动作、
   单一任务并发模型 ⇒ 对"写配置 / 触发 Valve / 弱网重连"等路径**零覆盖**。
2. **`mqtt_messages` 的可靠性缺陷仍在**（`G4-R1` 本次复现并发现重复行）：任何只看云端的验收都可能**误判丢包**。
3. **rebase 盲区仍在 `(21, 117]`**：见 F-4，`max_boot_seq` 会**停在 21 不推进**。
4. **`minimum_free` 是"本 boot 周期"的历史最低**：烧录已重置该基线，**不能**用它证明上一周期的
   `−29.4%` 问题已消失。
5. **`counters.cloud_records_sent` 恒 0**（`S-1`）：该字段不可用于任何判读。

### F-4 本次新增发现（供人工裁决 / 登记）

| 编号 | 级别 | 内容 |
|---|---|---|
| `S-1` | MINOR（观测） | `counters.cloud_records_sent` 无自增点 ⇒ 恒 0，易误读为"未发送" |
| `S-2` | MAJOR（证据管理） | 隔离 worktree 产出的 `firmware.bin` **未随 diff 归档** ⇒ 原件不可取回，只能重建 |
| `G4-R5` | MAJOR（云端取证） | `mqtt_messages` 在突发窗口**再次漏记**（上行 8.6% / 9.5%）**且出现重复行**（185 行 / 96 ID），印证并扩展 `G4-R1` |
| `G4-R6` | 观测口径（非缺陷） | rebase 后 `(21, 117]` 盲区生效：`boot=22` 被判 **`ambiguous`**，`max_boot_seq` **停在 21**、`ambiguous_count` 增长，`rewind_count` 因**合法补发 `boot=20` 记录** `0 → 21`。⇒ **今后不得用 `max_boot_seq` 判"设备是否重启"，一律以设备侧 `boot_seq` 为准** |
| `G4-R4`（补充） | 门禁口径 | DRAM 分母必须是 `internal.total`（heap ≈ 251 KB），不是 327 680；按 `<70%` 口径**空载即 77.08%**，阈值不可满足 |
| `G4-M1` | **部分关闭** | 原"M1：10 条 vs 20 条不可辨识，需新 boot 周期重测"——本次已在新 boot 周期用**分级**方式重测，且 `L3` 把 `minimum_free` 压出新低（57 544 → 57 356）⇒ **峰值时刻可被观测**，M1 的"不可辨识"前提不再成立；但 `minimum_free` 仍不可跨 boot 比较这一约束**保留** |

### F-5 后续建议（按优先级）

1. **裁决 `G4-R4`**：给出 DRAM 的最终口径（分母 + 阈值，或改为"突发增量 ≤ X pp / ≤ Y B"）。
   在此之前 DRAM 项无法通过任何测试翻身。
2. **决定是否为 `loop()` 增加埋点并二次烧录**：这是 `G-4` 唯一真正的能力缺口（建议实现
   `loop` 耗时最大/分位数累积器，随 `system.stats` 一并输出 `loop:{n,p50,p99,max}`）。
3. **修复 `S-1`**（接线或删字段）、**归档纪律 `S-2`**（凡 worktree 出固件必须同时归档 `.bin`+`sha256`）。
4. **在 runbook 写明 `G4-R6`**：rebase 后禁止用 `max_boot_seq` 判重启。
5. 若要真正验证"突发不退化"，**下一次应至少覆盖**：写入类命令、Valve 动作、MQTT 断连重连、
   以及 `boot_seq` 跨过 117（走出歧义带）。

---

## 附录 · 证据索引（详见 `docs/evidence/G-4-Stats-2026-10-20/README.md`）

| # | 文件 | 用途 |
|---|---|---|
| 1 | `git-state.txt` | 三仓 HEAD + 工作区状态（`16:21:40Z`） |
| 2 | `pre-flash-firmware.bin` / `pre-flash-device-state.txt` | 烧录前固件备份与设备/云端快照 |
| 3 | `p2stats-implementation.diff` | 源码差异（`sha256=d9b899ab…`） |
| 4 | `build.log` | 重建日志（`[SUCCESS]`，`error:` 0，RAM/Flash 行） |
| 5 | `serial-session.log` | **单次会话**串口原始捕获（启动 + 四级压力 + 静置，`220 909+ B`） |
| 6 | `burst-L1..L4.json` | 四级逐条命令台账（含 10 条代理 TLS 失败明细） |
| 7 | `post-flash-state.txt` | 烧录后 boot/watermark/EMQX/D1 交叉核对 |
