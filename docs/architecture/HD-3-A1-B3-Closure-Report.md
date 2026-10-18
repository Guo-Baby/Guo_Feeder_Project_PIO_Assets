# HD-3 · A-1 / B-3 缺陷闭环与重新验收报告

> **执行时间**：`2026-10-18 22:57 – 23:59`（本地）
> **基线**：`IMPLEMENTED - LOCAL ONLY` + 独立审查 `BLOCKED`
> **本轮性质**：统一授权下的分批修复 → 本地测试 → 重新验收
> **未执行**：生产 D1 查询/写入 · Worker/Pages 部署 · EMQX/Cron 变更 · 设备命令/烧录/OTA · Git 任何操作
> **关联文档**：`HD-3-A1-B3-Independent-Review.md`（缺陷来源）· `HD-3-H3-7-ACK-Semantics-Decision.md`（待裁决项）

---

## 1. 最终状态

| 项 | 状态 | 判据 |
|---|---|---|
| **A-1** | **`CONDITIONAL`** | 代码缺陷 `A1-01/02/04/05/06/07/08/09` 全部闭环，`A1-03` 部分闭环；**真机（格式化 / 断电 / NVS 丢失）未验证**；"整片擦除"边界需人工定义 |
| **B-3** | **`CONDITIONAL`** | 代码缺陷 `B3-01/02/03/05` 全部闭环，`B3-06` 记为文档边界；`B3-04`（`H-3-7` ACK 语义）为**产品裁决项**，未改码 |
| **整体 HD-3** | **`CONDITIONAL`** | 原 `BLOCKED` 的两条阻断（`A1-01` / `A1-02`）已从"代码层"移除，但缺真机证据与一项裁决 |

**状态定义**（本报告统一口径）：

- `PASS` —— 代码 + 本地可执行验证全闭环，无未决代码缺陷、无待验证项
- `CONDITIONAL` —— 代码闭环，但存在"必须真机 / 必须人工裁决 / 必须扩大授权"的未验证项
- `BLOCKED` —— 存在未修复的阻断缺陷

---

## 2. 缺陷闭环表

### 2.1 A-1（固件）

