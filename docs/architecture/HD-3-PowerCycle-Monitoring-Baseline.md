# HD-3 断电测试监测基线（人工断电前）

> 日期：`2026-10-19 02:20–02:30`（GMT+8）
> 状态：**`READY FOR MANUAL POWER-CYCLE`**（本文件记录断电**前**基线；断电结果待人工操作后追加）
> 关联：`HD-3-RealDevice-Verification-and-Release.md` §13
> 性质：**只读采集**，未复位、未刷写、未格式化、未清理日志、未改配置

---

## 1. 目标与边界

| 项 | 内容 |
|---|---|
| 目的 | 验证**物理掉电**（而非 MCU 复位）后的持久化恢复：半写元数据、段恢复、`boot_seq` 连续性、未上传日志是否丢失、服务端水位是否单调 |
| 为什么不能用 RD-7a 替代 | RTS 复位时 Flash **没有正在进行的写**；物理断电才可能落在"两阶段提交中间"或"段写到一半" |
| 本轮授权 | 仅**准备与监测**；**不代为操作电源**、不复位、不刷写、不 `uploadfs`、不擦分区 |
| 设备唯一性 | 仅 `COM8` / `288485896ce4`，不涉及其他设备 |

---

## 2. 断电前设备基线（串口实测，`dtr=False / rts=False`，**未产生复位**）

> ⚠️ **本节数字已作废（`02:27:33`）**：监测进程异常崩溃时句柄被异常释放，导致**一次非计划 MCU 复位**
> （`boot 3 → 4`）。复位后的新基线见 **§2.5**。本节保留作"复位前快照"，用于交叉比对。

采集命令：`python .pio/hd3_monitor.py snap COM8 .pio/hd3_baseline_pre_powercycle.log 3`
证据文件：`.pio/hd3_baseline_pre_powercycle.log`（16 行）

### 2.1 身份与固件

| 项 | 值 | 依据 |
|---|---|---|
| 串口 | `COM8`（CH343，115200 8N1） | `pio device list` |
| `device_id` | `288485896ce4` | 与云端 `log_history` / `log_boot_watermark` 一致 |
| 固件源码版本 | **`87b7d5c`** | `git diff 87b7d5c..HEAD -- src/` 为**空**；设备于 `01:16` 烧录该构建（md5 `2c11ecd182b2b28fdebfda7f66602459`） |
| 固件内嵌版本号 | **无**（未编译 `__DATE__`/git rev） | `grep` 无命中 ⇒ 只能靠"烧录记录 + 源码 diff"证明，**不能**从设备自身读出 |

### 2.2 日志序号与元数据（`logt meta`）

```
[LogT] meta boot=3 reserved=768 corrupt=1 reliable=1 base=513 limit=768 last=524
[LogT] evidence src=0 nvs_ok=1 nvs_q=0 qfile=0 failclosed=0 nvs_boot=3 nvs_reserved=768 gen=6
[LogT] trace corrupt=0 last_seg=0
```

| 字段 | 值 | 含义 |
|---|---|---|
| `boot` | **3** | RD-8 整片擦除后从 1 起：`1 → 2 → 3`，此后未再重启 |
| `reserved` | **768** | 已预留高水位（号段 `513…768`） |
| `base` / `limit` | `513` / `768` | 当前区间 |
| `last` | **524** | 最近一次分配出去的 `seq` |
| `reliable` | **1** | 预留写入成功 |
| `corrupt` | 1 | `meta.corrupt_count` 累计值（历史遗留，非本轮新增） |
| `src` | **0 = `REC_FROM_NVS`** | 恢复来源 = NVS 持久证据（`log_meta_evidence::Recovery`） |
| `nvs_ok` / `nvs_q` | `1` / `0` | NVS 证据有效、**未**处于隔离 |
| `qfile` | `0` | `/log/quarantine.bin` 未置位 |
| `failclosed` | **0** | 未进入 fail-closed |
| `nvs_boot` / `nvs_reserved` | `3` / `768` | NVS 证据中记录的水位，与 RAM 一致 |
| `gen` | 6 | 证据写入代数 |
| `trace corrupt` / `last_seg` | `0` / `0` | 无损坏段痕迹（与 `corrupt=1` 不冲突：前者计**段损坏**，后者计**记录级 CRC 累计**） |

