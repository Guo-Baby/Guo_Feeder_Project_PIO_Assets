# HD-3 · A-1 / B-3 独立检查与验收报告

> **性质**：以**发现缺陷**为目标的独立技术审查（不预设实现正确，不以既有测试全绿为验收依据）
> **基线**：`IMPLEMENTED - LOCAL ONLY`（未部署 / 未烧录 / 未 commit）
> **执行时间**：`2026-10-18 22:00–23:20`（本地）
> **授权范围**：本地独立审查 + 运行本地测试 + 补充 B-3 本地 `real_d1` 测试
> **未执行**：生产 D1 查询/写入 · Worker 部署 · EMQX/Pages 变更 · 设备命令 · 烧录 / OTA · Git 操作 · migration 修改 · 实现代码修改

---

## 0. 结论速览

| 项 | 状态 | 一句话结论 |
|---|---|---|
| **A-1** | **`BLOCKED`** | `R-1` 已被有效消除；**`R-2` 未被消除**，且新引入的 `fail-closed` 在**第二次重启后自我失效** ⇒ 不满足"可证明 `boot_seq` 唯一"的准入线 |
| **B-3** | **`CONDITIONAL`** | 逻辑在真实 D1 上 **52/52 全绿**，无假 `same` / 假 `collision`；附条件：测试断言需收紧、碰撞行仍发 ACK 的语义待裁决 |
| **整体 HD-3** | **`BLOCKED`** | 由 A-1 决定；B-3 不具备独立放行 A-1 的条件（两者解决同一故障的两端） |

**阻断级缺陷 1 项**（`A1-01`）、**重大 2 项**（`A1-02`、`A1-03`）、**次要 9 项**、**问题 1 项**、**笔误 2 项**。

---

## 1. 基线与工作区保护（阶段 1）

### 1.1 固件仓 `PIO_Assets/Guo_Feeder_Project`

| 项 | 值 |
|---|---|
| 分支 / HEAD | `wb` · `3acf91c32a5796467c948503d888d08918cf0732`（`2026-10-18 20:08`） |
| 本任务相关修改 | ` M src/log/log_manager.cpp`（`+308/−49`）· ` M src/log/log_manager.h`（`+30`）· ` M test/log_contract/run_contract_test.py`（`+27/−14`） |
| 本任务新增（未跟踪） | `?? src/log/log_meta_slot.h` · `?? test/log_contract/probe_meta_slot.cpp` · `?? docs/architecture/HD-3-*.md`（3 篇）· `?? docs/architecture/P3-WP6-*.md`（2 篇，**属 WP-6 任务，非本任务**） |
| 其他任务遗留修改 | ` M HANDOFF.md` · ` M docs/README.md` · ` M docs/architecture/P3-History-D1-Plan.md` · ` M docs/architecture/P3-WP3-History-Storage-Policy-Design.md` · ` M docs/issues/未修复的问题.md` |
| 处置 | **零改动**（未 checkout / 未 stash / 未格式化）。本报告为**新增文件**，不触碰上述任何差异 |

### 1.2 云端仓 `Cloudflare_Assets`

| 项 | 值 |
|---|---|
| 分支 / HEAD | `main` · `a3473eb06165e8b36f360b09aa5010b92e6b0c4e`（`2026-10-18 20:07` · RET-1） |
| 本任务相关修改 | ` M guo-feeder-api/src/log-ack.js` · ` M guo-feeder-api/src/history-ingest.js` · ` M guo-feeder-api/test/run_all.mjs` |
| 本任务新增 | `?? guo-feeder-api/test/current/log_ack_collision.test.mjs` |
| 本轮新增 | `?? guo-feeder-api/test/real_d1/b3_collision_real_d1.sh`（本报告新增，**不改动**上述 4 项） |

### 1.3 审查文件清单（所有结论均对应下列文件）

```
固件：src/log/log_meta_slot.h · src/log/log_manager.h · src/log/log_manager.cpp
      src/main.cpp(451) · src/services/device_identity.cpp(53,91)
      test/log_contract/probe_meta_slot.cpp · test/log_contract/run_contract_test.py
云端：src/log-ack.js · src/history-ingest.js · migrations/0003_p3_history_tables.sql
      test/current/log_ack_collision.test.mjs · test/real_d1/b3_collision_real_d1.sh
```

---

## 2. A-1 恢复状态机审查（阶段 2）

### 2.1 恢复优先级（`log_meta_slot::decide_boot_source`，`log_meta_slot.h:192`）

| # | 通道 | 判据 | 结果 |
|---|---|---|---|
| ① | 双槽 | `meta_read_best()`（`log_manager.cpp:1429`）`slot_seq` 大者胜 | `boot = 槽值` |
| ② | legacy | 仅当双槽都无效 | `boot = meta.bin` 值 |
| ③ | 段内 | 仅当前两者都无效，扫段取 `LogRecord.boot_seq` 最大值 | `boot = max_boot` |
| ④ | 全新 | 无段记录**且**无损坏痕迹 | `boot = 0 ⇒ s_boot_seq = 1` |
| ⑤ | 其余 | — | `BOOT_UNRECOVERABLE ⇒ fail-closed` |

★ ④ 与 ⑤ 的分界是"是否存在任何历史痕迹"。**该分界的证据范围限于日志区自身** —— 这正是 `A1-01` / `A1-02` 的根因。

### 2.2 双槽提交正确性（逐条核实）

| 核实项 | 结论 | 依据 |
|---|---|---|
| 格式 / 版本 / CRC / 提交标记 / 槽序号可识别损坏 | ✅ | `pack()`/`parse()`（`log_meta_slot.h:78,109`）四项校验（magic / fmt=3 / commit=1 / CRC32 覆盖 `[0..27]`）；`slot_seq` 与 `commit` **均在 CRC 跨度内** ⇒ 篡改必失配（探针 `M3/M4`） |
| 写目标槽失败时旧槽保留 | ✅ | `meta_commit_write()`（`1469`）目标恒为 `(s_meta_slot==0)?1:0`，即**永不覆盖当前有效槽**；失败时不更新 `s_meta_slot`（1492-1496 之前返回） |
| `commit=0`/`commit=1` 的断电语义 | ⚠️ **逻辑正确，物理未证** | 半写槽因 `commit≠1` 或 CRC 失配判无效（`parse` 123-131），探针 `M8` 覆盖"新旧槽并存"；**但** `open("w")` 的截断时序与 32 B 写入的掉电行为**未经真机断电验证**（见 §6-R1） |
| 选槽：一槽有效 / 两槽有效异版本 / 同版本 / CRC 错 / 双坏 / 新旧格式并存 / 回绕 | ✅（回绕除外） | 探针 `M7a–f`、`M5`（legacy `fmt=2` 即便 CRC 自洽也判无效）、`M9`；`slot_seq` **无回绕保护**（`next_seq = s_meta_slot_seq + 1`，1487），4e9 次提交不可达 |
| 写入成功但同步/关闭失败 ⇒ 误判已提交 | ❌ **可能发生** | 见 `A1-05` |

