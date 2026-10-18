# 批次 3 实施指令：`A1-03` 启动诊断 4 个 TLV 参数

> 交付对象：本地 AI Agent（单批可独立执行）  
> 依据：`HD-3-Risk-Closure-Plan.md`（`rev.2`）§4  
> 前置：固件 `wb @ 7e73177`；设备 `COM8 / 288485896ce4`（**本轮不动设备**）  
> **本指令不隐含真机操作授权**



---

## 1. 目标

让**一次启动**就能在云端日志里看到：`boot_seq` / `reserved_hw` / `recovery_src` / `failclosed` / `trace_corrupt`，  
从而摆脱"诊断只能插串口"的限制（`A1-03`）。

**方式**：复用既有启动事件 `LOG_SYS_BOOT_COMPLETE (0x0101)`，**追加 4 个 TLV 参数**。  
**不新增** event ID / topic / envelope；**不发**高频日志；**不改** ACK / 协议语义。

---

## 2. 前置事实（已核实，实施前必须**再次**复核）

| #   | 事实                                                         | 出处                                                     | 对本批的约束                                 |
| --- | ---------------------------------------------------------- | ------------------------------------------------------ | -------------------------------------- |
| F-1 | `LOG_P_MAX = 0x59`；PID `0x00…0x59` 已全部占用（90 个，无重复）         | `src/log/log_events.h:200`                             | 新 PID 只能取 `> 0x59`；**必须抬 `LOG_P_MAX`** |
| F-2 | 两处 `static_assert(LOG_P_MAX == 0x59)`                      | `log_events.h:619`、`test/log_contract/probe_ok.cpp:76` | 抬上限必须同步改这两处，否则编译失败                     |
| F-3 | `LOG_MAX_PARAMS = 8`；启动事件当前仅 1 个参数                         | `log_events.h:419`、`main.cpp:628`                      | 1+4 = 5 ≤ 8，**但仍须测试断言**                |
| F-4 | **仓库内无 TLV 解码器**：`probe_envelope.cpp` 只置 `param_count = 0` | `probe_envelope.cpp:164`                               | "旧解码器跳过未知 PID"**无代码可证** ⇒ 见 §3.2 V-3   |
| F-5 | 云端把 `payload_base64` 当**不透明数据**                            | `history-ingest.js:23`                                 | 加 PID 对云端零影响                           |
| F-6 | `docs/interfaces/log_envelope_contract.md` **未定义参数表**      | 全文检索无 `param`/TLV                                      | 必须补文档                                  |

> ⚠️ 若复核发现与上表不符（例如已有解码器、或 PID 有冲突），**先停止并报告**，不得按旧假设实施。

---

## 3. 要求

### 3.1 PID 分配

新增（建议值，实施时以实际空位为准）：

| PID    | 名称                    | 类型  | 取值域                                                        |
| ------ | --------------------- | --- | ---------------------------------------------------------- |
| `0x5A` | `LOG_P_RESERVED_HW`   | u32 | 序号高水位（`seq_reserved`）                                      |
| `0x5B` | `LOG_P_RECOVERY_SRC`  | u8  | `0=NVS,1=SLOT,2=LEGACY,3=SEGMENT,4=PROVISION,5=QUARANTINE` |
| `0x5C` | `LOG_P_FAILDCLOSED`   | u8  | `0/1`                                                      |
| `0x5D` | `LOG_P_TRACE_CORRUPT` | u32 | 损坏痕迹累计（`trace_corrupt`）                                    |

配套改动（**缺一不可**）：

1. `LOG_P_MAX` 抬到 `0x5D`；
2. `log_events.h:619` 的 `static_assert` 改 `0x5D`；
3. `test/log_contract/probe_ok.cpp:76` 的 `static_assert` 改 `0x5D`；
4. 全仓 grep 确认无重复值、无别处硬编码 `0x59`。

### 3.2 兼容性核验（**任一不满足即报 `BLOCKED`，不得声称"向后兼容"**）

| #       | 核验             | 通过判据                                                                                                                                                                                                                          |
| ------- | -------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **V-1** | PID 唯一性        | 脚本比对 `LOG_P_*` 全部取值，输出"无重复"；新增 4 个不与既有冲突                                                                                                                                                                                      |
| **V-2** | 参数上限           | 单测/断言：启动事件 `param_count=5 <= LOG_MAX_PARAMS(8)`                                                                                                                                                                               |
| **V-3** | 未知 PID 行为      | **二选一**：<br />(a) 在 `test/log_contract/` 增一个**最小解码器测试**，构造含未登记 PID 的 payload，断言"跳过且不报错、已知 PID 仍可解"；<br />(b) 出具证据链：仓库内**确无**解码器（贴 `probe_envelope.cpp:164`）+ 云端不透明（贴 `history-ingest.js:23`），并在契约文档写明"参数表为加性扩展，消费方必须跳过未知 PID" |
| **V-4** | 契约文档           | 新建 `docs/interfaces/log_params_contract.md`（或在既有契约中新增章节）：登记 5 个 PID（含 `0x21`）、类型、单位、取值域、**未知 PID 处理规则**                                                                                                                       |
| **V-5** | fail-closed 边界 | 代码注释 + 文档写明：`failclosed=1` 时 `log_emit()` 被短路 ⇒ **该诊断日志发不出去**；此类"看不到"≠"没问题"；且**不得**因发日志失败改变启动流程                                                                                                                               |

### 3.3 发射点（单点、一次）