> 观测记录（非缺陷）：读取不存在的 `/log/corrupt.trace` 时 LittleFS 打一行
> `[E][vfs_api.cpp:105] open(): /littlefs/log/corrupt.trace does not exist, no permits for creation`。
> 属 `corrupt_trace_read()` 以读模式打开缺失文件的噪声日志，返回值按"无痕迹"处理，**不改变任何判定**。

### 2.3 运行计数（`logt stats`）

```
[LogT] stats emit=12 debug=0 ring=0/64 hw=6 drop=0 consumed=12 flash=1 cloud=12 crit=0 flush=0 boot=3 seq=524 ready=1 psram=1 bytes=8192
[LogT] fstats ok=1 fail=0 seg_new=0 seg_del=0 crc_err=1 corrupt=0 retry=0 blocked=0 honored=0
[LogT] cstats batch=16 rec=0 retx=0 ack_ok=16 ack_to=0 ack_lost=0 qdrop=0 fdrop=0 deg=0 qused=0
[LogT] cstats2 evict_inf=0 seg_evict_unacked=0 hole_evict=0
[LogT] ackst ignored=5 partial=0 segdel=0 replay=4 offskip=3487 giveup=0
```

**关键判读（断电前"未上传日志 = 0"）**：

| 指标 | 值 | 结论 |
|---|---|---|
| `ack_lost` | **0** | 无"已发出但未收到 ACK"的批 |
| `ack_to` | **0** | 无 ACK 超时 |
| `seg_evict_unacked` | **0** | **没有**因段回收而丢弃未确认日志 |
| `hole_evict` | **0** | 无空洞强制回收 |
| `giveup` | **0** | 无放弃重传 |
| `ring=0/64`、`qused=0` | 空 | 队列已清空 |

> 因此：**此刻断电不会造成"未上传日志丢失"**（这是后续判定"断电是否丢数据"的对照基线）。
> `crc_err=1` 是运行期历史计数，`corrupt=0` 表示当前无损坏段。

### 2.4 连接状态

| 项 | 值 | 依据 |
|---|---|---|
| WiFi | 已连接（本轮未打印连接横幅，但 `cloud=12` 计数持续） | `cstats` |
| MQTT | 已连接（`ack_ok=16`、`cloud=12`） | `cstats` |
| 时间基准 | **设备不打印 RTC 时间**；只能以云端 `created_at` 作为关联锚点 | 串口无时间输出 |

---

### 2.5 非计划复位（`02:27:33`）与复位后复核基线（`02:28:01`）

**发生了什么**

| 时刻（本地） | 事件 |
|---|---|
| `02:24:58` | 监测进程 `watch` 打开 COM8（`dtr=False/rts=False`），**未复位**（之后 3 分钟无启动横幅） |
| `02:25:12` | 我用**第二个进程**尝试打开 COM8 以"证明端口被占用" ⇒ `PermissionError(13)` |
| `02:27:37` | 监测进程崩溃：`SerialException: GetOverlappedResult failed (PermissionError(13))` —— 第二句柄破坏了 CH343 的 overlapped I/O |
| `02:27:33–48` | 句柄被异常释放（`finally` 里的 `setDTR/setRTS` 已随句柄失效）⇒ **RTS/DTR 抖动触发一次 MCU 复位** |
| `02:27:48 / 02:28:06` | D1 出现 `boot_seq=4` 的 9 行（`seq 769…777`） |

> **归因结论**：这次复位是**我的探针工具**造成的，**不是**用户的物理断电，**也不是**设备自身故障。
> 它与 RD-7a（RTS 复位）同属一类，**不能**替代物理断电测试。

**复位后复核（`02:28:01`，`logt meta`）**

```
[LogT] meta boot=4 reserved=1024 corrupt=1 reliable=1 base=769 limit=1024 last=777
[LogT] evidence src=0 nvs_ok=1 nvs_q=0 qfile=0 failclosed=0 nvs_boot=4 nvs_reserved=1024 gen=8
[LogT] trace corrupt=0 last_seg=0
[LogT] stats emit=9 ... boot=4 seq=777 ... / cstats batch=8 ack_ok=7 ack_to=0 ack_lost=0 qused=2
[LogT] cstats2 evict_inf=0 seg_evict_unacked=0 hole_evict=0
```

| 判据 | 结果 | 结论 |
|---|---|---|
| `boot_seq` | `3 → 4`（**+1，未回退**） | ✅ 序号连续 |
| 恢复来源 | `src=0 (REC_FROM_NVS)` | ✅ 走 NVS 证据，非 `PROVISION` |
| `reliable` / `failclosed` | `1` / `0` | ✅ 无降级、无隔离 |
| `nvs_boot` | `4` | ✅ NVS 证据已随本次 boot 更新 |
| `trace corrupt` | `0` | ✅ 无损坏段（复位未落在写窗口） |
| 未上传日志 | `ack_lost=0`、`seg_evict_unacked=0`（`qused=2` 为在途） | ✅ 无丢弃 |

