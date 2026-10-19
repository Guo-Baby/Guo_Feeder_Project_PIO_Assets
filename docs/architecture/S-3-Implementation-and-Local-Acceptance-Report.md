# S-3 实施与本地验收报告（`loop()` 耗时观测 + 只读 `system.loop`）

> 工作包：**规格修订 → 编码实施 → 本地单元测试 / 构建 / 静态审查**
> 执行窗口：`2026-10-20 02:05 – 02:35`（本地）· `2026-10-19T18:05Z – 18:30Z`（UTC）
> 前序文档：`S-3-Loop-Timing-Observability-Investigation-and-Spec.md`（R1，`d595997`）
> 规格修订：`S-3-Spec-Revision-R2.md`
> 证据目录：`docs/evidence/S-3-2026-10-20/`
>
> ★ **本包未烧录、未复位、未发送任何设备命令、未改配置/数据库/部署/EMQX。**
> ★ **`BT-1 = CLOSED` · `G-4 = BLOCKED` · `WP-5 = 未解锁` · `STOP-R1 = 无解除` 全部未改动。**
> ★ 本报告**不**声称 L2 采集有效或 L3 P99 达标 —— 那两项需要真实设备与正式授权。

---

## 0. 结论速览

| 层级（R1 §6 定义） | 本包后状态 | 依据 |
|---|---|---|
| **L1 观测能力已实现** | **代码级已达成**（`IMPLEMENTED_LOCAL_ACCEPTED`） | 82/82 宿主机断言通过、`system.loop` 路由已接、`static_assert` 结构/几何约束成立、构建可复现 |
| **L2 数据采集有效** | **未验证**（`NOT VERIFIED`） | 需要真实 ESP32-S3 运行采样器；宿主机时间源是伪造的，**不能**证明目标板采样有效 |
| **L3 P99 达标** | **`NOT VERIFIED`** | 未烧录 ⇒ 无 P99 数据；正式证据时长（2 h + 30 min）另行授权 |

**一句话**：能测了、结构化地测、且测不准时**拒绝给数**；但"测得准不准"和"达不达标"都还没到能说话的时候。

---

## 1. 规格修订摘要与 Q1–Q10 决策（详见 `S-3-Spec-Revision-R2.md`）

| ID | 决策 | 性质 |
|---|---|---|
| `S3-Q1` | 绝对 `P99 < 20 ms` = **正式主判据**；相对 `劣化<20%` 原文保留但不作为本轮可执行判据 | 门禁语义（留痕，未改原文） |
| `S3-Q2` | 相对口径的分母**永久不可恢复** ⇒ **不替代、不自选**；将来启用须人工书面指定替代基线 | 门禁语义 |
| `S3-Q3` | `esp_timer_get_time()`（int64 µs），**代码级禁用 `micros()`** | 技术 |
| `S3-Q4` | `cloud_poll()` **确属 loop 内独立阻塞项**（已只读核实），本包**不实现**其埋点 → 登记 `S3-U1` | 范围控制 |
| `S3-Q5` | 新增独立只读 `system.loop`；`system.stats` **契约一字不改** | 技术 |
| `S3-Q6` | **不提供** reset 写命令；窗口统计用两次快照差分 | 技术 |
| `S3-Q7` | 单轮耗时 = 相邻两次进入 `loop()` 的间隔（含调度/抢占影响） | 语义 |
| `S3-Q8` | 证据时长 **2 h 稳态 + 30 min 突发，维持不变** | 门禁语义 |
| `S3-Q9` | 采样开销**计入**被测循环，报告须声明"含开销"；保留 `ENABLED=0` 对照构建（**已实际构建**） | 技术 |
| `S3-Q10` | 分模块埋点 **不做** → 登记 `S3-U2` | 范围控制 |

### 1.1 相对 R1 的三处技术偏离（必须显式记录）