**两阶段提交的实质保证**：任何时刻**至少一个**槽保存着最后一次成功提交的值（因为写入目标恒为非当前槽）——这一点成立。掉电后最坏回退到"上一次成功值"，而非"半新半旧"。这是 `R-1` 被消除的核心机制。

### 2.3 `R-1`（元数据写失败后序号重用）—— **已消除**

| 环节 | 证据 |
|---|---|
| `meta_write` 全部调用点 | 仅 `1498`（legacy 镜像，best-effort）。权威写入唯一走 `meta_commit_write()`（`1482`，由 `seq_reserve_next()` `1577` 调用） |
| 写失败后内存态一致 | `meta_commit_write` 失败 ⇒ 不改 `s_meta_slot`/`s_meta_slot_seq`；`seq_reserve_next` 返回 false ⇒ 调用方置 `s_seq_reliable=false`（`1737`/`2093`），**不写 `s_meta_reserved`** ⇒ 内存不会"以为已预留" |
| 重启后是否复用 | 槽回退到上一成功值 ⇒ `reserved` 回退；**但** `seq_init` 第 ⑤ 步用段内高水位抬高（`1691-1714`，读 `s_seg_last_seq[]`，**零额外 I/O**）⇒ 新区间严格大于段内已用 seq |
| `reserved` 抬高逻辑的边界 | ⚠️ 溢出会**降低** `reserved`（`A1-04`）；除此之外只会抬高 |
| 失败返回是否阻止危险写入 | ✅ 不阻止，但降级为 `seq_reliable=false` 并抬高水位 —— 属既有 §15 语义，可接受 |

**判定**：`R-1` 的**主因**（单块覆写 + 回退即重号）已由"双槽 + 选槽 + 段内高水位"三重机制消除；残余为"段同时不可用"（即 `R-2`）。

### 2.4 `R-2`（元数据与日志段同时丢失）—— **未消除（`BLOCKER`）**

五个必答问题的答复：

| # | 问题 | 答复 |
|---|---|---|
| 1 | 如何区分"真·全新设备"与"存储被清空的旧设备"？ | **无法区分**。`decide_boot_source`（`192`）的输入只有"槽 / legacy / 段 / 损坏计数"四项，**全部位于日志区**。`log_meta_slot.h:179-181` 与实施记录 `L-2` 均已承认该盲区 |
| 2 | `boot_seq=1` 的初始化路径在什么条件下可达？ | `total_records==0 && scan_corrupt==0 && 双槽无效 && legacy 无效`。**除"真·全新"外，任何"日志区被整体清空"都会命中** |
| 3 | 是否存在"设备身份不变、云端有历史、设备端无法恢复序号"的情形？ | **存在，且是常态路径**：`device_id` 由 eFuse MAC 派生（`device_identity.cpp:53`）并持久化在 **NVS**（`:91`），而 `main.cpp:451` 的 `LittleFS.begin(true, …)` 第 1 个实参 = **`formatOnFail = true`** ⇒ **挂载失败即格式化 LittleFS**。格式化后：`/log` 全空、NVS 中的 `device_id` 不变、云端 `log_history` 历史仍在 ⇒ 必然命中 ④ |
| 4 | A-1 是否仍可能生成与历史相同的 `(device_id, boot_seq, seq)`？ | **会，且是确定性的**：`boot_seq=1` + `reserved=0` ⇒ 从 `seq=1` 起分发，与云端 `(dev,1,1…)` 逐条撞 `ux_lh_dedup`；`INSERT OR IGNORE` 静默丢弃 ⇒ **不可恢复的数据损失** |
| 5 | 能否证明 `R-2` 已消除？ | **不能**。只能证明"在日志段仍有可用记录时，风险被消除"（即 ③ 覆盖的场景）。A-1 把 `R-2` 的触发面从"meta 丢失"收窄到"日志区整体失效"，**未关闭** |

**新增的、比 `L-2` 更严重的可达路径（`A1-02`）**：

```
Boot N   : 段文件头/尺寸损坏 → flash_scan_all() 逐个 flash_drop_broken_segment()
           ⇒ LittleFS.remove(path) + s_scan_corrupt++      （log_manager.cpp:707,714）
           双槽 + legacy 也失效 ⇒ ⑤ BOOT_UNRECOVERABLE ⇒ s_seq_failclosed = true
           ⇒ seq_init() 在 1731 提前 return：**不写槽、不写 legacy、不写任何标记**
Boot N+1 : 损坏段已在上一 boot 被删除 ⇒ total_records=0、scan_corrupt=0
           ⇒ ④ BOOT_FRESH ⇒ boot_seq=1 ⇒ 开始撞键
```

即：**唯一阻挡 `R-2` 的 `fail-closed` 状态不落盘，而它赖以判定的证据在同一 boot 内被自己删掉了。** 第二次重启静默回到"全新设备"。这不是理论推演 —— `flash_drop_broken_segment` 的删除是先于 `seq_init` 执行的。

### 2.5 `fail-closed` 行为（`log_manager.cpp:1966`）

| 核实项 | 结论 |
|---|---|
| 持久化状态未确认时是否可靠阻止序号分配 | ✅ **当次 boot 内可靠**。检查位于 `s_ready` 之后、DEBUG 过滤 / 参数编码 / 合并窗口 / 临界区**之前**，`return false` 无任何状态变更、无 Flash I/O、无锁 |
| 是否存在绕过路径 | ✅ 未发现。`s_seq_last++`（`2110`）是唯一分配点，且 `log_emit_internal` 是其唯一入口（`log_emit` 仅转发） |
| 是否持锁 / 已分配资源后返回 | ✅ 否 |
| 是否影响喂食/饮水/主循环/队列 | ✅ 否。副作用仅"少一条日志"；`log_task()` 不受影响；`log_emit` 的返回值历来被多数调用方忽略 |
| 是否可能**永久停写且无法诊断** | ❌ **是**（`A1-03`）。`s_seq_failclosed` 是纯 RAM 静态量（`116`），**永不重置**；`log_flash_get_info()`（`3609` 区）不暴露该标志，`seq_reliable=0` 与"预留失败"不可区分；唯一信号是 `1971` 的一行串口输出；期间不产生任何 batch ⇒ 云端只见"日志停止"，无主动告警 |
| 恢复条件是否明确 | ⚠️ 仅"重启后重新判定"，而重新判定会退化为 ④（`A1-02`）⇒ **所谓恢复恰恰是危险的** |
| 恢复过程中是否复用旧序号 | ❌ 见 `A1-02`：会从 `(1, 1)` 重放 |