**云端侧（本次复位）**：

| 项 | 值 |
|---|---|
| `boot_seq=4` 落库 | **9 行**，`seq 769…777`，`created_at 18:27:48–18:28:06Z` |
| `max_boot_seq` | **仍 117**（未回退） |
| `rewind_count` | `22 → 36`（`boot=4 < 117` ⇒ 必然计候选，**与 §4.2 A-10 预警一致**） |
| `last_rewind_boot` | `4` |

**这次意外反而提供了一条额外证据**：`boot=4` 是"复位 → 恢复 → 上传 → 水位单调"的完整闭环，
且**未产生** `log_conflict`（`boot=4` 是新键）。但它**不覆盖**"掉电瞬间半写"，所以物理断电仍为 `NOT RUN`。

### 2.6 复位后（当前）基线 — 以下数字为断电测试的真实对照

| 项 | 值 |
|---|---|
| `boot_seq` | **4** |
| `reserved` / `base` / `limit` / `last` | `1024` / `769` / `1024` / `777` |
| `corrupt` / `reliable` | `1` / `1` |
| `src` | **0 = `REC_FROM_NVS`** |
| `nvs_ok` / `nvs_q` / `qfile` / `failclosed` | `1` / `0` / `0` / `0` |
| `nvs_boot` / `nvs_reserved` / `gen` | `4` / `1024` / `8` |
| `trace corrupt` / `last_seg` | `0` / `0` |
| 未上传日志 | **0**（`ack_lost=0`、`seg_evict_unacked=0`；`qused=2` 为在途批次，采集后已 ack） |

> **断电后期望值**：`boot_seq` 应为 **5**；`src` 仍应为 `0`；`failclosed=0`；`trace corrupt` 用于判断
> 掉电是否落在写窗口。**若 `boot_seq` 回到 1 或出现 `PROVISION/QUARANTINE`，即判为异常并停止后续操作。**



## 3. 云端基线（只读查询，`--remote`）

| 项 | 值 |
|---|---|
| Worker 线上版本 | **`508461f3-0dcf-443c-98f9-d185f412174e`**（handlers `fetch, scheduled`） |
| 开关 | `LOG_BOOT_WATERMARK = "1"` |
| migration | `0001…0005` 全部已应用（`No migrations to apply!`） |

### 3.1 `log_boot_watermark`（设备 `288485896ce4`）

> 下表为 **`02:20` 首采**与 **`02:28` 复位后复采**两个时点；断电测试以 **复位后**为准。

| 字段 | `02:20`（boot=3） | `02:28`（boot=4，当前） |
|---|---|---|
| `max_boot_seq` | 117 | **117（不变）** |
| `max_seq_at_max` | 114441 | 114441 |
| `obs_count` | 32 | **46** |
| `rewind_count` | 22 | **36** |
| `last_rewind_boot` | 3 | **4** |
| `updated_at` | `17:45:55Z` | **`18:28:19Z`** |
| `status` | open | open |

```json
{"device_id":"288485896ce4","max_boot_seq":117,"max_seq_at_max":114441,
 "obs_count":46,"rewind_count":36,"last_rewind_boot":4,
 "updated_at":"2026-10-18 18:28:19Z","status":"open"}
```

### 3.2 `log_history` 与 `log_conflict`

| 项 | 值 |
|---|---|
| 本设备总行数 | **337**（`02:20` 时 328，复位后新增 `boot=4` 的 9 行） |
| 最新 `created_at` | **`2026-10-18 18:28:06` UTC**（= 本地 `02:28:06`） |
| `boot_seq` 分布（近期） | `117`(9) · `116`(9) · `115`(11) · `114`(9) · `113`(9) · **`4`(9, seq 769–777)** |
| `log_conflict` | **0 行**（`boot=4` 是新键，未撞） |

### 3.3 时间关联

- 采集时刻：本地 **02:28**（`GMT+8`）= **18:28 UTC**。
- 最近一次落库：**18:28:06 UTC** ⇒ 与采集同一分钟（复位后立即上报）。
- **可关联性**：云端 `created_at` 与服务端 `unixepoch()` 同源；设备端无时间戳打印，因此
  "设备事件 → 云端行"的关联**只能**通过 `boot_seq` + `seq` 区间，**不能**靠设备时间。