| ID | 原严重性 | 状态 | 修改文件 | 修复依据 | 剩余风险 |
|---|---|---|---|---|---|
| **A1-01** | `BLOCKER` | **`CLOSED`（模拟+代码审查）** | `src/log/log_meta_evidence.h`(新) · `log_manager.cpp`(seq_init/evidence) · `log_manager.h` · `main.cpp` · `device_identity.{h,cpp}` | ① 新增**独立于 LittleFS** 的 NVS 证据（`guolog/ev`，32 B，CRC 保护），格式化后仍存活；② 恢复判据新增"无证据 + 身份早已存在 ⇒ 隔离"；③ 仅当**本次 boot 才持久建立身份**（`device_identity_first_boot()`）才允许 `boot_seq=1` | 整片擦除（NVS+LittleFS 同时丢）仍不可在设备侧分辨 ⇒ `HUMAN_REQUIRED`（服务侧 boot_seq 回退检测）；**真机未验证** |
| **A1-02** | `MAJOR` | **`CLOSED`（模拟）** | `log_manager.cpp`(`quarantine_enter` / `seq_init`) · `log_meta_evidence.h` | 隔离**双份固化**（NVS 标志 + `/log/quarantine.bin`），且判据**隔离优先于一切**（`decide_recovery` 第①条）；`E10j` 证明"双槽+段全有效时隔离仍生效" | 两份都写失败时本次 boot 仍停写但**跨重启不保**（已显式串口告警）；真机未验证 |
| **A1-03** | `MAJOR` | **`PARTIAL`** | `log_manager.h`(LogFlashInfo +5 字段) · `log_manager.cpp` · `main.cpp`(`[LogT] meta` 第二行、开机摘要加 `src=`/`failclosed=`) | `seq_failclosed` / `evidence_source` / `nvs_*` 现可经串口与 `LogFlashInfo` 读取，与"预留失败"可区分 | **仍无 MQTT/远程通道**：设备静默停写时云端只看到"没有日志"。补法需新增上报字段 ⇒ 协议变更 ⇒ `HUMAN_REQUIRED` |
| **A1-04** | `MINOR` | **`CLOSED`** | `log_meta_evidence.h`(`seg_last_usable` / `align_reserve`) · `log_manager.cpp`(`flash_seg_high_water`) | 跳过 `0xFFFFFFFF` 哨兵；对齐饱和到 `LOG_SEQ_EXHAUSTED`，**绝不回绕成 0** | 无（`E7f/E7g/E9*` 覆盖） |
| **A1-05** | `MINOR` | **`CLOSED`** | `log_manager.cpp`(`meta_slot_write_raw` 回读逐字节校验) | 写后回读 32 B 并与打包缓冲逐字节比对；不一致 ⇒ 返回 false | 回读仍依赖 LittleFS 读路径；真机掉电行为未验证 |
| **A1-06** | `MINOR` | **`CLOSED`** | `log_manager.cpp`(`log_meta_wipe` 改为 legacy+双槽 / 新增 `_legacy`/`_slots` / `log_meta_test_fail_stage`) · `main.cpp`(`mstage`/`lqclear`/`levwipe`) | `mwipe` 恢复"元数据全失"原语义；新增 `mstage 1/2/3` 可注入"写槽①后失败（半写槽留盘）"与"回读失败" | `mcorrupt` 仍只作用于 legacy（**已在头文件注明**）；真机未用 |
| **A1-07** | `NIT` | **`CLOSED`** | `log_meta_evidence.h`(`next_boot_seq`) · `log_manager.cpp`(`seq_reserve_next` 溢出闸门) | 饱和到 `0xFFFFFFFF` 并置 `ok=false` ⇒ 转隔离；`reserved >= 0xFFFFFF00` 直接隔离 | 无（`E8*` 覆盖） |
| **A1-08** | `NIT` | **`CLOSED`** | `log_manager.cpp`(`meta_write` 改 临时文件+rename) | `rename` 为目录项级原子替换 ⇒ 回滚镜像不再被 `open("w")` 截断；镜像失败置 `seq_reliable=false` 留痕 | 镜像失败时旧固件仍可能读到上一代值（可接受） |
| **A1-09** | `NIT` | **`CLOSED`** | 本报告 §4 | 实测 `probe_meta_slot` **32 项**（原文档写 41）、`probe_meta_evidence` **63 项** ⇒ 文档按实测记 | 无 |

### 2.2 B-3（云端）

| ID | 原严重性 | 状态 | 修改文件 | 修复依据 | 剩余风险 |
|---|---|---|---|---|---|
| **B3-01** | `MINOR` | **`CLOSED`** | `src/log-ack.js` | 新增 `exists_unavailable` 计数（`unpersisted` 的子集）+ 每批一次 `console.error`；`fetchExistingRow().ok` 不再是死字段 | 该计数仍只在日志里；`unpersisted` 口径未变（兼容） |
| **B3-02** | `MINOR` | **`CLOSED`** | `src/log-ack.js` | `content_unverified > 0` ⇒ 每批 `console.error`（区别于逐行 `warn`） | 无告警通道（D1 表/指标），仅日志 |
| **B3-03** | `MINOR` | **`CLOSED`** | `test/current/log_ack_collision.test.mjs` | ① `C4` 的 `<=` 改 `===`；② C2 空断言换成对 `[log-ack] summary` 行的真实断言；③ C 组与 C3 标题按实现改写（响应体**不**新增字段） | 无 |
| **B3-04** | `QUESTION` | **`HUMAN_REQUIRED`** | 仅新增决策材料 `HD-3-H3-7-ACK-Semantics-Decision.md` | 未改任何代码 / 协议 / 重试语义 | 冲突行仍 ACK ⇒ 内容静默丢失；决策前不做改动 |
| **B3-05** | `NIT` | **`CLOSED`** | `src/log-ack.js`(`compareLogContent` 数值分支收紧) | 仅接受"有限数字"或"`^-?\d+$` 字符串"；`""`/`" 12 "`/`true` 与数字比较一律 `differs` | 生产路径不受影响（`asInt` 已归一化），属导出 API 加固 |
| **B3-06** | `NIT` | **`DOCUMENTED`（不改）** | `src/log-ack.js` 注释 | 规范化 base64 会引入"解析 payload"，与冻结约束冲突 ⇒ 只记录"`collision` 是候选、可能含重编码" | 保持现状；如需消除噪声须先修订"不解析 payload"约束 |