**结论：不得把 fail-closed 判定为安全通过。** 它在"当次 boot"是正确且干净的，但**不具备跨重启的持久性**，且**不可观测**。

### 2.6 测试独立性

| 核实项 | 结论 |
|---|---|
| 是否覆盖真实固件调用路径 | ⚠️ **部分**。`log_meta_slot.h` 是**被 `log_manager.cpp` 直接 include 的同一份代码**（非复制），故 `pack/parse/choose/decide` 是真实执行；但**文件 I/O、选槽后的状态更新、`seq_init` 分支体、fail-closed 短路**全部不在探针内（`probe_meta_slot.cpp:15-20` 已明确声明） |
| 测试桩与 LittleFS 实际行为是否一致 | ⚠️ 无桩 —— 探针跑纯逻辑，**不触碰任何文件系统**。因此"写失败/截断/两槽均坏"这类 **I/O 层**用例**完全没有覆盖** |
| 是否缺少文件写入失败 / 截断 / 双槽均坏 / 段部分损坏 / 多次重启组合测试 | ❌ **全部缺失**。`A1-02`（两次重启）与 `A1-05`（静默写丢失）因缺此覆盖而未被发现 |
| 是否存在"仅凭模拟提交标记就推断真实 Flash 原子性"的测试 | ✅ 未发现。`M8` 明确只断言"选择逻辑"；文件头亦声明不证原子性 |
| 旧格式兼容 / 版本迁移 / 边界值断言 | ✅ 有。`M5`（legacy `fmt=2` 隔离）、`M11`（0 / `UINT32_MAX`）、`M12b`（legacy 升级路径） |

**测试断言数量核对**：探针实际 **39 项** PASS（`grep -c "PASS  M"`），而 `HD-3-A1-B3-Implementation-Record.md:181` 记作"**41 项**" ⇒ 文档笔误（`A1-09`）。

---

## 3. B-3 云端审查（阶段 3）

### 3.1 内容比较正确性（`compareLogContent`，`log-ack.js:234`）

| 核实项 | 结论 |
|---|---|
| 五字段全覆盖 | ✅ `ts`/`level`/`code`/`message`/`payload` 逐项比较，任一项不等即 `differs` |
| 数字 vs 数字字符串 | ✅ 归一化**早于**比较：`asInt()`（`history-ingest.js:439`）已把元组中的 `ts`/`code`/`boot_seq`/`seq` 变为 `number`；`level` 由 `LOG_LEVEL_BY_CODE` 映射为**规范字符串**。因此"`\"123\"` vs `123`"在比较层不会出现假等价 |
| `null` / 缺失 / 空串 / 零值 | ✅ `null` 与 `null` ⇒ 等价；`null` 与有值 ⇒ `differs`（`263-270`）；`0` 是合法值（`M11a`）。空串在 `message`/`payload` 上按字面比较（与 `null` 不等价）|
| `message` / `payload` 逐字比较是否符合持久化语义 | ✅ 二者落库均为 `TEXT`（`0003:105,108`），输入侧 `normalizeLog` 只做"原样搬运"（`history-ingest.js:638-650`，非字符串即拒收）⇒ 逐字比较即**存储语义等价** |
| JSON 属性顺序 / 空白 / 转义 / 编码 | ✅ **不适用**：`payload` 存的是**不透明 base64 文本**（不是 JSON 对象），不存在重排/重序列化 ⇒ 不会因属性顺序误报 |
| 是否可能把不同内容判为相同 | ✅ 未发现（当前 8 元组形态下）。唯一理论入口是数值容忍分支接受 `""`/`" 12 "`/`true`（`271-292`），但元组中数值列已由 `asInt` 保证为 `number|null` ⇒ **不可达**；因该函数 `export`，记为 `B3-05` |
| 无法比较时是否归类 `content_unverified` | ✅ 缺列 / 行非对象 / 元组过短 ⇒ `unverifiable`（`238-256`），**不默认相同、也不默认碰撞** |

**证据边界（必须保持的表述）**：本实现只能证明"**两次规范化输入是否逐字一致**"，**不能**还原原始 MQTT 字节（`payload_base64` 之前的内容已不可得）。`payload_base64` 若因编码器差异产生不同表示（如补位/大小写），会被判为 `differs` —— 属"安全的假阳性"，记录为 `B3-06`。

### 3.2 唯一键冲突归因

| 核实项 | 结论 |
|---|---|
| `changes=0` 的成因是否已由 schema 证明 | ✅ 唯一键 `ux_lh_dedup(device_id,boot_seq,seq)`（`0003:126`）+ 全列 `NOT NULL` 约束（`device_id/ts/level/boot_seq/seq`）。`OR IGNORE` 可吞的约束只有唯一键与 `NOT NULL`；`normalizeLog` 已保证非空 ⇒ **实际成因唯唯一键** |
| 读取既有行失败 / 行不存在 / 字段无法比较 | ✅ 三者**分别**落到 `unpersisted`（不发 ACK）/ `unpersisted` / `content_unverified`，均保守 |
| 是否把查询异常当作合法重试 | ✅ **否**。`fetchExistingRow` 异常 ⇒ `found=false` ⇒ `unpersisted` ⇒ `continue`（不 ACK） |
| 是否把不同内容碰撞归入 `same_content` | ✅ 否（见 §3.1） |
| 是否把同一输入的合法重试误计为 `collision` | ✅ 否（真实 D1 实测 `R1b`/`R6b`/`R6d-2` 均 `same=1, collision=0`） |
| 三态是否互斥且都属于 `duplicate` | ✅ 每个 `duplicate` 恰好进入三者之一（`651-680`，`if/else if/else`） |
| 每个分支计数是否恰好 +1 | ✅ 真实 D1 12 个用例逐一验证（`duplicate` 与三细分之和恒等） |
| ACK 是否在冲突未识别前提前确认 | ⚠️ **是**（设计使然）：`duplicate` 分支不区分 verdict ⇒ `collision` 行**同样收到 ACK**（`B3-04`，对应待裁决项 `H-3-7`） |
| 查询次数是否增加 | ✅ **未增加**。`EXIST_SQL` 扩展为 `SELECT 1 AS hit, ts, …`（`149`），仍是**每未新增行 1 次**（L1 `B7` 断言 `existsQueries===3`；真实 D1 每请求 1 条 summary） |

### 3.3 HTTP 响应与可观测性