---

## 4. 观测清单（断电 → 上电）

### 4.1 断电前（已完成）

| # | 动作 | 状态 |
|---|---|---|
| P-1 | 保存串口基线 `.pio/hd3_baseline_pre_powercycle.log` | ✅ |
| P-2 | 记录 `boot=3 / reserved=768 / last=524 / src=0(NVS) / failclosed=0` | ✅ |
| P-3 | 确认未上传日志 = 0（`ack_lost=0 / seg_evict_unacked=0`） | ✅ |
| P-4 | 保存云端水位与 `log_history` 最新时间 | ✅ |
| P-5 | 启动只读监测（不发送任何命令、不复位） | ✅ 见 §5 |
| P-6 | 记录监测窗口与设备身份 | ✅ |

### 4.2 上电后（待执行）

| # | 观察点 | 期望 | 判定层 |
|---|---|---|---|
| A-1 | 串口是否出现 `[Log] recovery=` | `src=0 (NVS)`；**不应**出现 `QUARANTINE` / `PROVISION` | **设备端实测** |
| A-2 | `boot_seq` | 应为 **4**（3 + 1）；**不得**回到 1 | 设备端实测 |
| A-3 | `reliable` / `failclosed` | `reliable=1`、`failclosed=0` | 设备端实测 |
| A-4 | 是否有半写/损坏痕迹 | `trace corrupt` 若 >0 ⇒ 说明掉电落在写窗口，需记录 | 设备端实测 |
| A-5 | `mstage`/双槽 | 不适用（未注入）；但 `src` 不应变成 `1(SLOT)` 之外的异常值 | 设备端实测 |
| A-6 | WiFi / MQTT | `WiFi connected` → `MQTT connected` → `boot validated` | 设备端实测 |
| A-7 | 新日志能否上传 | `cstats ack_ok` 继续增长、`ack_lost=0` | 设备端实测 |
| A-8 | 是否被服务端当重复丢弃 | `log_history` 出现 `boot_seq=4` 新行；`duplicate` 不应异常升高 | **Worker 处理结果**（需 log/`summary`）+ **D1 持久化** |
| A-9 | 水位是否单调 | `max_boot_seq` **仍为 117**（不会因 `boot=4` 降低），`obs_count` 增加 | **D1 持久化** |
| A-10 | 是否产生新回退候选 | `rewind_count` 可能 +N（`boot=4 < 117` 属**预期**，因为水位是历史最大值） | **D1 持久化** |
| A-11 | `log_conflict` | 仍 0（`boot=4` 是新键） | D1 持久化 |

> ⚠️ **A-10 的口径**：当前水位 `117` 是 RD-8 之前的历史最大值，而设备真实 `boot_seq` 只有 `3`。
> 因此**任何** `boot=4` 的上报都会被计为 `rewind` —— 这是"水位高于设备真实值"的**已知后果**，
> **不能**据此判定"断电导致回退"。判定"是否异常"必须看 **设备侧 `boot_seq` 是否连续（3→4）**，
> 而不是看 `rewind_count` 是否增长。

### 4.3 判定分层（必须分开写）

| 层 | 证据来源 | 能证明什么 |
|---|---|---|
| **设备端实测** | 串口 `[Log] recovery=` / `[LogT] meta` | 序号、恢复来源、隔离、半写痕迹 |
| **Worker 处理** | `wrangler tail` 或 `[log-ack] summary` | `accepted/duplicate/collision` 计数、是否 200 |
| **D1 持久化** | 只读 SQL | 实际落库行、水位、冲突表 |
| **推断（未验证）** | —— | 一律标注，不得写成结论 |

---

## 5. 监测链路（`rev.2` · 已重启）

