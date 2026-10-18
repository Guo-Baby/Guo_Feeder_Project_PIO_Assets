# `HD-3` · `A-1` / `B-3` 实施记录（**本地实现 · 未部署**）

> ## 状态：**`IMPLEMENTED - LOCAL ONLY`**（`2026-10-18`）
>
> - **已实现**：云端 `B-3`（冲突内容检测 + `collision` 计数）· 固件 `A-1`（双槽元数据 + 恢复优先级 + fail-closed）
> - **未部署**：Worker 未发布；固件**未烧录**（`P2` = `HARDWARE DEFERRED`）
> - **未改**：`migrations/` · `wrangler.toml` · `docs/interfaces/cloud_protocol.md` · 设备日志信封 · 冻结响应字段集
> - **门禁未变**：`BT-1` = `OPEN` · `G-4` = `BLOCKED` · `WP-5` 未解锁 · `STOP-R1` 无书面解除
> - **定级未变**：`log` / `telemetry` = `CODE-PROVEN RISK`；"是否曾发生真实内容碰撞/丢失" = **`UNRESOLVED`**
>
> **上位文档**：`HD-3-Idempotency-Collision-Implementation-Plan.md`（`rev.3` · 含 §15.10 生产只读实测）

---

## §1 `B-3` · 云端冲突内容检测与 `collision` 计数

### 1.1 比较语义（三态互斥）

| 判定 | 含义 | 常量 |
|---|---|---|
| **`same`** | 键存在 **且** 五个字段全部相等 ⇒ **合法重试已证** | `CONTENT_SAME` |
| **`differs`** | 键存在 **且** 至少一个字段不等 ⇒ **候选碰撞** | `CONTENT_DIFFERS` |
| **`unverifiable`** | 键存在 **但** 无法比较（缺列 / 输入元组不完整） | `CONTENT_UNVERIFIABLE` |

**比较字段（五个全比）**：`ts` · `level` · `code` · `message` · `payload`

| 字段 | 归一规则 | 为什么这样定 |
|---|---|---|
| `ts` / `code` | 数值比较（容忍 `"110903"` 与 `110903` 的形态差；非有限值 ⇒ `differs`） | D1 可能以字符串回传数字；**不因类型形态误报** |
| `level` | 字符串逐字 | 已成枚举（`WARN`/`ERROR`/`CRITICAL`） |
| `message` | 字符串逐字 | 不做 JSON 重排、不做 trim ⇒ **不因序列化形式误报** |
| `payload` | **字符串逐字**（base64 文本） | 存的是 `rec.payload_base64`，**不是**原始 MQTT 字节；本模块不解码、不假设它是字节 |
| 一方 `NULL` 一方有值 | `differs` | `NULL` 不等于"等价" |

> **★ 关键**：`differs` 只说明"**同键的两次输入内容不同**"。
> 它**不能**区分"不同日志撞键"与"同一条日志被重新编码后重发"（后者内容也会不同），
> 也**不能**推断"哪一条才是真实日志"、更**不能**推断"发生过丢失"。

### 1.2 查询次数：**零新增**

| 项 | 改动前 | 改动后 |
|---|---|---|
| 存在性查询 | `SELECT 1 AS hit … LIMIT 1`（每条未新增行 1 次） | `SELECT 1 AS hit, ts, level, code, message, payload … LIMIT 1`（**仍 1 次**） |
| 每请求新增查询数 | — | **0** |

**为什么保留 `SELECT 1` 前缀**：既有 5 个 L1 套件的 D1 替身用 `norm.startsWith("SELECT 1")` 识别该查询。
把前缀改成 `SELECT ts, …` 会让替身把查询当"未知读操作"，**无谓地破坏既有测试资产**。
⇒ 用 `SELECT 1 AS hit, ts, level, code, message, payload` 同时满足"前缀不变"与"一次取回内容"。

### 1.3 计数口径（**子集关系，不是互斥分区**）

```
duplicate          = 未新增行 且 键已存在          ← **语义未变**
  ├─ same_content      = duplicate 中内容相同（合法重试）
  ├─ collision         = duplicate 中内容不同（候选碰撞）
  └─ content_unverified = duplicate 中无法比较
unpersisted        = 未新增行 且 无法证明键存在（查询失败或真不存在）⇒ 不 ACK
```