| 核实项 | 结论 |
|---|---|
| 响应体仍满足冻结四字段契约 | ✅ `{accepted,duplicate,rejected,filtered}`，`history-ingest.js:325` 未改字段集合；L1 `C1/C2/C3` 与真实 D1 **每个用例**均断言 `Object.keys().sort().join(',') === "accepted,duplicate,filtered,rejected"` |
| `T02j` 正向断言是否覆盖字段数量与结构 | ✅ 该断言即"排序后键集合全等" ⇒ 数量与名称同时冻结；`B-3` 未削弱它（`collision` 不进响应） |
| `[log-ack] summary` 是否稳定可关联 | ✅ 每批一行，含 `dev=` 与全部计数；**真实 D1 逐请求取证成功**（新增夹具以此为准） |
| 是否可能被采样 / 截断 / 不可见 | ⚠️ `wrangler.toml`：`[observability.logs] enabled=true · head_sampling_rate=1 · persist=true` ⇒ **不采样、持久化**，但 Workers 日志有**保留期**（非永久）⇒ 历史上限即取证上限 |
| 是否擅改响应契约以暴露 `collision` | ✅ 未改（`history-ingest.js:296-306` 已写明理由与升级条件） |

---

## 4. 新增 / 修改的本地测试（阶段 4）

### 4.1 新增文件（1 个，本报告作者编写）

`guo-feeder-api/test/real_d1/b3_collision_real_d1.sh` —— **真实 D1 引擎**上的 B-3 专用夹具（端口 `8799`，独立 persist 目录 `Cloudflare_Assets/.wrangler/b3_collision/`）。

**为什么必须新写而不是复用 `log_ack_collision.test.mjs`**：L1 套件用内存替身，无法证明 D1 的**行形态**（`INTEGER→number` / `NULL→null` / `TEXT→string`）与 `meta.changes` 语义。本夹具额外断言 `unverified=0`（真实 D1 必回全列）—— 这是 L1 结构上无法覆盖的。

**隔离性自检（脚本内自动断言）**：`--remote` 字面命中 `=0`；本地库**无** `mqtt_messages` 表；`ux_lh_dedup` 真实存在；persist 目录已被 gitignore；测试身份 = 合成 `0b3c0d3e0f01`。

**取证方式**：`collision` 不在响应体 ⇒ 逐请求读取 Worker 日志新增的 `[log-ack] summary` 行（请求前后做行号标记，不依赖累计值）。

### 4.2 修改（**仅本夹具自身**，未触碰实现与既有夹具）

- `b3_collision_real_d1.sh` 的 `q()` 助手加**有界重试**（≤5 次）：`wrangler dev` 与 `d1 execute --local` 并发打开同一 persist 目录时，偶发返回非 JSON（实测约 1/30）⇒ 原写法产生**假阴性**（详见 §5.2）。

### 4.3 未修改（纪律确认）

未改动 `log_ack_collision.test.mjs`、`log_ack*.test.mjs`、`run_all.mjs`、`history_ingest_real_d1.sh`、`v37a_log_reject_matrix.sh`、`retention_real_d1.sh`、任何 migration、任何 `src/**`。

---

## 5. 测试执行与结果（阶段 5）

| # | 命令 | 结果 | 退出码 |
|---|---|---|---|
| 1 | `python test/log_contract/run_contract_test.py`（固件契约 7 步） | `CONTRACT TEST: ALL PASS`；`probe_meta_slot` = **39 PASS / 0 FAIL**（`M1…M12`） | `0` |
| 2 | `pio run -e esp32-s3-devkitc-1` | `[SUCCESS] Took 30.56 s`；RAM **40.5%**（132 712 / 327 680 B）；Flash **66.4%**（1 393 505 / 2 097 152 B） | `0` |
| 3 | `node test/current/log_ack_collision.test.mjs` | **48 PASS / 0 FAIL** / `EXPECTED_FAILURE 0` / `UNEXPECTED_PASS 0` | `0` |
| 4 | `node test/run_all.mjs`（16 套件） | **16/16 `exit=0`**（含 legacy 套件的 `EXPECTED_FAILURE`，属预期） | `0` |
| 5 | `bash test/real_d1/b3_collision_real_d1.sh`（新增） | **52 PASS / 0 FAIL**（第 3 次运行，确定性） | `0` |

### 5.1 新增夹具覆盖矩阵（真实 D1 实测）

| 组 | 用例 | 断言结果 |
|---|---|---|
| `R1` 合法重试 | 同键 + 逐字节相同 | `duplicate=1` · `same=1` · `collision=0` · `unverified=0` · D1 Δ0 · 行数恒 1 |
| `R2` message 冲突 | 同键 + `message` 不同 | `collision=1` · `same=0` · **既有行 `message` 未被改写**（`OR IGNORE` 保留首行） |
| `R3` payload 冲突 | 同键 + `payload` 不同 | `collision=1` · 既有行 `payload` 未被改写 |
| `R4` ts/level/code 冲突 | 三者分别单独变化 | 三次均 `collision=1` |
| `R5` 不同键同内容 | `seq` 不同 + 内容逐字节相同 | `accepted=1` · Δ+1 · `collision=0`（不同键不参与比较） |
| `R6a/b` 全 NULL | `code/message/payload` 全缺 → 重投 | 三列 `IS NULL = 1`；重投 `same=1`（NULL≡NULL） |
| `R6c` NULL vs 有值 | 既有 NULL、本次有值 | `collision=1`、`same=0`（**不视为等价**） |
| `R6d` 数值形态 | `ts` 由 `1792087602` 改为 `"1792087602"` | `same=1`（`asInt` 归一化后等价） |
| `R7` 守恒 | 行数 / 域隔离 | 设备 4 行 · 全表 Δ4 · `telemetry_history` Δ0 |
| 逐用例 | 响应体结构 | **每个用例**均断言恰为四字段且无 `collision` |

### 5.2 执行过程中的失败与处理（**留痕，不删断言**）

| 次 | 结果 | 原因 | 处理 |
|---|---|---|---|
| 1 | 51 PASS / 1 FAIL | **夹具自身缺陷**：隔离自检 `grep -c -- '--remote'` 命中了我写在文件头注释里的同一字面 | 改写注释为"远端标志"，**未削弱断言** |
| 2 | 51 PASS / 1 FAIL | `R1b` 的 `COUNT(*)` 读到空输出：`wrangler dev` 与 `d1 execute --local` 并发争用 persist 目录，偶发非 JSON（**夹具的假阴性**，非实现缺陷） | 给 `q()` 加 ≤5 次有界重试后重跑 |
| 3 | **52 PASS / 0 FAIL** | — | 确定性通过 |

> 结论：两次失败**均源于新写的夹具**，与 `A-1`/`B-3` 实现无关。修的是测试装配，不是被测量对象。

---

## 6. 缺陷清单（阶段 6）

### `A1-01` — `R-2` 未消除：`formatOnFail` 使"全新设备"成为常态误判