---

## 3. 实现摘要

### 3.1 A-1：恢复判据从「日志区四层」升级为「三份独立证据 + 隔离」

**新增模块** `src/log/log_meta_evidence.h`（freestanding，纯逻辑，可 wasm 真跑）：

| 符号 | 作用 |
|---|---|
| `pack()` / `parse()` | NVS 证据记录（32 B：magic `GLNE` / fmt 1 / boot_hw / reserved_hw / flags / gen / corrupt / CRC32-28 B） |
| `decide_recovery()` | **唯一**恢复判据：`隔离 > NVS > 双槽 > legacy > 段内 > 真·首启 > 隔离` |
| `align_reserve()` / `next_boot_seq()` | 饱和算术（`A1-04`/`A1-07`） |
| `seg_last_usable()` | 过滤 `0xFFFFFFFF`"末条未知"哨兵 |

**判据变化（核心）**：

| 情形 | 旧行为 | 新行为 |
|---|---|---|
| 日志区被 `formatOnFail` 格式化（NVS 健在） | `BOOT_FRESH` ⇒ `boot_seq=1` ⇒ **撞键** | `REC_FROM_NVS` ⇒ 续用 NVS 高水位 |
| 日志区 + NVS 全无，但身份早已存在 | `BOOT_FRESH` ⇒ `boot_seq=1` ⇒ **撞键** | `REC_QUARANTINE` ⇒ **拒绝分配** |
| 同上，但身份本次才持久建立 | `BOOT_FRESH` | `REC_PROVISION` ⇒ `boot_seq=1`（唯一合法首启） |
| 上一轮已隔离 | 仅 RAM，重启即"洗白" | NVS + 文件双份固化 ⇒ **跨重启保持** |

**证据持久化边界**（必须原样引用，不得扩大）：

```
NVS 分区   ── 存活于 LittleFS 格式化 / 日志区损坏 / 双槽损坏
日志区     ── 存活于 NVS 损坏
整片擦除   ── 两者同时丢失 ⇒ 无法与全新设备区分 ⇒ 等义「受控恢复出厂设置」
            ⇒ 需服务侧 boot_seq 回退检测兜底（HUMAN_REQUIRED）
```

**受控恢复出厂**：`log_quarantine_clear()`（串口 `lqclear`）**保留** `boot_hw`/`reserved_hw`，只清隔离位 ⇒ 下次开机从**已知最大 `boot_seq`** 续推，**不回 1**；且运行期闸门不立即清除（须重启重判，避免复用未固化区间）。

**兼容性影响**：

| 项 | 变化 |
|---|---|
| 日志记录格式（128 B）· 段头（16 B）· meta 槽（32 B）· CBOR 批次 · MQTT envelope · 云端 `ux_lh_dedup` | **零改动** |
| `LogFlashInfo` | **仅追加** 6 字段（`seq_failclosed`/`nvs_evidence_ok`/`nvs_quarantine`/`quarantine_file`/`evidence_source`/`nvs_*`/`evidence_gen`），既有字段与顺序不变 |
| 旧固件回滚 | `meta.bin`（`fmt=2`）仍每次提交镜像，且改为原子 rename ⇒ 回滚可读性**提升** |
| 老设备升级本固件 | 无 `guolog/ev` ⇒ 走 `legacy`/段 ⇒ **不触发隔离**；首次提交即写入 NVS 证据（自愈） |
| 新增 NVS 命名空间 | `guolog`（key `ev`，32 B）；写入频率 = 每次开机 + 每次 seq 预留（≤ 每 256 条日志） |
| RAM / Flash | `40.5% → 40.5%`（+32 B）· `66.4% → 66.7%`（+5 148 B） |
| 开机顺序 | `device_identity_init()` **提前到 `log_init()` 之前**（`main.cpp`），仍早于 `wifi_init`/`cloud_init` |