| 项 | 内容 |
|---|---|
| 脚本 | `.pio/hd3_monitor.py`（**只读**：`Serial()` 先设 `dtr=False / rts=False` 再 `open()`；**全程不碰 RTS/DTR**；watch 模式**不发送任何命令**） |
| 启动命令 | `python .pio/hd3_monitor.py watch COM8 .pio/hd3_powercycle_watch.log 7200` |
| 输出文件 | `.pio/hd3_powercycle_watch.log`（append，逐行带本地时间戳） |
| **断线自愈** | `SerialException` / 端口消失 ⇒ 关闭句柄 → 等端口重新出现 → 重开继续写**同一日志**（带 `reopen#N` 标记）。<br>这是 `rev.2` 新增：断电若同时断 USB，端口会消失再枚举，原实现会直接崩溃丢 boot 横幅 |
| **心跳** | 每 30 s 写一行 `### alive` ⇒ 可证明"监测仍存活"，而不是我以为它活着 |
| **当前实例** | `rev.2` 于 **`02:29:15.360`** 重启（`pid=40080`）；`02:29:16` 与 `02:29:46` 两次 `### alive` 已落盘；**无** `rst:0x`/`ESP-ROM` 横幅 ⇒ 本次打开**未复位** |
| 旧实例 | `02:24:58` 启动的 `rev.1` 已崩溃（见 §5.1），其两行日志保留在同一文件头部，便于追溯 |
| 已验证"能收到输出" | `snap` 模式两次成功收到 `Recv cmd:` 与 `[LogT]` 响应（§2 / §2.5） |
| 是否触发复位 | **首次启动（`02:24:58`）未复位**（3 分钟内无启动横幅、`boot_seq` 仍 3）；**后续复位是崩溃导致的，见 §2.5** |
| 不会做的事 | watch 模式**连 `logt` 都不发**；不刷写、不格式化、不 `uploadfs`、不改配置 |

### 5.1 ★ 事故记录：`02:27:37` 监测崩溃 → 非计划复位（已修）

| 项 | 内容 |
|---|---|
| 现象 | `SerialException: GetOverlappedResult failed (PermissionError(13, '拒绝访问。'))`，进程退出 |
| 根因 | 我用**第二个进程**打开 COM8 去"证明端口被占用"。CH343 上第二次 `open` 会破坏已持有句柄的 overlapped I/O |
| 后果 | 句柄被异常释放 ⇒ RTS/DTR 抖动 ⇒ **一次 MCU 复位**（`boot 3 → 4`，D1 `18:27:48Z` 落库 9 行） |
| 修复 | ① 脚本加**断线自愈 + 心跳**；② **不再**用"二次打开"验证占用；③ 需要证明存活时看 `### alive` 心跳，而不是碰端口 |
| 残留风险 | **进程被强杀/异常退出**时，仍可能因句柄释放触发一次复位（驱动行为，脚本无法拦截）。因此：**只在测试结束后正常停止**；若中途必须停，先告知，我会按"可能复位一次"处理 |
| 结论 | 这次复位**不改变**任何持久化结论（`src=0`、`boot 3→4`、`failclosed=0`），但它**不能**替代物理断电测试 |

## 6. 回退就绪（详见主报告 §12）

| 路径 | 目标 | 就绪度 |
|---|---|---|
| 固件 | `3acf91c`（A-1 之前） | ⚠️ **需重建**：无 pre-A-1 构建产物（现有两份 `firmware.bin` md5 相同，均为 A-1） |
| 固件（保留当前） | `87b7d5c` | ✅ 产物在（md5 `2c11ecd1…`），可用于"测试后恢复原状" |
| Worker | `72023596-657a-4a65-aa62-81848540f324` | ✅ 版本存在、handlers 含 `scheduled`；命令未执行 ⇒ `NOT VERIFIED` |
| 开关 | `LOG_BOOT_WATERMARK="0"` + `deploy` | ✅ 可执行；`[vars]` 非热更新 |
| D1 | 不回退（表保留） | ✅ 0004/0005 已应用，无 down migration |

---

## 7. 待人工执行

### 7.1 推荐断电方式（按优先级）

| 优先 | 方式 | 串口是否连续 | 能抓到 boot 横幅 |
|---|---|---|---|
| **A（首选）** | 断**设备电源输入**（DC 供电 / 电源开关），USB 数据线保持连接 | ✅ 连续 | ✅ 完整 |
| B | 拔 USB 线（USB 既供电又传数据） | ❌ 会断开，靠 §5 的断线自愈重连 | ❌ 可能丢横幅（但 `boot_seq` / `src` 仍可从 `logt meta` 读到） |

> 若只能用 B：**不影响判定**——`[LogT] evidence src=` 反映的是**本次 boot 实际使用的恢复来源**，复位后仍可读。

### 7.2 步骤

1. 保持本会话与监测进程运行（看 `.pio/hd3_powercycle_watch.log` 有 `### alive`）。
2. 按 §7.1 断电，**等待 ≥5 s**（让电容放净）。
3. 重新上电，**不要**按键、**不要**重烧、**不要** `uploadfs`。
4. 等待 **≥60 s**（让 `boot_seq=5` 的批次完成上传）。
5. 告诉我"已上电"，我来采集并出对比报告。