| 字段 | 内容 |
|---|---|
| **严重性** | **`BLOCKER`** |
| **位置** | `src/main.cpp:451`（`LittleFS.begin(true, …)`）· `src/log/log_meta_slot.h:192-217`（`decide_boot_source`）· `src/log/log_manager.cpp:1663-1672`（`BOOT_FRESH` 分支）· `src/services/device_identity.cpp:53,91` |
| **触发条件** | LittleFS 挂载失败（掉电导致 FS 元数据损坏即足）⇒ `formatOnFail=true` 自动格式化 ⇒ `/log` 全空、双槽与 legacy 不存在、`total_records=0`、`scan_corrupt=0` ⇒ 命中 `BOOT_FRESH` |
| **预期** | 设备身份未变、云端历史仍在 ⇒ 必须**禁止**从 `boot_seq=1`/`seq=1` 重放（`R-2`） |
| **实际** | `s_boot_seq=1`、`reserved=0` ⇒ 从 `(1,1)` 起分发；云端 `ux_lh_dedup` 逐条命中 ⇒ `INSERT OR IGNORE` **静默丢弃**，不可恢复 |
| **现有测试为何未发现** | 探针只测纯逻辑（`M12d` 把 `(false,false,0,0,0) → BOOT_FRESH` 当作**正确**断言）；无"格式化后重启"用例；`L-2` 只把该盲区写成"段文件被完整删除"这一罕见措辞，**未与 `formatOnFail` 关联**，因此看起来像边角情形 |
| **最小复现** | 真机：`log_init()` 后 `mwipe`（旧钩子）+ 删除全部 `log_*.bin` + 重启；或直接把 LittleFS 分区擦除后重启。观测串口 `[Log] fresh device -> boot_seq=1` 且后续 `(boot_seq,seq)` 与云端历史重合 |
| **影响** | **数据安全（不可恢复）**：日志行被云端静默丢弃；设备端与云端**双双无告警** |
| **建议方向** | 引入**独立于日志区**的持久证据：(a) NVS 已存 `device_id`（`Preferences` 命名空间）⇒ 在同一命名空间存 `boot_epoch`/`has_history` 标志，`LittleFS` 格式化不影响 NVS；(b) 或把"已启动过"标记写入 NVS 并在 `decide_boot_source` 中作为 ⑥ 号输入 —— 有标记而无日志区 ⇒ `UNRECOVERABLE` 而非 `FRESH`；(c) 兜底：`device_identity_init` 已能判断"MAC 派生成功"，可作为"本机曾运行"的弱证据 |
| **需真机/断电验证** | **是**（格式化后重启、掉电挂载失败） |

### `A1-02` — `fail-closed` 不落盘，且判定证据在同一 boot 被删除 ⇒ 第二次重启静默回到"全新"

| 字段 | 内容 |
|---|---|
| **严重性** | **`MAJOR`**（因其**削弱了 A-1 唯一的新增防线**，实质影响接近 `BLOCKER`） |
| **位置** | `log_manager.cpp:1674-1686`（置位 `s_seq_failclosed`）· `1719-1732`（提前 return，**不写任何持久标记**）· `707-718`（`flash_drop_broken_segment` 删除损坏段 + `s_scan_corrupt++`）· `830-848`（`flash_scan_all` 不重置 `s_scan_corrupt`） |
| **触发条件** | Boot N：双槽 + legacy 失效，且所有段头/尺寸损坏（掉电写入风暴后常见）⇒ 段被删除、`scan_corrupt>0` ⇒ ⑤ `UNRECOVERABLE` ⇒ fail-closed；Boot N+1：段已不存在 ⇒ `total_records=0`、`scan_corrupt=0` ⇒ ④ `BOOT_FRESH` |
| **预期** | `BOOT_UNRECOVERABLE` 一旦判定，其结论必须**跨重启保持**（至少"再判一次仍不可恢复"） |
| **实际** | 状态纯 RAM（`116`），永不重置；Boot N+1 判定为全新设备 ⇒ `boot_seq=1` ⇒ 复用历史键 |
| **现有测试为何未发现** | 无"多轮重启组合"用例（`probe_meta_slot.cpp` 不触 I/O，`decide_boot_source` 单次调用即断言）；`M12f` 只验证"`scan_corrupt=1 ⇒ UNRECOVERABLE`"这一次判定，从未把 Boot N 的输出喂给 Boot N+1 |
| **最小复现** | 真机：`logt mfail 1` + 破坏两个段头 + `mwipe`（并删除 legacy）⇒ 观察 Boot N 串口 `boot_seq unrecoverable -> fail-closed`，重启一次 ⇒ 观察 `fresh device -> boot_seq=1` |
| **影响** | 可用性：Boot N 期间日志**全停**（有意）；数据安全：Boot N+1 起**产生撞键**（`A1-01` 同后果） |
| **建议方向** | 在 `BOOT_UNRECOVERABLE` 分支**持久化隔离标记**（例如写入一个 `/log/quarantine.bin`，或在两槽各写一条 `commit=0` + `corrupt` 记录），并把该标记作为 `decide_boot_source` 的第 6 号输入 ⇒ 只要标记存在就永不判 `FRESH`。**注意**：不可用"写槽 commit=1 + boot_seq=0"的方式，否则下次会被读成 `boot_seq=1` |
| **需真机/断电验证** | **是** |

### `A1-03` — fail-closed 在设备运行期**不可远程观测**

| 字段 | 内容 |
|---|---|
| **严重性** | **`MAJOR`**（可观测性） |
| **位置** | `log_manager.cpp:116`（标志）· `1966-1976`（唯一串口输出）· `3586-3615`（`log_flash_get_info` 未含该标志）· `main.cpp:1034`（仅串口 `[LogT]` 测试指令读取） |
| **触发条件** | 任意进入 fail-closed 的设备 |
| **预期** | 停止写日志属重大降级，应可通过 MQTT 状态/命令查询区分 |
| **实际** | 只有 `seq_reliable=0`，与"`meta` 写失败"**不可区分**；无 MQTT 上报；且 fail-closed 期间不产生 batch ⇒ 云端只表现为"日志静默" |
| **现有测试为何未发现** | 无"状态可观测性"测试；`LogFlashInfo` 未随 A-1 扩展 |
| **最小复现** | 进入 fail-closed 后 `logt meta`（串口）与常规状态查询对比 |
| **影响** | 运维：部署后若发生 fail-closed，**无人会发现**（设备其余功能完全正常） |
| **建议方向** | `LogFlashInfo` 增加 `seq_failclosed` 字段并纳入既有状态上报；或令 fail-closed 只抑制 `INFO/WARN`、放行 `CRITICAL`（用固定 `boot_seq=0xFFFFFFFF` 的一次性告警记录），使"停写"本身可被云端看见 |
| **需真机验证** | 是（中） |