| 关系 | 说明 |
|---|---|
| `same + collision + unverified` ≤ `duplicate` | 三者是**已比较/键存在**的细分子集 |
| `collision` ⊄ 互斥于 `duplicate` | `collision` **不替代** `duplicate`，两者**同时**上报 |
| `unpersisted` 与 `duplicate` | **互斥**（键是否存在） |

### 1.4 ⚠️ **偏差申报**：`collision` **不进入 HTTP 响应体**

原计划把 `collision` 作为加性字段放进 `/api/history/ingest` 响应。**实施时发现与冻结契约冲突**：

- `BT-1` 的 **`T02j`** 断言"响应体**恰为**四字段（`accepted` / `duplicate` / `rejected` / `filtered`）"，
  且该断言是**既有实测通过的套件**（`test/current/log_ack.test.mjs`，60 条）。
- 加字段 ⇒ `T02j` 变红。**为让测试通过而修改该断言** = 削弱已验收契约，**不做**。

⇒ **实际落点**（两处，均不触碰响应契约）：
1. **每批一行结构化日志**：`[log-ack] summary … same=… collision=… unverified=…`（`wrangler tail` 可观测）
2. **`ackPersistedLogs()` 的返回值**：`{ same_content, collision, content_unverified, … }`（供测试与内部调用方）

> **后果（必须知晓）**：上游（Pages / 设备）**看不到** `collision`；
> 要对外暴露必须先修订 `T02j`（属契约变更，需单独授权）。
> 新增套件 **C1/C2/C3** 已把"响应体仍恰为四字段"作为**正向断言**写死，防止后续误加。

### 1.5 剩余盲区

| # | 盲区 | 原因 |
|---|---|---|
| `B-3-a` | 只覆盖**本请求内**未新增的行 | 跨请求的历史重复无法回溯 |
| `B-3-b` | 无法区分"重新编码重发"与"不同日志撞键" | 只能比内容，不能比来源 |
| `B-3-c` | 对比依赖 `log_history` 现存行；该行若被 retention 删除，后续同键会**重新插入**，不再有"冲突" | 唯一键已随行消失（`RET-1` 域） |
| `B-3-d` | `payload` 逐字比较 ⇒ 同一逻辑日志若 payload 内嵌 `uptime`，重发会被判 `differs`（**误报候选**） | 固件是否重编码未验证 |
| `B-3-e` | 查询失败的行记 `unpersisted`，不进 `collision` | 保守（无法证明键存在） |

---

## §2 `A-1` · 固件日志序号持久化与恢复保护

### 2.1 问题（`R-1` / `R-2` 的根因）

| 路径 | 机制 | 后果 |
|---|---|---|
| **`R-1`** | `meta.bin` 单块覆写，写失败/掉电后仍是**上一次**的值 ⇒ 两个 boot 读到同一 `boot_seq`，并各自从同一号段起分 | 同 `(boot_seq, seq)` 被两次 boot 使用 ⇒ 撞键 |
| **`R-2`** | `meta.bin` 与段同时丢失 ⇒ 旧代码 `boot=0` ⇒ `s_boot_seq = 1` | **从 1 重放**，与云端历史撞键 |

### 2.2 设计：双槽 + 两阶段提交

| 项 | 实现 |
|---|---|
| 文件 | `/log/meta_a.bin` · `/log/meta_b.bin`（各 **32 B**） |
| 布局 | 复用 legacy 偏移；把原本空着的 `[20..27]` 用起来：`20 slot_seq`（提交序号）· `24 commit`（1=有效）· `28 crc32`（覆盖 `[0..27]`） |
| 格式版本 | **`fmt = 3`**（legacy = 2）⇒ 旧文件不会被误读为槽 |
| 提交顺序 | ① 写**非当前有效槽**，`commit=0`（先让目标槽失效）→ ② 再写 `commit=1`（原子生效点）→ ③ best-effort 镜像 legacy |
| 选槽 | 两槽均有效 ⇒ `slot_seq` 大者胜；相等取 A（确定性）；仅一有效 ⇒ 用之；都无效 ⇒ 回退 legacy |
| 掉电安全性 | ① 之前/之中掉电 ⇒ 旧槽完好；①②之间掉电 ⇒ 目标槽 `commit=0` ⇒ 无效 ⇒ 回退旧槽 |

**不改的东西**（重要）：`LOG_META_SIZE` 仍 32 B、`LOG_META_CRC_SPAN` 仍 28、**不改记录格式、不改日志信封**。

### 2.3 恢复优先级（`log_meta_slot::decide_boot_source`）