### 3.2 B-3：只做加性计数与观测硬化，契约零改动

| 项 | 变化 |
|---|---|
| HTTP 响应体 | **不变**（`accepted`/`duplicate`/`rejected`/`filtered`） |
| `duplicate` 口径 | **不变**（未新增行 且 键存在） |
| 查询次数 | **不变**（每次未新增行仍 1 次 `SELECT 1 AS hit, …`） |
| schema / migration | **未动** |
| `summary` 行 | 追加 `exists_unavailable=`（既有字段顺序不变） |
| `compareLogContent` | 数值容忍分支收紧（`B3-05`），返回三态语义不变 |

---

## 4. 测试证据

### 4.1 执行结果（全量）

| # | 测试 | 层级 | 结果 | 退出码 |
|---|---|---|---|---|
| 1 | `test/log_contract/run_contract_test.py` | **纯逻辑 · wasm32 真执行** | **8/8 步 ALL PASS**；`probe_meta_slot` **32** 项、`probe_meta_evidence` **63** 项全部 PASS | `0` |
| 2 | `pio run -e esp32-s3-devkitc-1` | **编译** | `SUCCESS` 33.09 s · RAM 40.5%（132 744 B）· Flash 66.7%（1 398 653 B） | `0` |
| 3 | `node test/current/log_ack_collision.test.mjs` | **L1 逻辑替身** | **61 PASS / 0 FAIL**（原 48 ⇒ +13） | `0` |
| 4 | `node test/run_all.mjs` | **L1 全量回归（16 套件）** | **16/16 `exit=0`** | `0` |
| 5 | `bash test/real_d1/b3_collision_real_d1.sh` | **真实 D1 引擎（本地 workerd）** | **53 PASS / 0 FAIL**（原 52 ⇒ +1） | `0` |

### 4.2 新增/修改的测试资产

| 文件 | 类型 | 说明 |
|---|---|---|
| `test/log_contract/probe_meta_evidence.cpp` | **新增** | 63 项：证据记录往返 / CRC/magic/fmt / 恢复优先级（12 例）/ 高水位 / 溢出 / 饱和 / 哨兵 / **跨重启状态机 B1–B6（13 例）** |
| `test/log_contract/probe_meta_slot.cpp` | 修改 | 移除 `M12`（判据已迁出），改为指向新探针的迁移说明（**未删减覆盖**：7 条语义全部在 `E5c/d/e/f/g/h/i` 承接） |
| `test/log_contract/run_contract_test.py` | 修改 | 6 步 → **8 步**（分母全量改写） |
| `test/current/log_ack_collision.test.mjs` | 修改 | 48 → **61**：新增 `A14–A18`（数值边界）、`C5`（HTTP 端到端碰撞 + 日志取证 + 不泄露内容）、`B3/B4` 的 `exists_unavailable`、`C4` 改 `===`、C2 空断言替换、C 组标题改正 |
| `test/real_d1/b3_collision_real_d1.sh` | 修改 | 52 → **53**：新增 `exists_unavailable=0` 断言 |

### 4.3 强制验收场景覆盖矩阵