### `A1-04` — 高水位算术在 `0xFFFFFFFF` 端点**溢出即降级**

| 字段 | 内容 |
|---|---|
| **严重性** | **`MINOR`** |
| **位置** | `log_manager.cpp:926`（读末条失败 ⇒ `s_seg_last_seq[i]=0xFFFFFFFF`）· `1706-1708`（`((seg_max + 999)/1000)*1000`） |
| **触发条件** | 某段末条记录读失败（`f.seek`/`f.read` 失败） |
| **预期** | 注释（`1691-1694`）称"取 max 只会抬高、**不会降低**" |
| **实际** | `0xFFFFFFFF + 999` 在 `uint32` 回绕为 `998` ⇒ `/1000*1000 = 0` ⇒ `reserved` **降为 0** |
| **现有测试为何未发现** | 探针不含该算式；无"末条读失败"注入 |
| **最小复现** | 单测：令 `s_seg_last_seq[x]=0xFFFFFFFF` 后调用 `seq_init` 的高水位段（或抽出该算式做纯函数测试） |
| **影响** | 不直接致碰撞（`boot_seq` 仍递增）；破坏"只增不减"不变量，使"预留高水位"失去意义 |
| **建议方向** | 判 `seg_max >= 0xFFFFFFFF - LOG_SEQ_RESERVE` 时跳过抬高（或饱和到 `0xFFFFFFFF - 999`），并显式处理 `0xFFFFFFFF` 这一"未知"哨兵值 |
| **需真机验证** | 否（可纯函数验证） |

### `A1-05` — 无法检测"写入返回成功但未落盘"

| 字段 | 内容 |
|---|---|
| **严重性** | **`MINOR`** |
| **位置** | `log_manager.cpp:1372-1393`（`meta_slot_write_raw`：仅校验 `f.write()==32`）· `1469-1500`（`meta_commit_write`） |
| **触发条件** | LittleFS 写缓存提交失败 / `close()` 内部 I/O 错误（Arduino `File::close()` **无返回值**，无法检查） |
| **预期** | 提交成功应意味着"新状态已持久" |
| **实际** | 函数可能返回 `true` 而槽内容仍是旧值；`s_seq_reliable` 保持 `true`（`seq_reserve_next:1586`） |
| **现有测试为何未发现** | 无 I/O 层注入；`s_meta_fail_next` 是"全有/全无" |
| **最小复现** | 真机：注入 `LittleFS` 写失败（需额外钩子）；或纯逻辑模拟"读出仍是旧槽" |
| **影响** | 内存与 Flash 信念分叉；重启后 `reserved` 回退 ⇒ 依赖 §2.5 的段内高水位兜底（**该兜底本身依赖段完整**） |
| **建议方向** | 写后**回读校验**（32 B，成本可忽略）：`meta_slot_read()` 成功且 `slot_seq/commit` 匹配才返回 true |
| **需真机验证** | 是（低优先） |

### `A1-06` — 两阶段提交只有"全有/全无"注入；板上测试钩子语义已失效

| 字段 | 内容 |
|---|---|
| **严重性** | **`MINOR`**（测试资产） |
| **位置** | `log_manager.cpp:1472-1476`（注入点）· `3739-3747`（`log_meta_wipe` 只删 legacy `meta.bin`）· `3749+`（`log_meta_corrupt` 同上） |
| **触发条件** | 使用 `logt mwipe` / `mcorrupt` / `mfail` 做上板验证时 |
| **预期** | 钩子应能模拟"元数据丢失/损坏"与"① 成功 ② 失败" |
| **实际** | `mwipe`/`mcorrupt` 只作用于 legacy 镜像 ⇒ 槽仍有效 ⇒ 仍判 ① `BOOT_FROM_SLOT` ⇒ **假 PASS**；`mfail` 在 ① 之前整体失败，**无法**注入"② 失败（半写槽留在盘上）"这一最关键中间态 |
| **现有测试为何未发现** | 钩子不在自动测试内，无元测试 |
| **最小复现** | 上板：`logt mwipe` 后重启，观察串口仍为 `boot=…`（未走重建路径） |
| **影响** | 后续真机验证可能得出**错误的"已覆盖"结论** |
| **建议方向** | 钩子扩展为 `mwipe all|legacy|slot_a|slot_b`，`mfail` 增加 `stage=1|2` 参数 |
| **需真机验证** | 是 |

### `A1-07` — `seq_reserve_next` / `boot_seq++` 无溢出保护

| 字段 | 内容 |
|---|---|
| **严重性** | **`NIT`** |
| **位置** | `log_manager.cpp:1579-1580`（`base` / `next_reserved`）· `1717`（`s_boot_seq = boot + 1`） |
| **触发条件** | `reserved` 或 `boot` 达 `UINT32_MAX` |
| **预期/实际** | 预期饱和或拒绝；实际回绕（`boot=UINT32_MAX ⇒ boot_seq=0`，`reserved` 回绕到小值） |
| **现有测试** | `M11` 只测 `pack/parse` 端点往返，未测算式 |
| **影响** | 4e9 次开机 / 4e9 条日志，**实际不可达**；仅记录 |
| **建议** | 饱和处理 + 断言日志 |

### `A1-08` — legacy 镜像写失败会**破坏可回滚性**

| 字段 | 内容 |
|---|---|
| **严重性** | **`NIT`** |
| **位置** | `log_manager.cpp:1498`（`(void)meta_write(...)`）· `1342-1365`（`open("w")` 截断） |
| **触发条件** | 提交成功后镜像写失败或被截断 |
| **影响** | 旧固件回滚时读到损坏/缺失的 `meta.bin` ⇒ 旧固件走它自己的重建路径（其重建**不含** `boot_seq` 保护）⇒ 回滚本身成为 `R-2` 的触发器 |
| **建议** | 镜像改为"写入临时文件 + rename"，或至少在镜像失败时置 `s_seq_reliable=false` 留痕 |

### `A1-09` — 文档笔误：探针断言数 `41` ≠ 实际 `39`

| 字段 | 内容 |
|---|---|
| **严重性** | **`NIT`** |
| **位置** | `docs/architecture/HD-3-A1-B3-Implementation-Record.md:181` |
| **证据** | `python test/log_contract/run_contract_test.py \| grep -c "PASS  M"` ⇒ **39**（`M1`×5 + `M2`×1 + `M3`×1 + `M4`×1 + `M5`×1 + `M6`×3 + `M7`×6 + `M8`×2 + `M9`×2 + `M10`×4 + `M11`×6 + `M12`×7） |
| **建议** | 改为 39，或在报告中注明"39 项断言 / 12 组" |

### `B3-01` — `fetchExistingRow().ok` 未被使用（死字段 + 查询异常不可计数）