| 编号 | R1 | R2 | 原因 |
|---|---|---|---|
| **`S3-D1`** | `256 桶 × 0.2 ms`，桶类型 **`uint16`**，量程 51.2 ms | **`128 桶 × 0.2 ms`，桶类型 `uint32`**，量程 25.6 ms | ① `256×uint32` 结构达 1088 B，**超 1 KB 栈铁律**（快照要放命令栈帧）；② R1 的 `uint16` 在 1 kHz × 2 h（≈7.2e6 样本）下**必然提前饱和** ⇒ P99 静默失真。`uint32` 上限 4.29e9 ≈ **49.7 天 @1 kHz** |
| **`S3-D2`** | 采样率自洽用 `n/Δuptime ≈ 1/P50` | 用 **`1/mean`**（`sum_us/n` 精确均值） | 任务书要求"不得以 P50 替代平均周期" |
| **`S3-D3`** | `elapsed` 类型未定 | **`uint64_t`** | `uint32` µs 在 71.6 min 回绕 ⇒ 2 h 窗口 elapsed 归零、自洽校验失效 |

> ★ `S3-D1` 保住了最关键的性质：**20 ms = 100 × 0.2 ms 仍恰在桶边界** ⇒ `p99_bucket ≤ 99 ⟺ P99 < 20 ms`，判定**精确、无桶内插值歧义**（`static_assert` 强制）。

---

## 2. 实际修改的文件与目的

| 文件 | 行数 | 修改目的 |
|---|---|---|
| `src/app/loop_stats.h` | **+170**（新） | 常量（桶几何/阈值/最小样本）、`LoopStatsSnapshot`（576 B POD）、`LoopStatsQuantiles`、`invalid_bits` 位定义、4 个 API、**1 KB 体积 `static_assert`** |
| `src/app/loop_stats.cpp` | **+374**（新） | 采样器（无锁/零输出/零分配）、快照读取（单临界区整体拷贝 + 自测耗时）、分位/差分纯函数、宿主机注入钩子 |
| `src/main.cpp` | **+16** | ① `#include "app/loop_stats.h"`；② `loop()` **第一行**插入 `loop_stats_tick()`；③ `setup()` 末尾 `loop_stats_init()` |
| `src/services/system_command.h` | **+20** | `syscmd_loop()` 声明 + 完整输出字段契约注释 |
| `src/services/system_command.cpp` | **+161** | `syscmd_loop()` 实现 + `loop_invalid_reason_name()`；**不可信即抑制分位输出** |
| `src/services/command_manager.cpp` | **+24** | `system_router` 新增 `object == "loop"` 分支 + 前向声明 + `command_system_loop()` |
| `platformio.ini` | **+5** | `-DGF_LOOP_STATS_ENABLED=1`（可关；附改动纪律注释） |
| `test/loop_stats_host/loop_stats_probe.cpp` | **+504**（新） | 宿主机探针：9 组 / **82 项**断言 |
| `test/loop_stats_host/run.sh` · `run_probe.js` | **+77**（新） | wasm32 freestanding 编译 + node 运行器（与 `test/device_model_host` 同范式） |
| **合计** | **10 文件 / +1351 行**（**0 行删除**） | 无重构、无无关文件改动 |

---

## 3. 核心实现与关键边界条件

### 3.1 采样器（热路径）

```cpp
void loop_stats_tick() {                  // loop() 第一行
    now = esp_timer_get_time();           // int64 µs，不回绕
    if (!available) { prev = now; start = now; available = 1; return; }  // 丢首样本
    dt = now - prev; prev = now;
    if (dt <= 0 || dt > 60s) { anomaly++; return; }        // 不入 n/sum/min/max
    if (dt >= 25600) over++;                               // 溢出桶
    else hist[dt / 200]++;                                 // 桶自增（饱和则置位去重计数）
    n++; sum += dt; min/max 更新;
}
```
- **无锁**（单写者 = `loopTask`）、**零输出**、**零分配**、**O(1)**。
- `over` 样本仍计入 `n`/`sum`/`min`/`max` ⇒ `max_us` **永不因直方图分辨率而失真**。

### 3.2 读路径（冷路径，有上界）

- 单临界区整体拷贝 **512 B 直方图 + 56 B 标量**，预算 **≤10 µs**；
- 同一次调用内**自测**该段耗时（`snap_crit_max_us`，含临界区进出 + 拷贝）⇒ 目标板上一次查询就能取证，无需额外埋点；
- 读者可能在 esp-mqtt 任务（云端命令）或 `loopTask`（串口 `cm {}`）⇒ 临界区跨核保护成立；**读路径不调用 `tick()`**，无嵌套临界区风险。