| 要求场景 | 覆盖方式 | 结果 |
|---|---|---|
| 正常启动 / 正常元数据恢复 | `E10d/e`（B2）· `M7/M10` | `PASS`（模拟） |
| 单槽损坏 / 双槽损坏 / 元数据缺失 | `M7d/e`、`M9`、`M8` · `E5c` | `PASS`（纯逻辑） |
| **日志段损坏 → 段被删除 → 连续两次重启** | `E10j`（隔离跨重启）+ `decide_recovery` 隔离优先 | `PASS`（**模拟**）· **真机 NOT RUN** |
| **LittleFS 挂载失败 / 格式化后的恢复** | `E10f/g`（B3：槽/段全无、NVS 健在 ⇒ 仍 `boot_seq=3`） | `PASS`（**模拟**）· **真机 NOT RUN** |
| **NVS 状态丢失 / 证据不足** | `E10h/i`（⇒ 隔离） | `PASS`（模拟）· **真机 NOT RUN** |
| 受控恢复出厂 | `E10l/m`（清隔离后从记忆的 `boot_hw` 续推） | `PASS`（模拟） |
| 元数据提交中断 | `M8`（两阶段）+ `mstage 1/2` 注入钩子 | 钩子已就位 · **上板 NOT RUN** |
| 写后回读异常 | `mstage 3` 注入 + `meta_slot_write_raw` 回读分支 | 钩子已就位 · **上板 NOT RUN** |
| 高水位边界 / 序号溢出 | `E6*` / `E7*` / `E8*` / `E9*` | `PASS`（纯逻辑） |
| B-3 三类判定 / 计数守恒 / 查询失败 / 重复 / 碰撞 | `A1–A18`、`B1–B7`、`C1–C5`、real_d1 `R1–R7` | `PASS`（L1 + **真实 D1**） |
| HTTP 四字段契约 / ACK 兼容性 / 完整回归 | `C1/C2/C3/C5` + real_d1 逐用例 + `run_all` 16 套件 | `PASS` |

### 4.4 未运行项与原因（不得以推断替代）

| 项 | 状态 | 原因 |
|---|---|---|
| 真机断电（两次写之间、任意窗口） | **`NOT RUN`** | 无设备接入，且属破坏性测试，需单独授权 |
| 真机 LittleFS 挂载失败 / 格式化后重启 | **`NOT RUN`** | 同上 |
| 真机 NVS 分区擦除 | **`NOT RUN`** | 同上 |
| 真机双槽半写（`mstage 2`）实际落盘形态 | **`NOT RUN`** | 同上 |
| NVS 键级原子性 / 掉电行为 | **`NOT RUN`** | 无法在主机与本地 D1 环境验证 |
| 真实 D1 多行批（`PERSIST_CHUNK > 1`）的 `meta.changes` 组合 | **`NOT RUN`** | 本轮未扩展该夹具（`R*` 均单条）；属可补项 |
| 生产 D1 只读核对（schema / 索引 / 行数） | **`NOT RUN`** | 本轮明确禁止生产查询 |
| `H-3-7` 决策 | **`HUMAN_REQUIRED`** | 产品语义裁决 |

---

## 5. 安全与回归评估

| 风险 | 结论 | 依据 |
|---|---|---|
| **跨重启恢复** | **已解决（模拟层）** | `E10a–m` 完整跑通 B1→B6 六轮；隔离由 RAM 单点变为 NVS+文件双份 |
| **幂等键重用（`(device_id, boot_seq, seq)`）** | **已收敛**：`R-1` 由双槽+高水位消除；`R-2` 的"日志区失效"路径由 NVS 证据消除；**仅剩"整片擦除"** | `E10f/g` 证明格式化后不再回 1；`E10h/i` 证明无证据时不猜 |
| **碰撞 ACK** | **未解决（待裁决）** | `B3-04` → `H-3-7`；本轮只增加可观测性，未改语义 |
| **旧版本回滚** | **已改善** | `meta.bin` 镜像改原子 rename；`fmt=2` 与 `fmt=3` 版本隔离不变（`M5`） |
| **缓冲区占满** | **未变** | 隔离期间不产生记录 ⇒ RAM/Flash 环无新增压力；`ring_drop`/段淘汰逻辑未动 |
| **新增失效模式** | 2 项，已受控 | ① NVS 写失败 ⇒ `seq_reliable=false` + 串口告警（不静默）；② 隔离两份标记都写失败 ⇒ 显式 `BOTH markers failed` 告警 |
| **首启误判** | 已加约束 | 只有"身份本次才**持久**建立"（`putString` 成功）才允许 `boot_seq=1`；写失败 ⇒ 仍走隔离（避免每轮重放） |

**关键不变量（本轮未改变的）**：日志 128 B 记录 / 16 B 段头 / 32 B meta / `LOG_SEQ_RESERVE=256` / CBOR 批次 / JSON envelope / 云端唯一键 / HTTP 四字段 / `duplicate` 口径 / 查询次数。