| 字段 | 内容 |
|---|---|
| **严重性** | **`MINOR`** |
| **位置** | `src/log-ack.js:183-203`（返回 `{ok,found,row}`）· `651-680`（仅读 `found`） |
| **触发条件** | D1 查询异常 |
| **预期** | `ok=false` 表示"查询失败"⇒ 应按设计（`188` 注释）与"确实不存在"区分统计 |
| **实际** | 两者都进 `unpersisted`，`ok` 无任何读者 ⇒ 查询异常率**不可观测** |
| **现有测试为何未发现** | `B4` 只断言 `unpersisted=1 / attempted=0`（行为正确），不要求区分 |
| **影响** | 可观测性：D1 抖动与"设备重投到不存在的键"无法区分，排障成本上升 |
| **建议** | summary 增 `exists_unavailable` 计数（仅日志，不进响应体），或直接删除 `ok` 字段以免误导 |
| **需真机验证** | 否 |

### `B3-02` — `content_unverified` 在真实 D1 下恒为 0，其守护价值未被激活

| 字段 | 内容 |
|---|---|
| **严重性** | **`MINOR`** |
| **位置** | `src/log-ack.js:234-256` |
| **证据** | 新增夹具 12 个用例（真实 D1）全部 `unverified=0`；L1 中它只在"替身只回 `{hit:1}`"时出现 |
| **预期/实际** | 预期该分支守护"内容列不可读"；实际生产中 D1 必回全列 ⇒ 该分支只在**未来 schema 漂移**（列改名/删除）时触发 |
| **影响** | 不是缺陷，而是**观测信号未接线**：真正发生漂移时 `unverified` 会暴涨，但当前无阈值/告警 |
| **建议** | 为 `unverified > 0` 增加一次性 `console.error`（区别于逐行 `warn`），使漂移可在 tail 中一眼可见 |

### `B3-03` — L1 套件三处断言/注释质量问题

| 字段 | 内容 |
|---|---|
| **严重性** | **`MINOR`**（测试资产） |
| **位置** | `test/current/log_ack_collision.test.mjs:461-463` · `:412-413` · `:288` |
| **问题** | ① `C4` 用 `same+collision+unverified <= duplicate` —— 若实现**一个都不计**也会 PASS，应改 `===`；② `C2` 的"内容细分在 summary 层可见"实为重复断言 `duplicate===1`，属**空断言**；③ C 组标题仍写"响应体的加性字段（`collision` / `collision_unchecked`）"、`C3` 标题写"`collision=null`"，与实现（**响应体不加字段**）**相反** |
| **现有测试为何未发现** | 断言与被测行为同源，无人质疑 |
| **影响** | 未来若细分计数被改坏（如漏计），`C4` 不会报警；注释会误导后续维护者以为契约已变 |
| **建议** | ① 改 `===`；② 删除或替换为对 summary 行的真实断言；③ 改正三处标题 |

### `B3-04` — 碰撞行仍收到 ACK（语义待裁决，对应 `H-3-7`）

| 字段 | 内容 |
|---|---|
| **严重性** | **`QUESTION`** |
| **位置** | `src/log-ack.js:651-680`（`duplicate` 分支不区分 verdict，仍进入发布流程） |
| **触发条件** | 键存在但内容不同（`collision`） |
| **预期（若 ACK 语义 = "该条已持久化"）** | 内容未被持久化 ⇒ **不应** ACK |
| **实际** | 与 `same_content` 同样 ACK ⇒ 设备停止重投 ⇒ 该条真实内容**永久丢失**，唯一痕迹是 Worker 日志 |
| **现有测试为何未发现** | `B2` 明确断言"仍允许 ACK（键存在已是既有权衡）"，把该行为当作**预期** |
| **影响** | 数据安全：撞键内容丢失（与 `A-1` 的后果同源，但此处由云端**主动确认**） |
| **建议** | 与 `H-3-7` 一并裁决：若 ACK 语义收紧为"内容已持久化"，则 `collision` 应走 `unpersisted`（不 ACK）；否则必须在文档中把 ACK 定义为"键存在"并接受该损失 |
| **需真机验证** | 否（协议语义问题） |

### `B3-05` — `compareLogContent` 数值容忍分支对 `""` / 空白 / 布尔亦生效

| 字段 | 内容 |
|---|---|
| **严重性** | **`NIT`** |
| **位置** | `src/log-ack.js:271-292` |
| **触发条件** | 任一侧为 `number` 且另一侧为 `""`/`" 12 "`/`true` |
| **预期/实际** | 预期只容忍"数字 vs 数字字符串"；实际 `Number("")===0`、`Number(" 12 ")===12`、`Number(true)===1` |
| **现状** | 当前 8 元组中数值列已由 `asInt` 归一化 ⇒ **不可达**；但该函数是 `export` 的公共 API |
| **建议** | 收紧为"仅当另一侧匹配 `/^-?\d+$/` 时走数值比较"，否则 `differs` |

### `B3-06` — `payload_base64` 不同编码表示会被判为 `collision`

| 字段 | 内容 |
|---|---|
| **严重性** | **`NIT`**（安全的假阳性） |
| **位置** | `src/log-ack.js:253`（逐字比较） |
| **说明** | 同一字节序列若以不同 base64 变体（补位/大小写/换行）表示，判 `differs` ⇒ 计入 `collision`。方向安全（不会漏报），但会产生噪声 |
| **建议** | 在文档中明确"`collision` 是**候选**，可能包含重编码"，或在比较前对 `payload` 做 base64 规范化（需先确认不破坏"不解析 payload"的冻结约束） |

---

## 7. 未能通过静态检查或模拟测试证明的风险

| # | 风险 | 为何不能证明 | 需要什么 |
|---|---|---|---|
| `R-1` | LittleFS 两次写之间的**掉电原子性** | 探针跑 `-nostdlib` 纯逻辑，**不触碰文件系统**；`open("w")` 的截断时序与 32 B 编程的中间态无法在主机复现 | **真机断电测试**（建议 100+ 次随机窗口切断） |
| `R-2` | `formatOnFail` 挂载失败的实际概率与后果 | 需要可控地破坏 FS 元数据 | 真机（`LittleFS.end()` + 写坏 + 重启） |
| `R-3` | `A1-02` 的"两次重启"链路 | 需要真实 Flash 与真实删除行为 | 真机（两轮重启，串口取证） |
| `R-4` | D1 查询异常分支（`ok=false`） | 真实 D1 上无法**安全地**制造查询异常（会破坏表） | 仅 L1 替身可覆盖（已在 `B4` 覆盖） |
| `R-5` | `meta.changes` 在 `PERSIST_CHUNK > 1` 批里的语义 | 本轮真实 D1 用例均为**单条**；多条批的 `changes` 组合未取证 | 追加 real_d1 多行批用例（未在本轮授权范围的"至少场景"内，故未做） |
| `R-6` | 生产 `ux_lh_dedup` 与 `0003` 是否逐字一致 | 本轮禁止生产只读查询 | 已有 RET-1 轮次的只读取证（`EXPLAIN` 命中 `ix_lh_device_ts`），但**未**直接核对索引 DDL |
| `R-7` | Worker 日志保留期是否覆盖排障窗口 | 取决于账户计划（Free 保留期短） | 产品/运维确认；若需长期留痕须另建 D1 表（属契约变更，需授权） |