### 3.3 关键边界条件（逐条落地）

| 边界 | 处置 | 单测 |
|---|---|---|
| **首样本** | 丢弃（只建基线），否则首个 `dt` 是不完整周期 | `T1` |
| **桶边界** | `dt=199→桶0`、`200→桶1`、`25599→桶127`、`25600→over` | `T2` |
| **异常样本** | `dt≤0` 或 `dt>60 s` ⇒ `anomaly++`，**不入统计** | `T3` |
| **计数器溢出** | `n` 到 `UINT32_MAX` ⇒ `n_saturated=1`，此后不再增长并**否决 P99** | `T6` |
| **桶饱和** | 桶到 `UINT32_MAX` ⇒ 停下 + 位图**按不同桶去重**计 `sat_buckets`，**否决 P99**（`invalid_reasons:["bucket_saturated"]`，`valid:false`，**不输出 p99 数值**） | `T6` |
| **快照一致性** | 单临界区拷贝；`magic`（`'LST1'`）+ `bucket_count` + `bucket_width_us` 三重几何校验 | `T8` |
| **P99 桶边界** | `20 ms` 与桶宽整除（`static_assert`）⇒ 桶 99 = `[19800,20000)` 全 <20 ms；桶 100 = `[20000,20200)` 全 ≥20 ms | `T4` |
| **P99 落在溢出区** | **不给**伪 P99，改给 `p99_lower_bound_us=25600` + **精确 `p99_max_us`** | `T7` |
| **计数回退/恒等式破坏** | `Δ` 为负 或 `Δn ≠ ΣΔhist + Δover` ⇒ `counter_regression` | `T8` |
| **空窗口** | `Δn=0` ⇒ `insufficient_samples`，**不得**误报"P99 在溢出区" | `T7` |

> ★ 实现期真实修掉了一个 bug：差分计算原先在命中 P99 桶后 `break`，导致恒等式校验用到的 `ΣΔhist` 只是部分和 ⇒ 误判 `counter_regression`。由 **`T9` 端到端用例**捕获（`valid` 期望 1 实得 0），已改为**遍历全部桶**。

### 3.4 接口语义（禁止误读清单，已写入 R2 §4.4）

`scope:"uptime"`（累积非窗口）· `p50_us/p99_us` = **桶上界（不含）** · `min_us/max_us` 为累积极值 · `snap_crit_max_us` 是**读路径**不是热路径 · `mean_us` 才代表平均周期。

---

## 4. 单元测试 / 静态审查 / 构建结果

### 4.1 宿主机单元测试：**82 / 82 通过**

```
$ bash test/loop_stats_host/run.sh
[T1] 可用性与首样本丢弃                 pass=11 fail=0
[T2] 桶边界与溢出桶（桶宽 200µs，量程 25.6ms）  pass=8  fail=0
[T3] 异常样本（dt<=0 / dt>60s）不计入统计      pass=4  fail=0
[T4] 分位计算（rank = ceil(p*N)）        pass=14 fail=0
[T5] 两次快照差分（窗口统计）            pass=5  fail=0
[T6] 桶饱和 / n 饱和 ⇒ 拒绝输出有效 P99  pass=6  fail=0
[T7] 样本不足 / P99 落 overflow / 异常主导 / 空窗口  pass=11 fail=0
[T8] 几何校验 / 计数回退 / 恒等式破坏     pass=11 fail=0
[T9] 端到端（tick→snapshot→compute）     pass=12 fail=0
=== SUMMARY: pass=82 fail=0 ===
```
- 平台：`clang++ 22.1.8 --target=wasm32 -nostdlib -fno-builtin`（freestanding，**无 libc**），由 node 真实执行；
- 探针自带 `memcpy/memset`，**不依赖被测环境**；时间源是探针控制的假时钟 ⇒ 可精确构造边界。
- 关键实测：`sizeof(LoopStatsSnapshot)=576`；`T9` 采样率自洽 `983/s`（counts）vs `984/s`（1/mean）⇒ 偏差 **0.1%**。