| 优先级 | 来源 | `boot_seq` 取值 | 安全依据 |
|---|---|---|---|
| ① | 双槽有效 | `slot.boot_seq + 1` | 槽内已是上次成功提交值 |
| ② | legacy `meta.bin` 有效 | `legacy.boot_seq + 1` | 升级迁移路径（旧固件只写过它） |
| ③ | 段内存在 `boot_seq > 0` | **`max(段内 boot_seq) + 1`** | ★ `LogRecord.boot_seq`（`LOG_OFF_BOOT_SEQ = 8`）本就在记录里 |
| ④ | 无段记录 **且** 无损坏痕迹 | `1` | **全新设备**，无本地历史 |
| ⑤ | 其余 | — | **`BOOT_UNRECOVERABLE` ⇒ fail-closed** |

> **④ 与 ⑤ 的分界是本设计唯一允许"从 1 开始"的场景。**
> 只要存在**任何**历史痕迹（段记录数 > 0 或 `scan_corrupt > 0`），就不允许从 1 重放。

### 2.4 额外的 `seq` 保护：段高水位抬高 `reserved`（零 I/O）

`meta` 写失败时，持久化的 `seq_reserved` 会**落后**于段内实际用过的 `seq`。
若不修正，重启后会**重用已用过的号**（即使 `boot_seq` 正常，仍会撞键）。

修法：每次 `seq_init()` 用 `s_seg_last_seq[]`（`flash_scan_all()` 已建好的内存数组）求最大值，
若大于持久化 `reserved` 则**向上对齐抬高**。

| 性质 | 说明 |
|---|---|
| I/O 成本 | **0**（纯内存数组；不额外扫段） |
| 单调性 | **只抬高、不降低** ⇒ 对正常路径无副作用 |
| 覆盖场景 | `R-1` 的残余（meta 落后但段完好） |

### 2.5 fail-closed（`s_seq_failclosed`）

| 项 | 实现 |
|---|---|
| 触发 | `BOOT_UNRECOVERABLE`（双槽 + legacy 全失效，且段内无 `boot_seq`，且非全新设备） |
| 行为 | `log_emit_internal()` **开头直接 `return false`**：不分配 `seq`、不进 RAM 环、不落 Flash、不上云 |
| 阻塞性 | **无** —— 不加锁、不做 I/O、不等延时；只在首次触发打一行串口 |
| 上报 | 既有 `LogStats.seq_reliable = 0` 随状态查询暴露（**不新增字段**，不改 `LogStats` 布局） |
| 取舍 | 宁可少记一条日志，也不产生**可能与历史撞键**的 `(boot_seq, seq)` |

### 2.6 磨损 / 掉电 / 失效分析

| 场景 | 行为 |
|---|---|
| **擦写磨损** | 每次提交 = 2 次 32 B 槽写 + 1 次 32 B legacy 写。提交频率 = 每 **256 条日志** 一次（`LOG_SEQ_RESERVE`）。按实测 57 行/天 ⇒ **约 4.5 天一次** ⇒ 每天 < 0.05 次擦除，可忽略 |
| **单槽损坏** | 另一槽仍有效 ⇒ 正常恢复（探针 `M7d`/`M7e`） |
| **双槽损坏** | 回退 legacy；legacy 也坏 ⇒ 段恢复；段也无线索 ⇒ fail-closed（`M9b` + `M12e/f`） |
| **掉电在两次写之间** | 目标槽 `commit=0` ⇒ 无效 ⇒ 回退旧槽 ⇒ **旧状态不丢**（`M2` + `M8b`） |
| **写入失败（非掉电）** | `meta_commit_write()` 返回 false ⇒ 调用方 `s_seq_reliable=false` 并**继续**用当前区间（不阻塞主循环）；下次启动由段高水位修正 |
| **槽文件不存在** | 走 ②/③/④/⑤ 判定（不视为错误） |

### 2.7 回滚兼容与限制

| 项 | 结论 |
|---|---|
| 旧固件回滚 | 旧固件只认 `/log/meta.bin` ⇒ 每次提交后 **best-effort 镜像**该文件（保持 `fmt=2`）⇒ 回滚可读到**最近一次成功值** |
| **`L-1`（限制）** | 若 legacy 镜像写失败，回滚后的旧固件会读到**更旧**的 `boot_seq` ⇒ 可能重放。属**已知限制**，不掩盖 |
| **`L-2`（限制）** | 段文件被**完整删除**且无损坏计数 ⇒ 会被判为"全新设备"（④）⇒ `boot_seq=1`。固件没有独立于日志区的"曾经存在"证据，**无法区分"真·全新"与"数据被清空"** |
| 前向兼容 | 新固件优先双槽，legacy 只读不写（除镜像）⇒ 旧文件可被采纳（②） |