> **纪律声明**：以上各项**不得**以"没有复现"表述为"问题不存在"。

---

## 8. 安全边界执行确认

| 约束 | 状态 |
|---|---|
| 生产 D1 查询 / 写入 | **未执行**（全部 `d1 execute` 均带 `--local --persist-to`；夹具内自动断言远端标志命中 = 0） |
| Worker / Pages 部署 | **未执行** |
| EMQX / Cron / 生产配置 | **未改动** |
| 设备命令 / 重启 / 烧录 / OTA | **未执行**（仅本地 `pio run` 编译，不涉及 `upload`） |
| 已应用 migration 修改 | **未修改**（`0003` 仅阅读） |
| Git 操作 | **未执行**（无 add/commit/push/reset/clean/stash/checkout/branch） |
| 其他任务工作区差异 | **未被覆盖/格式化/清理**（`HANDOFF.md`、`docs/README.md`、`P3-*`、`未修复的问题.md` 保持原状） |
| 实现代码修改 | **未修改**（`log_manager.*`、`log_meta_slot.h`、`log-ack.js`、`history-ingest.js` 均为审查前原样） |
| RET-1 / WP-6 / 无关文档 | **未修改** |
| 本轮新增文件 | 仅 2 个：本报告 + `test/real_d1/b3_collision_real_d1.sh` |

---

## 9. 独立验收判定

### 9.1 A-1 —— **`BLOCKED`**

**理由（对照授权判据）**：
- 判据原文："如果 A-1 仍无法证明 `R-2` 安全……不得判定对应子项 `ACCEPTED`"。
- `R-2` **未被证明安全**（`A1-01`），且触发路径不是边角情形 —— 而是**平台自身代码**（`formatOnFail=true`）在任一挂载失败时主动造成的。
- 更严重的是 `A1-02`：A-1 新增的 `fail-closed` 是其声称的"最后一道门"，但它**不落盘**，而判定证据在**同一个 boot 内被自己删除** ⇒ 第二次重启静默回到不安全状态。这使"已 fail-closed"的表述具有**误导性**。

**可以确认的正面结论**（不因 BLOCKED 而抹去）：
- `R-1` 已**实质消除**：双槽两阶段提交 + `slot_seq` 选槽 + 段内高水位三重机制，逻辑正确、探针可证、编译通过。
- `fail-closed` 在**当次 boot** 内实现干净（无锁、无 I/O、无状态变更、无旁路）。
- 恢复优先级 ①②③ 的语义与实现一致，`legacy fmt=2` 隔离正确。

**解除条件（建议，需人工批准）**：
1. 关闭 `A1-02`（持久化隔离标记 + 作为恢复判据的第 6 号输入）—— **最小、可独立验证**；
2. 关闭 `A1-01`（NVS 侧独立证据）或**显式**把 `formatOnFail` 改为 `false` 并把"挂载失败"定义为设备不可用（会改变整机可用性语义，须产品裁决）；
3. `A1-03`（可观测性）至少给出"部署后如何发现 fail-closed"的书面运维手段。

### 9.2 B-3 —— **`CONDITIONAL`**

**通过项**：内容比较语义、三态互斥、计数守恒、查询次数不变、ACK 契约四字段冻结 —— 均在**真实 D1** 上 52/52 验证；无"不同内容判为相同"的路径。

**附条件（不阻断本地保留，阻断"视为已验收"）**：
1. `B3-03` 的 `<=` 改 `===`、删除空断言、修正 3 处过期标题（属测试资产，成本极低）；
2. `B3-04` 必须与 `H-3-7` 一并裁决（碰撞行是否 ACK），并在文档中把 ACK 语义**一次讲清**；
3. 文档需注明 `content_unverified` 在真实环境**常态为 0**，其价值在于 schema 漂移告警而非日常分类（`B3-02`）。

### 9.3 整体 HD-3 —— **`BLOCKED`**

`B-3` 已具备"可交付本地"的质量，但 HD-3 的**同一故障**（键碰撞导致日志丢失）在设备端仍有可复现的入口。`B-3` 只是"事后能看见"，而 `A-1` 失效后 `B-3` 的 `collision` 也**未必**能看见，存在一个完全静默的组合：

> 设备经历 `formatOnFail` 格式化 ⇒ `boot_seq=1`、`seq` 从 1 起；若此时 **RTC 未同步 ⇒ `ts=0`**（`RET-1` 已证明该状态真实存在），且记录为**内容确定的启动期事件**（同一 `level`/`code`/`message`/`payload`），则云端在 `(dev,1,1)` 上取到的既有行与本条输入**五字段全等** ⇒ `compareLogContent` 判为 `same_content` ⇒ 计入 `duplicate` 并**照常 ACK**。
>
> 即：**撞键 + 内容恰好一致 ⇒ `A-1` 的 `R-2` 与 `B-3` 的 `same` 分类合并为"完全静默的数据丢失"**，`collision` 计数仍为 0。`ts` 一旦已同步（非 0），同一路径会退化为 `collision`（可见），故此风险集中在"格式化后首次联网前"这段时间窗。

---

## 10. 建议的下一步（按优先级，均需人工授权）

| 顺序 | 动作 | 依据 |
|---|---|---|
| 1 | 修 `A1-02`（隔离标记持久化）—— 改动最小、可纯逻辑 + 真机双验证 | 阻断级 |
| 2 | 修 `A1-01`：NVS 侧"曾运行"证据，或改写 `formatOnFail` 策略 | 阻断级 |
| 3 | 补 `A1-03` 可观测性（`LogFlashInfo` 增字段 + 状态上报） | 重大 |
| 4 | 补 A-1 的 I/O 层与"两次重启"测试（当前结构上缺失） | 防止 `A1-02` 类缺陷复发 |
| 5 | 收紧 `B3-03` 断言；裁决 `B3-04`（`H-3-7`） | 验收前置 |
| 6 | `A1-04` / `A1-05` 防御性修复（溢出哨兵、写后回读） | 加固 |
| 7 | 真机断电测试计划（`R-1`/`R-2`/`R-3`）—— 属设备验证范畴，需单独授权 | 证据缺口 |

**本轮到此停止，等待下一步指令。**