### 4.2 静态审查（剥离注释后统计，全部 0）

| 检查项 | `loop_stats.cpp` 结果 |
|---|---|
| `Serial` / `printf` / `log_emit` / `ESP_LOG` | **0 / 0 / 0 / 0** |
| `new` / `malloc` / `String` / `std::` | **0 / 0 / 0 / 0** |
| `delay(` / `vTaskDelay` / `micros(` | **0 / 0 / 0** |
| `loop_stats_tick()` 内临界区 | **0**（热路径无锁） |
| 全文 `portENTER_CRITICAL` 实调用 | **1**（仅在 `loop_stats_get()`） |

> 说明：`grep "Serial"` 等会命中文件头注释里的"禁止清单"，因此审查脚本**先剥离 `//` 与 `/* */`** 再统计（脚本内联在证据卷宗，可复现）。

### 4.3 构建结果（独立构建目录，互不污染）

| 构建 | RAM | Flash | `.bin` | `sha256` |
|---|---|---|---|---|
| **ENABLED=1（交付件）** | **133344 (40.7%)** | **1407105 (67.1%)** | **1407472 B** | `72d7a610…744c` |
| **ENABLED=0（对照件）** | 132744 (40.5%) | 1406393 (67.1%) | 1406752 B | `b7ce1299…b9a5` |
| 上一版生产（`system.stats`，boot 22） | 132744 (40.5%) | 1402305 (66.9%) | 1402672 B | `a2c8a038…`（历史记录） |

- **编译错误 0**；**告警 2 条，均为既有**（`cloud_manager.cpp:1461` `DynamicJsonDocument` 弃用、`main.cpp` `containsKey` 弃用），与上一版 G-4 构建**逐条相同** ⇒ 本包**未新增任何告警**。
- **可复现性**：干净全量构建（417 个编译单元，263 s）与增量构建产出的 `.bin` **字节完全相同**（`72d7a610…`）⇒ 源码 + 锁版本工具链可复现。

---

## 5. RAM / Flash / 固件大小 / 采样开销

### 5.1 三组增量（**不要混用**）

```
A) 本包 vs 上一版生产   : RAM  +600 B  | Flash +4800 B | .bin +4800 B
B) 采样器 开 vs 关      : RAM  +600 B  | Flash  +712 B | .bin  +720 B
C) 关 vs 上一版生产     : RAM     0 B  | Flash +4088 B | .bin +4080 B  ← syscmd_loop/路由/字符串，与采样器无关
```

- **新增静态 RAM = 600 B**（`bss` 1224221 → 1224805 = **+584 B**；`loop_stats.cpp.o` 的 `bss` 为 **586 B**：直方图 512 + 饱和位图 16 + 标量/对齐 58）。
- ★ `ENABLED=0` 的 `bss = 1224221` **恰好等于**上一版生产 ⇒ 宏关闭时采样器 **真正零静态 RAM**（不是"留数组只关逻辑"）。这条是**回滚可信度**的直接证据。
- Flash +4088 B 那部分主要是 `syscmd_loop()` 的 JSON 键名与 `invalid_reasons` 字符串（`.rodata`，落 DROM）。

### 5.2 采样开销

| 项 | 值 | 状态 |
|---|---|---|
| 每轮热路径开销（预算） | **≈1.0–1.8 µs**（`esp_timer_get_time` 0.5–1.5 + 分支/索引/自增 0.1–0.3） | **估算，非实测** |
| 占 20 ms 阈值 | ≤0.009% | 由估算推得 |
| 若 P50 ≈ 1 ms | ≈0.10–0.18% | 由估算推得 |
| **目标板实测** | — | **`NOT VERIFIED`** |
| 读路径临界区（预算 ≤10 µs） | 结构上 512 B 拷贝 + 进出临界区 | **`NOT VERIFIED`**（已内建 `snap_crit_max_us` 出口，烧录后一次查询即得） |

> ★ 不得把上表的"预算/估算"当实测引用。目标板上量化采样器自身影响的**既定方法**：分别烧 `ENABLED=0` 与 `ENABLED=1` 两个固件，比较同一负载下的 `system.loop`/串口对照（需单独授权、含 2 次重启）。

---

