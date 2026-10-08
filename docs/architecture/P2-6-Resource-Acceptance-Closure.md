# P2-6 Resource Acceptance — Closure（新预算下的重新验收）

> **性质**：在 **P2-0.2 Flash Budget Amendment** 之后重新执行 P2-6 acceptance。**documentation-only**：未修改任何 firmware / test / Cloudflare / EMQX / `platformio.ini` / partition 代码。
> **状态**：**`P2-6 = PASS`**（新 ceiling **14 KiB = 14,336 B**）· **`P2-7 = READY`**（资源前置已满足）
> **基线**：代码基线 **`6775bb3`**（§T 复现说明）· 对比基线 **`623da59`**（P2 之前，§17.1 F-26 测量点）
> **上位冻结文档**：`P2-0-architecture-review-and-design-freeze.md` **§17.2 / §17.2.1（P2-0.2 修订）**
> **前置文档**：`P2-6-Resource-Acceptance.md`（旧预算下的 FAIL 记录，**保留不改**）· `P2-6-Optimization-Feasibility-Audit.md` · `P2-Flash-Budget-Reconciliation.md`

---

## 0. 结论摘要（先读这段）

| 项 | 值 |
|---|---|
| **旧 ceiling** | `+4…8.5 KiB` = **8,704 B** |
| **新 ceiling（§17.2.1）** | **≤ 14 KiB = 14,336 B** |
| **实测 Flash 合计增量**（B − base，段级） | **+12,548 B** |
| **裁定** | **`12,548 ≤ 14,336` ⇒ PASS** |
| headroom | **+1,788 B**（占 ceiling 12.5% · 占当前用量 14.2%） |
| **P2-6 Overall** | **`PASS`** |
| **P2-7** | **`READY`**（资源前置满足；实物测试另行准备，见 `P2-7-Hardware-Test-Readiness.md`） |

> 本次**仅**因 §17.2.1 预算修订而由 FAIL 转为 PASS；**实现代码零改动**（`git diff 6775bb3 HEAD -- src/ platformio.ini partitions.csv` 为空）。旧 FAIL 记录（`P2-6-Resource-Acceptance.md`）作为历史证据**原样保留**。

---

## 1. 预算修订依据

| 项 | 内容 |
|---|---|
| 修订文件 | `docs/architecture/P2-0-architecture-review-and-design-freeze.md` **§17.2.1**（P2-0.2 Flash Budget Amendment，2026-10-09） |
| 修订内容 | `.text` / `.rodata` **不再单列为硬性 acceptance ceiling**；**Flash 合计增量上限 = `≤ 14 KiB`（14,336 B）** |
| 保持不变 | DRAM 静态表 0 B · PSRAM description 4096 B · 栈 ≤512 B · CPU · MQTT 缓冲 · 稳态零流量 · §17.3 禁止事项 · §17.4 变化检测 |
| 追溯审计 | `P2-Flash-Budget-Reconciliation.md`（`b1ca1de`）· `P2-6-Optimization-Feasibility-Audit.md`（`b5d0c56`） |
| 归因结论 | **D. 预算与功能共同漂移**（原 `.text` 预算只有"行数"一个推算变量，其上游假设对应更早的 *Node 4–5 / Property 8–10* 范围；最终冻结范围 = 4 node / 16 property / 6 命令 + Device Model 独立层 + Transport Facade + 分片重组 + 订阅注册表 + pending 结果分类 + fingerprint 哨兵 + 诊断，**行数未随之重估**） |

---

## 2. 复测方法与可复现性

### 2.1 段级测量（唯一裁决口径）

- `xtensa-esp32s3-elf-objdump -h`（输入段 VMA + size）读 `.flash.text` / `.flash.rodata` / `.dram0.data` / `.dram0.bss` / `.iram0.text` / `.flash.rodata_noload` / `.flash.appdesc`；`firmware.bin` 取文件字节数。
- **权威裁决依据 = `B − base` 段级增量**（非逐模块 `.o` 数字，后者仅用于成因解释）。

### 2.2 四个 clean build

| 代号 | 定义 | 构建方式 |
|---|---|---|
| **base** | `623da59`（P2 之前） | 独立 worktree `.p26base`，`PLATFORMIO_BUILD_DIR=.pio/build/p26base` |
| **A** | Homie disabled = **default build**（无 `PLATFORMIO_BUILD_FLAGS`） | `.pio/build/p27off` |
| **B** | `-DGF_HOMIE_ENABLED=1` | `.pio/build/p27on` |
| **C** | `-DGF_ENV_PRODUCTION=1 -DGF_HOMIE_ENABLED=1` | `.pio/build/p27prod` |

### 2.3 代码状态一致性证明

```
git diff 6775bb3 HEAD -- src/ platformio.ini partitions.csv   ⇒  空
git diff --name-status 6775bb3 HEAD                          ⇒  仅 2 个 .md（docs/architecture/）
```
⇒ 产生 `p27{off,on,prod}` 的代码状态与当前 HEAD **完全一致**（其间只新增了审计文档）。

### 2.4 复现性验证（本次新增，独立 clean build）