- 位置：`main.cpp:628` 现有 `log_emit(LOG_SYS_BOOT_COMPLETE, ...)` 处，**参数从 1 个扩到 5 个**；
- 数据来源：`log_flash_get_info()`（含 `boot_seq/seq_reserved/evidence_source/seq_failclosed/trace_corrupt`）；  
  `boot_seq` 用既有 `LOG_P_BOOT_SEQ (0x21)`；
- **必须**保证：
  1. 每启动**最多 1 条**（不得放进循环/定时器）；
  2. `log_emit()` 内部会做突发合并（`LOG_P_COUNT`）；本事件**不得**加入合并白名单，避免被折叠掉；
  3. 参数栈占用 ≤ `5 * sizeof(LogParamIn)`（约 60 B），**不得**在栈上分配大缓冲；
  4. 调用前不得引入阻塞 I/O（`log_flash_get_info()` 会读 `corrupt.trace`，须确认其为只读小文件、失败即返回，不重试）。

---

## 4. 约束

- 不新增 event ID；不改 `LOG_SYS_BOOT_COMPLETE` 的 code 值（`0x0101`）；
- 不新增 MQTT topic；不改 envelope；不改 ACK；不改 HTTP 契约；
- **不**把积压量/上传进度做成遥测（需协议授权，且会推翻 `WP-6` 容量结论）；
- **不**发周期统计日志；
- 非阻塞：不得 `delay()`、不得等待 NV/Flash 重试；
- 内存：不得在栈上分配 >1 KB；
- 既有日志记录格式（除本事件参数表）保持不变。

---

## 5. 验收标准

| #  | 判据                                                                                                                                                                                            |
| -- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| C1 | `pio run -e esp32-s3-devkitc-1` → **SUCCESS**，无新增 warning（`-Werror` 环境下为 0）                                                                                                                   |
| C2 | `python test/log_contract/run_contract_test.py` → **8/8 步 ALL PASS**                                                                                                                          |
| C3 | 契约测试**新增 ≥10 项**：①4 个 PID 均存在 ②`reserved_hw` u32 ③`recovery_src` 取值 0–5 ④`failclosed` 仅 0/1 ⑤`trace_corrupt` u32 ⑥`boot_seq` 仍在 ⑦PID 无重复 ⑧`param_count=5 ≤ 8` ⑨未知 PID 跳过（按 V-3 所选路径） ⑩启动仅 1 条 |
| C4 | 既有断言不减少：`probe_meta_slot` 32 + `probe_meta_evidence` 83                                                                                                                                       |
| C5 | `git diff` 不包含 `migrations/`、`src/log/log_manager.cpp` 的**逻辑**改动（只允许注释级）                                                                                                                      |
| C6 | 文档：`log_params_contract` 已登记 5 个 PID + 未知 PID 规则 + fail-closed 说明                                                                                                                             |

---

## 6. 必交报告

1. `LOG_P_*` 全量取值表（证明无重复）+ 4 个新 PID 的最终选择；
2. 两处 `static_assert` 与 `LOG_P_MAX` 的改动 diff；
3. V-1…V-5 的核验结果（**逐条**，附文件:行号证据）；
4. 契约测试新增项清单与 PASS/FAIL；
5. `pio run` 的 RAM/Flash 变化（与 `40.5% / 66.8%` 对比）；
6. **真机验证**：`NOT RUN`（本批不操作设备），但需给出验证步骤与预期（见 §7）；
7. 明确写出：`真机 = NOT RUN`、`云端可见性 = 需部署/真机后才成立`。

---

## 7. 计划中的真机验证（**需后续单独授权，本批不执行**）

| 步骤                                  | 预期                                             |
| ----------------------------------- | ---------------------------------------------- |
| ① 记录当前 `boot_seq=N`                 | 串口 `logt meta`                                 |
| ② 烧录新固件（app 分区，不动 LittleFS）         | `Hash of data verified`                        |
| ③ 复位一次                              | 串口出现 `[Log] recovery=…`，`boot_seq=N+1`         |
| ④ 云端查 `log_history` 最新 `code=257` 行 | `payload` 解出 5 个 PID；`0x21=N+1`、`0x5C=0`       |
| ⑤ 查该行是否**只有 1 条**/启动                | `COUNT(*) where code=257 and boot_seq=N+1` = 1 |

---

## 8. 失败 / 停止条件

1. `LOG_P_MAX` 抬升导致任何 `static_assert`/编译失败且无法在**不改变既有 PID 值**的前提下修复 ⇒ **停**；
2. V-3 两条路径都无法证明兼容性 ⇒ **停**并报 `BLOCKED`（不得"先上再说"）；
3. 需要在栈上分配大缓冲 / 需要 `delay()` ⇒ **停**（违反非阻塞与内存铁律）；
4. 必须修改 event ID / envelope 才能实现 ⇒ **停**（超授权）；
5. 契约测试既有 32/83 项数下降 ⇒ **停**。

---

## 9. 允许的 Git 操作

- 只读：`status` / `diff` / `log` / `show`；
- 需要提交时逐文件 `git add` 并核对 `--cached --name-only`；
- **禁止**：`push`、`reset --hard`、`clean`、`stash`、切分支、`rebase`/`merge`/`cherry-pick`；未授权不得 `commit`。

---

## 10. 明确禁止事项

1. 烧录 / 复位 / `uploadfs` / 擦除设备（本批 **NOT RUN**）；
2. 修改 `log_manager.cpp` 的序号/隔离**逻辑**（只允许加注释）；
3. 新增 event ID、topic、遥测字段；
4. 把诊断做成周期性日志；
5. 改动 ACK / HTTP 四字段 / MQTT envelope；
6. 声称"云端已可见"（未部署、未真机 ⇒ 不成立）。