## 6. 未完成的目标板验证与正式门禁事项

| # | 事项 | 为什么没做 | 需要什么 |
|---|---|---|---|
| 1 | 烧录 `ENABLED=1` 固件 | **本包明确不烧录** | 独立烧录授权（含 1 次重启、`boot_seq` 22→23） |
| 2 | L2 采集有效性（`n/Δelapsed` 自洽、注入对照响应、`sat=0`） | 需真实采样器 | 烧录后 + S1–S4 冒烟矩阵 |
| 3 | **热路径开销 / 临界区耗时的目标板实测** | 同上 | 烧录后读 `snap_crit_max_us`；开销用 `ENABLED=0/1` 对照 |
| 4 | **L3 P99 达标** | 无数据 | L2 通过 + **2 h 稳态 + 30 min 突发**（另行授权） |
| 5 | `G-4` 解锁 | 本项只是五项之一；`G4-R4`（DRAM 口径）**仍待人工裁决** | 人工裁决 + 全部五项齐备 |
| 6 | `cloud_poll()` 单次耗时（`S3-U1`）、分模块埋点（`S3-U2`） | 范围外 | 另行立项 |

---

## 7. 风险、回滚与归档位置

### 7.1 风险

| # | 风险 | 缓解 | 现状 |
|---|---|---|---|
| R1 | 采样开销扰动被测对象 | `ENABLED=0` 对照件已构建；报告强制声明"含开销" | 已缓解（未实测） |
| R2 | 读路径临界区过长（>10 µs） | 内建 `snap_crit_max_us` 自测；超预算则改分块拷贝/双缓冲 | **待目标板取证** |
| R3 | 桶饱和导致 P99 失真 | 桶 `uint32`（49.7 天 @1 kHz）+ 饱和即**拒绝输出 P99** | 已消除/已兜底 |
| R4 | `n` 溢出 | `n_saturated` 标志 + 否决 P99 | 已兜底 |
| R5 | `micros()` 误用致 71.6 min 回绕 | 代码级禁用；单测覆盖 `dt` 异常 | 已消除 |
| R6 | 误并入 `system.stats` 破坏既有解析方 | 新 object，`system.stats` **零改动** | 已规避 |
| R7 | 静态 RAM +600 B 抬高内存基线 | `minimum_free` 跨 boot **不可比**（本包新 boot 必然需重建基线） | 已登记 |
| R8 | 串口/日志回归制造假长尾 | 采样器零输出（静态审查 0 命中） | 已规避 |
| R9 | 采样点位置被后续改动挪走 | `main.cpp` 注释标注"必须是第一行"；报告 §3.1 明示 | 已登记 |

### 7.2 回滚（三级）

| 级 | 动作 | 代价 | 验证 |
|---|---|---|---|
| 代码级 | `-DGF_LOOP_STATS_ENABLED=0` 重建 | **零静态 RAM**（`bss` 已证与上一版相同）；仅残留一次空函数调用（≈0.04 µs/轮） | 已有**对照件** `s3-loopstats-CONTROL-off-firmware.bin`（`b7ce1299…`） |
| 固件级 | 回烧已归档的上一版 `.bin`（`a2c8a038…` / 1402672 B） | **1 次重启**（需授权） | 烧后核对 `boot_seq`+1、`system.stats` 正常、零丢弃 |
| 状态级 | 无 | — | `G-4` 仍 `BLOCKED`，本包**不产生**任何门禁状态变更 |

### 7.3 归档位置（`S-2` 纪律：diff + `.bin` + SHA-256 + 构建配置 + 构建日志）

`docs/evidence/S-3-2026-10-20/`

| 文件 | 内容 |
|---|---|
| `source.diff` | 完整源码差异（10 文件 / +1351 行），`sha256=055717b0…5612` |
| `s3-loopstats-firmware.bin` | 交付件，1407472 B，`sha256=72d7a610…744c` |
| `s3-loopstats-CONTROL-off-firmware.bin` | 对照件，1406752 B，`sha256=b7ce1299…b9a5` |
| `build-clean.log(.gz)` | 干净全量构建日志（417 编译单元） |
| `build-incremental.log(.gz)` / `build-control-off-incremental.log(.gz)` | 增量与对照构建日志 |
| `build-metrics.txt` | RAM/Flash/段尺寸/三组增量/可复现性/告警对照 |
| `host-probe.log` | 82 项断言完整输出，`sha256=ae8057da…1f1f` |
| `platformio.ini.snapshot` | 构建配置快照（含 `-DGF_LOOP_STATS_ENABLED=1`） |
| `git-status.txt` | 提交前工作区快照 |