### 7.3 上电后我会核对

| # | 判据 | 期望 |
|---|---|---|
| 1 | 设备 `boot_seq` | **4 → 5**（**不得**回到 1） |
| 2 | `src` | 仍为 `0 (REC_FROM_NVS)`；**不得**出现 `PROVISION(4)` / `QUARANTINE(5)` |
| 3 | `reliable` / `failclosed` | `1` / `0` |
| 4 | `trace corrupt` | 若 >0 ⇒ 说明掉电落在写窗口（**记录，不判失败**） |
| 5 | WiFi / MQTT | `WiFi connected` → `MQTT connected` → `boot validated` |
| 6 | D1 | 出现 `boot_seq=5` 行；`max_boot_seq` 仍 117；`log_conflict` 仍 0 |
| 7 | 水位 | `obs_count` 增加；`rewind_count` 增加属**预期**（见 §4.2 A-10） |

> 本文件在断电结果出来后**追加** §8「实测结果」，不改写上面任何基线数字。

---

## 8. 实测结果（`2026-10-19 02:30:48 – 02:31:48`）

**结论：`PASS`（首次物理断电，1 个循环）**

### 8.1 执行方式与时间线

| 时刻 | 事件 | 证据 |
|---|---|---|
| `02:29:52–02:30:2x` | 断电前正常上传：`[Cloud LOG] OK` + `log_ack boot=4 to=779` | 串口 |
| `02:30:48.891` | **串口丢失**（USB 被拔）：`SERIAL LOST #1: ClearCommError PermissionError(13)` | 监测日志 |
| `02:30:49–02:31:10` | 端口不存在（`WAIT port COM8 not present`），持续 **~21 s** | 监测日志 |
| `02:31:10.538` | 端口重新枚举，监测自动重开：`OPEN(no-reset) ... reopen#1` | 监测日志 |
| `02:31:11.275` | 设备开始输出：`[System] LittleFS mounted` | 串口 |
| `02:31:11.6–11.8` | **`[Log] recovery=NVS boot_hw=4 -> boot_seq=5`** | 串口 |
| `02:31:12.7` | **`[SYSCMD] reset reason=power_on`** | 串口 |
| `02:31:14.7` | `WiFi connected` → `[CFG] boot validated` | 串口 |
| `02:31:15.5` | `[Cloud] MQTT connected` | 串口 |
| `02:31:16–33` | `boot=5` 的 9 条日志逐条 `ack ok`（`seq 1025…1033`） | 串口 + D1 |
| `02:31:48` | `replay sweep done (acked=1033 gc=256 floor=257 giveup=0)` | 串口 |

> **断电方式** = 方案 B（USB 既供电又传数据）。断电时长 **~21 s**（远大于"电容放净"的 5 s 要求）。
> **端口重枚举后仍抓到了完整的应用启动段**（从 LittleFS 挂载起），只缺 ROM/bootloader 那几行。

### 8.2 设备端判定

| # | 判据 | 期望 | 实测 | 判定 |
|---|---|---|---|---|
| 1 | 复位原因 | `power_on` | `[SYSCMD] reset reason=power_on` | ✅ **这是"物理断电"的判定性证据**（软件复位会是 `sw`/`panic` 等） |
| 2 | `boot_seq` | 4 → **5** | `boot_hw=4 -> boot_seq=5` | ✅ 递增，**未回 1** |
| 3 | 恢复来源 | `NVS(0)` | `recovery=NVS ... (nvs=1 slot=1 legacy=0 seg_max=0)` · `src=0` | ✅ 走 NVS 持久证据 |
| 4 | 是否为"全新设备" | 否 | **无** `PROVISION` | ✅ 未被误判 |
| 5 | 隔离 | 无 | `failclosed=0`，无 `QUARANTINE` 日志 | ✅ |
| 6 | 预留可靠性 | `1` | `reliable=1` | ✅ |
| 7 | 新号段 | 正常分配 | `base=1025 limit=1280 reserved=1280` | ✅ 从 NVS `reserved_hw=1024` 续推 |
| 8 | 损坏痕迹 | 0（或可解释） | `corrupt.trace does not exist`（读失败⇒按 0）· `corrupt=1`（历史累计，未增） | ✅ 掉电**未落在写窗口** |
| 9 | 段恢复 | 无损坏段 | `Flash ring ready: valid=1 ... total_rec=5` | ✅ 无 CRC 错、无删除 |
| 10 | 配置 | 8/8 | `[CFG][I] init done, loaded 8/8 modules` → `boot validated` | ✅ LittleFS 未受损 |
| 11 | 联网 | 恢复 | `WiFi connected` → `MQTT connected`（~3 s） | ✅ |
| 12 | 未上传日志 | 不丢 | 启动后重放并逐条 `ack ok`；`giveup=0` | ✅ 5 条历史记录全部补传成功 |
| 13 | 时间基准 | 恢复 | `[Time] SNTP sync OK (2026-10-19 02:31:15)` | ✅ |