| 构型 | 构建 | `.flash.text` | `.flash.rodata` | `.dram0.bss` | `.iram0.text` | `firmware.bin` |
|---|---|---:|---:|---:|---:|---:|
| B（既有 `p27on`） | 6775bb3 | 1,030,435 | 263,664 | 109,520 | 82,311 | 1,401,888 |
| B（**独立重编** `p26recheck`） | 当前 HEAD | **1,030,435** | **263,664** | **109,520** | **82,311** | **1,401,888** |

> **逐段、逐字节一致** ⇒ 测量链路可复现，数字可信。

---

## 3. 段级实测数据

### 3.1 四构型绝对量

| 段 / 度量 | **base** | **A**（default） | **B**（Homie） | **C**（prod+Homie） |
|---|---:|---:|---:|---:|
| `firmware.bin` | 1,389,328 | 1,391,040 | **1,401,888** | 1,402,320 |
| `.flash.text` | 1,021,171 | 1,022,443 | **1,030,435** | 1,030,663 |
| `.flash.rodata` | 260,380 | 260,824 | **263,664** | 263,860 |
| `.flash.rodata_noload` | 16,009 | 16,009 | 16,009 | 16,009 |
| `.flash.appdesc` | 256 | 256 | 256 | 256 |
| `.dram0.data` | 24,072 | 24,072 | **24,088** | 24,088 |
| `.dram0.bss` | 107,072 | 108,632 | **109,520** | 109,520 |
| `.iram0.text` | 82,311 | 82,311 | **82,311** | 82,311 |
| `.ext_ram.*`（静态 PSRAM） | **不存在** | 不存在 | 不存在 | 不存在 |

### 3.2 增量（相对 base）

| 项 | **A − base**（P2-2 Facade） | **B − A**（Homie 净） | **B − base**（**裁决口径**） | **C − B**（生产门禁） |
|---|---:|---:|---:|---:|
| `firmware.bin` | +1,712 | +10,848 | **+12,560** | +432 |
| `.flash.text` | +1,272 | +7,992 | **+9,264** | +228 |
| `.flash.rodata` | +444 | +2,840 | **+3,284** | +196 |
| **`.text + .rodata` 合计** | **+1,716** | **+10,832** | **+12,548** | **+424** |
| `.dram0.data` | 0 | +16 | **+16** | 0 |
| `.dram0.bss` | +1,560 | +888 | **+2,448** | 0 |
| `.iram0.text` | 0 | 0 | **0** | 0 |
| `.flash.rodata_noload` | 0 | 0 | **0** | 0 |

---

## 4. Flash 预算裁决 = **PASS**

| 判据（§17.2.1） | 上限 | 实测（B − base） | 裁决 |
|---|---:|---:|---|
| **Flash 合计增量** | **≤ 14,336 B** | **+12,548 B** | **PASS** |
| headroom | — | **+1,788 B**（12.5% of ceiling · 14.2% of usage） | — |
| `.flash.text`（观测项，非 ceiling） | 不再单列 | +9,264 B | 记录 |
| `.flash.rodata`（观测项，非 ceiling） | 不再单列 | +3,284 B | 记录 |

> **正式结论**：**`P2-6 Flash Budget = PASS`**（新冻结 ceiling `≤ 14 KiB`）。
> **物理 Flash 备注**：app 分区 `app0/app1` 各 `0x200000 = 2,097,152 B`；C 构型 `firmware.bin = 1,402,320 B`，占用 **66.9%**，余 **694,832 B**。该余量是背景事实，**不构成**预算依据（预算为自设上限）。

---

## 5. 非 Flash 资源逐项复核（全部沿用 §17.2 / 旧裁决口径）