> `.log` 受 `.gitignore`（`*.log`）限制，已同时归档 `.gz`（可解压复原）。

---

## 8. Git 状态

| 项 | 值 |
|---|---|
| 分支 | `wb`（同步目标 `origin/main`，**同步只走 `git push origin wb:main`**） |
| 本包源码改动 | 10 文件 / **+1351 / -0** |
| 新增文件 | `src/app/loop_stats.h` · `src/app/loop_stats.cpp` · `test/loop_stats_host/{loop_stats_probe.cpp,run.sh,run_probe.js}` |
| 提交 | **`23bee8e`**（`feat(s3): loop() 耗时观测实施 + 只读 system.loop`） |
| 工作区 | 提交后仅剩 1 个**有意未跟踪**文件 `docs/evidence/G-4-Stats-2026-10-20/pre-flash-firmware.bin`（1.4 MB，SHA 已记录在 G-4 证据 README） |

---

## 9. 变更声明（精确）

| 类别 | 本包是否产生变更 | 说明 |
|---|---|---|
| **文档** | ✅ 有 | 新增本报告 + `S-3-Spec-Revision-R2.md` + 证据卷宗；`docs/issues` 登记 3 项；`HANDOFF`/`docs/README` 索引同步 |
| **源代码** | ✅ 有 | 10 文件 +1351 行（见 §2），**无删除、无重构、无无关文件改动** |
| **构建产物** | ✅ 有 | 2 个固件（交付件 + 对照件）+ 4 份构建日志，均已归档 |
| **设备（烧录/复位/命令）** | ❌ **无** | 全程未开串口、未连设备 |
| **设备状态（`boot_seq`/计数/配置）** | ❌ **无** | 未与设备交互 |
| **数据库（D1）** | ❌ **无** | 未执行任何 SQL |
| **云端部署（Worker/Pages）** | ❌ **无** | 未部署 |
| **EMQX 配置** | ❌ **无** | — |
| **门禁状态** | ❌ **无** | `BT-1 CLOSED` / `G-4 BLOCKED` / `WP-5 未解锁` / `STOP-R1 无解除` |
| **既有证据** | ❌ **无** | `docs/evidence/` 其他目录未触碰（仅新增 `S-3-2026-10-20/`） |
| **`minimum_free` 跨 boot 比较** | ❌ **未做** | 本包无设备数据 |

---

## 10. 提交与推送

| 项 | 值 |
|---|---|
| 提交哈希 | **`23bee8e`**（父提交 `d595997` = S-3 R1 规格文档） |
| 规模 | 26 文件 / **+3520 行**（含证据卷宗与两个固件；**源码本体 = 10 文件 / +1351 行**） |
| 推送 | ✅ `git push origin wb:main` → **`d595997..23bee8e`**；`HEAD == origin/main == 23bee8e` |
| 工作区 | 干净，除 **1 个有意未跟踪** 文件 `docs/evidence/G-4-Stats-2026-10-20/pre-flash-firmware.bin`（1.4 MB，属上一轮 G-4 证据，SHA 已记录） |

---

## 附：下一步（需人工授权，不在本包范围）

1. **烧录授权**（1 次重启，`boot_seq` 22→23）→ 读 `system.loop` 的 `snap_crit_max_us` 与 `n/elapsed` 自洽 ⇒ **L2 判定**。
2. L2 通过后，按 S1–S4 冒烟矩阵跑一遍（各 10 min / 35 条突发）。
3. 最后才是 **2 h 稳态 + 30 min 突发**的 L3 判定（`Q1` 绝对口径）。
4. `G4-R4`（DRAM 口径）仍需人工裁决 —— 它和 `loop()` P99 是**两个独立阻塞项**，本包只解决了后者中的"能不能测"。