### 8.3 云端侧判定

| # | 判据 | 实测 | 判定 |
|---|---|---|---|
| 1 | `boot=5` 落库 | **9 行**，`seq 1025–1033`，`created_at 18:31:16–18:31:33Z` | ✅ 新键被 `accepted` |
| 2 | 重放是否造成重复行 | `boot=2` 在重放后**仍为 3 行**（`257–259`）；`boot=3` 的 `513`、`boot=4` 的 `769` 均未被重复插入 | ✅ **`ux_lh_dedup` 去重生效** |
| 3 | 重放中真正新增的 | `boot=3` 增 `523/524`、`boot=4` 增 `778/779`（断电前未 ACK 的尾部） | ✅ 该类记录**没有丢** |
| 4 | `log_conflict` | **仍 0 行** | ✅ 重放内容与首行一致（`same_content`），无内容冲突 |
| 5 | 水位单调性 | `max_boot_seq` **仍 117**；`obs_count 46→63`；`rewind_count 36→53`；`last_rewind_boot=5` | ✅ **水位未被拉低**；`rewind` 增长属 §4.2 A-10 已预警的预期行为 |
| 6 | 总行数 | 337 → **348** | ✅ 只有新键增长 |

### 8.4 这次测试证明了什么 / 没证明什么

**证明了**

1. 物理断电（`reset reason=power_on`）后，`boot_seq` **连续递增**（4→5），**不从 1 重放**；
2. 恢复走 **NVS 持久证据**，而不是 `PROVISION` —— `A1-01` 的"日志区为空不得判全新"在真实掉电下成立；
3. 掉电时**没有**正在写的元数据（`reliable=1`、`trace=0`），两阶段提交未被打断；
4. 段内**未 ACK 的尾部记录没有丢**（重放补传成功，`giveup=0`）；
5. 云端去重与水位在"断电重放"场景下行为正确（`boot=2` 未产生重复行、`max_boot_seq` 未回退）。

**没证明**

| 项 | 原因 |
|---|---|
| **掉电恰好落在元数据写窗口** | 只有 1 次断电，且 `trace corrupt=0` ⇒ 未命中该窗口。要覆盖需多次随机断电（建议 ≥20 次，见主报告 R-3） |
| 掉电落在**段写入中途**（半条记录） | 同上，未命中；`crc_err` 未增加 |
| 断电时长与电容放电的边界 | 本次断 21 s，未测"极短断电"（<100 ms） |
| 电池/DC 供电路径 | 本次是 USB 断电（方案 B），未测"仅断 DC、USB 保持"（方案 A） |

> 因此状态定为：**`PHYSICAL POWER-CYCLE = PASS (1 cycle, USB power path)`**，
> 而"掉电半写"仍是 **`NOT COVERED`** —— 不能因为本次通过就把 R-3 整条划掉。

---

## 9. 扩展：随机插拔 14 次（`2026-10-19 02:31 – 02:46`）

> 用户随机插拔 USB（供电 + 串口同线，方案 B）。监测**全程未中断**：
> `SERIAL LOST` **14 次** ⇒ `reopen#1…#14` ⇒ 抓取到 **14 段完整应用启动日志**。
> `boot_seq` **5 → 18 连续无跳号** ⇒ 可确认**恰好 14 次**上电（不是"大约 20 次"）。

### 9.1 逐轮结果