---

## §3 测试证据（本地）

| 层 | 套件 / 步骤 | 结果 |
|---|---|---|
| 云端 L1（新） | `test/current/log_ack_collision.test.mjs` | **48 PASS / 0 FAIL** |
| 云端 L1（全量） | `node test/run_all.mjs` | **16 套件全部 exit=0** |
| 固件契约（新） | `test/log_contract/run_contract_test.py` **第 [7] 步** `probe_meta_slot.cpp` | **41 项全部 PASS**（`M1…M12`） |
| 固件契约（全量） | 同上，7 步 | **`CONTRACT TEST: ALL PASS`** |
| 固件编译 | `pio run -e esp32-s3-devkitc-1` | 见 §3.2 |

### 3.1 `probe_meta_slot` 覆盖项（**真执行**，wasm32）

`M1` 往返保真 · `M2` `commit=0` ⇒ 无效 · `M3` CRC 保护 · `M4` `slot_seq` 受 CRC 保护 ·
`M5` legacy `fmt=2` 隔离 · `M6` magic/擦除态/全 0 · `M7` 六种选槽 · `M8` **掉电在两次写之间仍选旧槽** ·
`M9` 双槽损坏 ⇒ `-1` · `M10` 连续提交 `slot_seq` 严格递增 · `M11` `UINT32` 端点 ·
`M12` **五种恢复来源判定**（含 fail-closed 两例）

### 3.2 未被自动化覆盖的部分（**必须知道**）

| # | 未覆盖 | 为什么 | 后果 |
|---|---|---|---|
| `A-1-x1` | LittleFS 写入原子性、掉电时真实 Flash 行为 | 需真机 | 探针只证明**时序设计**正确，不证明硬件行为 |
| `A-1-x2` | `flash_find_max_seq()` / 段扫描 | 依赖 LittleFS | 段恢复路径只有**判定逻辑**被覆盖 |
| `A-1-x3` | fail-closed 的实际短路（`s_ready` 等运行时状态） | 依赖完整固件 | 只有 `decide_boot_source` 被覆盖 |
| `A-1-x4` | 主循环/队列/硬件任务的阻塞性影响 | 需真机 | 代码层面已确认无锁无 I/O 无延时 |
| `B-3-x1` | 真实 D1 的唯一索引行为 | 属 `test/real_d1/*` | 本次未扩展 L2（授权未要求） |
| `B-3-x2` | 真实 EMQX 发布语义 | 属 `BT1-T04` | 与 `B-3` 正交 |

> **不得**把上表任何一项表述为"已验证"。

---

## §4 接口文档：**为什么本次不需要改**

| 文档 | 是否需要改 | 原因 |
|---|---|---|
| `docs/interfaces/cloud_protocol.md` | **否** | `B-3` 不改设备协议、不改响应字段集（§1.4）；`A-1` 不改信封（`boot_seq`/`seq` 字段位置与语义均未变） |
| `migrations/README.md` | **否** | 无 schema 变更 |
| `HD-3-…Plan.md` §15 | **已改** | `rev.3` 增 §15.10 实测记录；§15.4 `Q-2a` 改掩码形态 |

> 若将来要让 `collision` 进入 HTTP 响应，**必须先改** `cloud_protocol.md` 的响应契约与 `T02j`。

---

## §5 需人工决策

| ID | 事项 | 选项 | 建议 |
|---|---|---|---|
| **`H-3-7`** | `collision` 是否要进 HTTP 响应（需改 `T02j` + 协议文档） | 是 / 否 / 暂缓 | **暂缓**（日志层已可观测；改冻结契约的收益低于风险） |
| **`H-3-8`** | `A-1` 的 fail-closed 是否接受"极端情况下日志停写" | 接受 / 改为"继续写但标记降级" | **接受**（重放是不可恢复的；停写是可见的） |
| **`H-3-9`** | 固件批次（烧录）与云端发布是否同批 | 同批 / 分开 | **分开**（`P2` = `HARDWARE DEFERRED`） |
| **`H-3-10`** | 是否需要补 `B-3` 的 L2（`real_d1`）用例 | 补 / 不补 | **建议补**（真实唯一索引下的 `differs` 才是有力证据） |