---

## 6. 未决事项（需人工）

| # | 事项 | 需要什么 | 推荐 |
|---|---|---|---|
| 1 | `H-3-7-a` 冲突行是否保持 ACK | 产品裁决 | **保持 ACK**（方案 A） |
| 2 | `H-3-7-b` 是否批准新增 `log_conflict` 表 | **新 migration 授权**（不动 `0003`） | 批准，否则冲突随日志过期不可追溯 |
| 3 | `H-3-7-c` 是否接受"ACK = 键存在"语义并写清 | 文档裁决 | 接受并补一句到契约文档 |
| 4 | `A1-03` 远程可观测（MQTT 上报 `failclosed`） | **协议变更授权** | 需要；否则部署后仍可能"静默停写" |
| 5 | "整片擦除"边界的最终定义 | 产品/运维裁决 | 定义为「受控恢复出厂」+ 服务侧 `boot_seq` 回退检测 |
| 6 | 真机验证包（断电 / 格式化 / NVS 擦除 / 半写） | **设备 + 破坏性测试授权** | 按 §4.3 逐项执行 |
| 7 | `first_boot` 策略是否切换为"必须产线 commissioning" | 产品裁决 | 当前默认"身份本次持久建立即允许"，成本最低 |

---

## 7. 工作区保护确认

### 7.1 未执行的操作（逐项确认）

| 禁止项 | 状态 |
|---|---|
| `git reset` / `clean` / `stash` / 分支切换 / rebase / merge / cherry-pick / commit / push | **未执行**（仅 `git status` / `git diff` / `git rev-parse` 只读） |
| 生产 D1 查询 / 写入 | **未执行**（全部 `--local --persist-to`；夹具自检"远端标志命中 = 0"） |
| Worker / Pages 部署 | **未执行** |
| EMQX / Cron / 生产配置 | **未改动** |
| migration 修改 | **未修改**（未新增、未改 `0003`） |
| 设备命令 / 重启 / 烧录 / OTA | **未执行**（仅本地 `pio run` 编译） |
| 关闭门禁（`BT-1 OPEN` / `G-4 BLOCKED` / `WP-5` / `STOP-R1`） | **未触碰** |
| 重开或修改 RET-1 | **未触碰** |
| 覆盖其他任务的工作区差异 | **未触碰**（`HANDOFF.md`、`docs/README.md`、`P3-*`、`未修复的问题.md` 保持原状） |

### 7.2 本轮工作区变化

**固件仓（`wb` @ `3acf91c`，未提交）**

| 类型 | 文件 |
|---|---|
| 新增 | `src/log/log_meta_evidence.h` · `test/log_contract/probe_meta_evidence.cpp` · `docs/architecture/HD-3-A1-B3-Independent-Review.md` · `docs/architecture/HD-3-H3-7-ACK-Semantics-Decision.md` · `docs/architecture/HD-3-A1-B3-Closure-Report.md`（本文件） |
| 修改（本轮） | `src/log/log_manager.cpp` · `src/log/log_manager.h` · `src/log/log_meta_slot.h` · `src/main.cpp` · `src/services/device_identity.{h,cpp}` · `test/log_contract/run_contract_test.py` · `test/log_contract/probe_meta_slot.cpp` |
| 修改（既有，未触碰） | `HANDOFF.md` · `docs/README.md` · `docs/architecture/P3-History-D1-Plan.md` · `docs/architecture/P3-WP3-History-Storage-Policy-Design.md` · `docs/issues/未修复的问题.md` |

**云端仓（`main` @ `a3473eb`，未提交）**

| 类型 | 文件 |
|---|---|
| 新增 | `test/real_d1/b3_collision_real_d1.sh` |
| 修改（本轮） | `src/log-ack.js` · `test/current/log_ack_collision.test.mjs` |
| 修改（既有，未触碰） | `src/history-ingest.js` · `test/run_all.mjs` |

> ⚠️ 本轮**没有** `git add` / `git commit`。上述差异全部保留在工作区，等待后续人工分批提交。