| `boot_seq` | `recovery` | `reset reason` | 云端行数 | `seq` 区间 |
|---|---|---|---|---|
| 5 | NVS | power_on | 9 | 1025–1033 |
| 6 | NVS | power_on | 9 | 1281–1289 |
| 7 | NVS | power_on | **1** | 1537 |
| 8 | NVS | power_on | 9 | 1793–1801 |
| 9 | NVS | power_on | **8** | 2049–2056 |
| 10 | NVS | power_on | 9 | 2305–2313 |
| 11 | NVS | power_on | 9 | 2561–2569 |
| 12 | NVS | power_on | 9 | 2817–2825 |
| 13 | NVS | power_on | 9 | 3073–3081 |
| 14 | NVS | power_on | 9 | 3329–3337 |
| 15 | NVS | power_on | 9 | 3585–3593 |
| 16 | NVS | power_on | **4** | 3841–3844 |
| 17 | NVS | power_on | 9 | 4097–4105 |
| 18 | NVS | power_on | 9 | 4353–4361 |

> `7` / `9` / `16` 行数偏少，是**断电太快、记录本来就少**（不是丢数据）：设备侧 `total_rec` 全程 `5→18` **每轮 +1、从未回落**。

### 9.2 14 轮的关键判据（全部为 0 异常）

| 检查项 | 结果 |
|---|---|
| `recovery=NVS` | **14/14** |
| `reset reason=power_on` | **14/14** |
| `failclosed=1` | **0** |
| `QUARANTINE` / `PROVISION` | **0 / 0** |
| `reliable=0`（预留失败） | **0** |
| `corrupt.trace` 出现（段损坏痕迹） | **0**（14 次都是 "does not exist"） |
| `crc_err` / `seg_del` / `giveup` > 0 | **0 / 0 / 0** |
| `[Log] seq:` 的 `reserved` | `1024 → 4352`，**每轮 +256 严格单调** |
| `total_rec` | `5 → 18`，**每轮 +1，无回落**（说明记录从未被清空） |
| `ack` 超时 / 丢失 | **0**（221 次 `ack ok`，`replay sweep ... giveup=0`） |

**结论**：14 次随机断电中，**没有一次**落在元数据/段写入窗口（否则会出现 `reliable=0`、`trace>0` 或 `crc_err` 增长）。
即：**「跨重启恢复」14/14 通过；「掉电半写」仍未命中**（14 次样本，0 次命中）——这是「没有证据表明有问题」，
**不是**「证明写窗口抗掉电」。

> ✅ **负责人裁决（`2026-10-19 02:52`）**：写入窗口只有毫秒级，随机断电命中概率极低，**继续抽样检测的投入产出不成立**
> ⇒ **不做专项检测**。该项登记为 **`ACCEPTED RISK (owner decision)`**，不再是系统级关闭的阻塞项。
> ⚠️ 口径纪律：写「已接受的风险」，**不得**写成「已验证安全 / 已覆盖」。

### 9.3 ★ 新发现：水位被"历史峰值"污染，`rewind_count` 已失去区分力

| 指标 | 值 | 解读 |
|---|---|---|
| `max_boot_seq` | **117** | RD-8 整片擦除**之前**的历史峰值；设备真实值现在只有 18 |
| `obs_count` | **274** | 观测批次总数 |
| `rewind_count` | **264** | 其中 **264 条被计为"回退候选"**（= 96.4%） |
| `last_rewind_boot` | 18 | 最近一次候选 |

**这不是 bug，是设计缺陷的实测暴露**：

- 水位表的 `max_boot_seq` 一旦取到"擦除前的峰值"，设备后续每次正常启动（`boot_seq` 远小于 117）**都会被计为 rewind**；
- 于是 `rewind_count` 从"异常信号"退化为"几乎等于 `obs_count` 的计数器"，**无法区分**"设备正常重启"与"真的发生回退"；
- `status` 会长期停在 `open`，形成**告警疲劳**（264 条候选里 0 条是真异常）；
- 缺少**重置基线（rebase）**机制：整片擦除是合法运维动作，但水位表没有"经确认后接受新基线"的入口。

**建议（需产品/运维裁决，本轮未实现）**：

1. 最小改动：给 `log_boot_watermark` 增加人工确认的 `rebase` 操作（写 `status='benign'` + 更新 `max_boot_seq` 为当前观测值 + 留审计字段 `rebased_at/rebased_by`）；
2. 或：以 `(device_id, max_boot_seq, max_seq_at_max)` 三元组作为"世代"，跨世代比较时先判是否发生过格式化（可与 `device_event` 关联）；
3. 无论哪种，**都不能**用"降低 `max_boot_seq`"来消音 —— 那会丢失"曾经到过 117"的事实。

> ⚠️ 在此之前，**不要**把 `rewind_count` 当告警阈值用；唯一可靠的判据仍是设备侧 `[Log] recovery=` 与 `boot_seq` 连续性。