| 项 | 冻结要求 | 实测 | 裁决 |
|---|---|---|---|
| **新增 DRAM 静态语义/描述数据表** | 0 B | `device_model.cpp.o`：`data = 0` · `bss = 0`；全部表（`kProperties` 896 B / `kNodes` 48 B / `kCmdsValve` 24 B / `kEnumValve` 16 B / …）均为 **`r`（只读）** ⇒ `.flash.rodata` | **PASS** |
| **Homie runtime buffers** | runtime state/cache（非预算违规项） | `.dram0.bss +2,448` · `.dram0.data +16` ⇒ **+2,464 B**（`s_prop_state[32]` 640 B · pending · base/runtime 缓冲；P2-2 Facade 队列/注册表/LWT 1,560 B 计入 A 构型） | 记录（非 static-table 违规） |
| **静态 PSRAM** | ❌ 禁 `EXT_RAM_BSS_ATTR` | 三构型均**无 `.ext_ram.*` 段**；全仓 `grep EXT_RAM src/` = 0 | **PASS** |
| **PSRAM description** | 4096 B，boot 一次分配、复用 | `heap_caps_malloc(4096, MALLOC_CAP_SPIRAM)`（`homie_bridge.cpp:310`），`if (s_desc_buf == nullptr)` 守卫、**无 `free()`** | **PASS** |
| **DRAM fallback** | — | `homie_bridge.cpp:314` 存在 `MALLOC_CAP_8BIT` 回退（可达 4096 B DRAM 堆） | **WARNING**（如实记录；不写 "DRAM heap = 0 无条件 PASS"） |
| **`homie_task()` 栈帧** | ≤ 512 B | **240 B**（`entry a1, 240`） | **PASS**（余 272 B） |
| **`$description` 单包** | ≤ 4096 B（目标 ≤2048） | **1,907 B**（16 属性）/ **1,618 B**（weight 未装，13 属性） | **PASS** |
| **MQTT 分片** | 不分片 | 1,907 B < `CLOUD_MQTT_BUFFER_SIZE = 8192` | **PASS** |
| **稳态 state 取值** | ~21 次 / loop | **13 次 / loop**（`DM_SRC_STATE` 属性数 = 13 / 16） | **PASS** |
| **IRAM 增量** | — | `.iram0.text` 82,311 三构型 **+0** | **PASS** |
| **boot description 构建耗时** | < 5 ms | **`NOT DEVICE-MEASURED`**（无真机 wall-clock） | 未验证 |
| **网络稳态零流量** | 变化驱动 | **`NOT RUN`**（未做设备侧抓包） | 未验证 |
| **OQ-3 weight 死区** | 1.0 g + 1000 ms | `deadband = 1.0f` · `min_pub_ms = 1000`（编译期 `static_assert` pin 死） | **静态 PASS** |
| **OQ-3 真机校验** | — | **deferred**（HX711 未安装） | 未验证；默认值未改 |

---

## 6. 回归测试（**代码零改动**，全部重跑）

### 6.1 宿主 / 静态测试

| 套件 | 结果 |
|---|---|
| Device Model host（正向 + 负向探针） | **harness OK**（正向 PASS；负向如期 FAIL 7 项） |
| MQTT Transport host | **74 PASS / 0 FAIL** |
| Homie Projection / Bridge host | **222 PASS / 0 FAIL** |
| P2-2 transport 静态检查 | **45/45** |
| P2-3 homie 静态检查 | **116/116** |
| P2-4 `/set` 静态检查 | **64/64** |
| P2-4 weight valid 静态检查 | **21/21** |
| P1-7 production 门禁（含 `--bins`） | **27 PASS / 0 FAIL** |

### 6.2 三构型构建

| 构建 | 结果 |
|---|---|
| **default**（无 flags，Homie disabled） | `[SUCCESS]` |
| **Homie enabled**（`-DGF_HOMIE_ENABLED=1`） | `[SUCCESS]`；独立重编复现逐段一致 |
| **production + Homie**（`-DGF_ENV_PRODUCTION=1 -DGF_HOMIE_ENABLED=1`） | `[SUCCESS]` |

> 全部构建 `grep -c 'error:'` = 0。所有回归保持 PASS ⇒ 预算修订**未改变任何代码行为**。

---

## 7. 最终状态

```
P2-6 Overall            = PASS  （新 ceiling ≤ 14 KiB / 14,336 B，§17.2.1）
  Flash 合计增量        = +12,548 B        ≤ 14,336 B        PASS
  headroom             = +1,788 B  （12.5% of ceiling）
  .flash.text   (观测)  = +9,264 B
  .flash.rodata (观测)  = +3,284 B
Device Model DRAM       = 0 B                              PASS
Homie runtime DRAM      = +2,464 B （runtime state/cache）
static PSRAM            = 0                                PASS
PSRAM description       = PASS （4096 B，一次分配，复用，无 free）
DRAM fallback           = WARNING （MALLOC_CAP_8BIT 回退可达）
stack (homie_task)      = 240 B                            PASS
$description            = 1,907 B / 1,618 B                 PASS
steady-state reads      = 13 / loop                        PASS
IRAM 增量               = 0                                PASS
boot description timing = NOT DEVICE-MEASURED
network steady capture  = NOT RUN
OQ-3 deadband           = static PASS / real-HX711 deferred

回归                    = 全绿（host 74+222 / 静态 45+116+64+21 / P1-7 27 / 三构型 build）
P2-7                    = READY （资源前置满足；实物测试见 P2-7-Hardware-Test-Readiness.md）
```

---

## 8. 范围与纪律声明

**本阶段仅**新增/修改文档：`P2-0-…freeze.md` §17.2.1（预算条款）+ 本文件 + `P2-7-Hardware-Test-Readiness.md`。

**未修改**：任何 `src/` 代码 · `platformio.ini` · partition · ACL · Cloudflare · EMQX · 测试脚本 · 任何 frozen 行为/架构/ABI/topic 契约。

**未做**：为省 Flash 的新优化 · 删除 diagnostics · 删除 retained `/set` 防护 · 删除 validation · 修改 P2-7 frozen vertical slice · 进入 P2-8 · push。

**证据位置（scratch / 非受跟踪）**：`.pio/build/p26recheck`（复现 B）· `.pio/build/p26recheck_prod`（复现 C）· `.pio/build/p27{off,on,prod}` · worktree `/d/Guo_Feeder_Project/.p26base`（base `623da59`）。
