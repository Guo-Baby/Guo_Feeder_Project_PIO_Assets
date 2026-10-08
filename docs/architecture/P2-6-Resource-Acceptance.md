# P2-6 Resource Acceptance（资源实测与冻结预算裁决）

> **性质**：资源实测 + 冻结预算正式裁决。**本阶段未修改任何 firmware / test / Cloudflare / EMQX 代码；未做任何优化；未 commit 任何代码变更。**
> **状态**：`P2-6 = FAIL`（Flash 超冻结 §17.2 budget）· `P2-7 = BLOCKED` · `DRAM 判据歧义 = 本次正式裁决（已消除）` · `PSRAM = PASS` · `DRAM fallback = WARNING`
> **基线**：Firmware **`28aec44`**（wb）· 对比基线 **`623da59`**（P2 之前，§17.1 F-26 的测量点）
> **上位冻结文档**：`docs/architecture/P2-0-architecture-review-and-design-freeze.md`（§17 / §17.2 / §17.3 / §17.4）
> **本报告数据来源**：4 个 clean build 的 ELF / map / 反汇编 + 宿主 wasm 代理。**未重新测量、未重新解释数字**——所有数字取自本轮 P2-6 实测原始输出。

---

## 0. 结论摘要（先读这段）

| 项 | 结果 |
|---|---|
| **P2-6 Overall** | **`FAIL`** |
| 原因 | Homie-enabled 构型的 **Flash 增量超过冻结 §17.2 总预算上限（+8.5 KiB）** |
| 实际 | `.text` **+9,712 B** · `.rodata` **+3,604 B** · **合计 +13,316 B ≈ 13.0 KiB** |
| **P2-7** | **`BLOCKED`**（§20 依赖链 P2-6 → P2-7 未满足） |
| Waiver | **不予授予**（本阶段不自行豁免） |
| DRAM 判据 | 本次正式裁决：约束针对**静态语义/描述数据表** ⇒ **Device Model static model data = 0 B DRAM → PASS**；Homie runtime buffers（+2,464 B）属 runtime state/cache，**不**构成 static-table budget 违规 |
| PSRAM | `PSRAM description = PASS` · `DRAM fallback = WARNING`（如实记录，不隐藏） |

> **明确声明**：**当前物理 Flash 空间充足并不改变 frozen resource budget 的 FAIL 结论。** 物理余量（见 §3.3）仅为背景信息，不构成豁免理由。

---

## 1. 测量方法

### 1.1 四个 clean build 构型

所有构型均为**全量构建目录**（避免 `.pio` 旧产物与 SCons 增量干扰）：

| 代号 | 定义 | 构建方式 |
|---|---|---|
| **base** | `623da59`（P2 之前，§17.1 基线） | 独立 worktree `.p26base`，`PLATFORMIO_BUILD_DIR=.pio/build/p26base` |
| **A** | Homie disabled（默认 `GF_HOMIE_ENABLED=0`） | `.pio/build/p26off` |
| **B** | `-DGF_HOMIE_ENABLED=1` | `.pio/build/p26on` |
| **C** | `-DGF_ENV_PRODUCTION=1 -DGF_HOMIE_ENABLED=1` | `.pio/build/p26prod` |

### 1.2 手段

- **段级**：`xtensa-esp32s3-elf-size` + `objdump -h`（VMA + size）
- **逐对象归因**：`firmware.map` 的 live 区输入段归因（跳过 *Discarded input sections*）+ `nm -S --defined-only` 的地址区间分类
- **栈帧**：`objdump -d` 反汇编，取函数 prologue 的 `entry a1, N`（GCC 帧分配即 N）
- **宿主逻辑代理**：`clang++ --target=wasm32 -nostdlib` 编译 Device Model + Homie 投影纯逻辑，`node` 运行

### 1.3 基线自证（证明测量口径正确）

base 构型实测 **`text = 1,120,774 / data = 284,452 / bss = 1,222,621`**，与 §17.1 F-26 记录值**逐位一致** ⇒ 基线可信、测量链路正确。

---

## 2. 实测数据（ELF / map）

### 2.1 汇总表

| 段 / 度量 | **base** `623da59` | **A** Homie off | **B** Homie on | **C** prod+Homie |
|---|---|---|---|---|
| `firmware.bin` | 1,389,328 | 1,391,040 | **1,402,656** | 1,403,056 |
| `size.text` | 1,120,774 | 1,122,046 | 1,130,486 | 1,130,690 |
| `size.data` | 284,452 | 284,896 | 288,072 | 288,268 |
| `size.bss`（含 PSRAM dummy 段） | 1,222,621 | 1,224,181 | 1,225,069 | 1,225,069 |
| `.flash.text` | 1,021,171 | 1,022,443 | **1,030,883** | 1,031,087 |
| `.flash.rodata` | 260,380 | 260,824 | **263,984** | 264,180 |
| `.flash.rodata_noload` | 16,009 | 16,009 | 16,009 | 16,009 |
| `.flash.appdesc` | 256 | 256 | 256 | 256 |
| `.iram0.text` | 82,311 | 82,311 | **82,311** | 82,311 |
| `.iram0.vectors` | 1,027 | 1,027 | 1,027 | 1,027 |
| `.dram0.data` | 24,072 | 24,072 | **24,088** | 24,088 |
| `.dram0.bss` | 107,072 | 108,632 | **109,520** | 109,520 |
| `.xt.lit` | 26,064 | 26,088 | 26,184 | 26,184 |
| `.ext_ram.*`（静态 PSRAM） | **不存在** | 不存在 | 不存在 | 不存在 |

> `size.bss` 之所以远大于 `.dram0.bss`，是因为其中含 `.flash_rodata_dummy`（1,048,576 B NOBITS 占位）等非 RAM 段。**真实 RAM 口径 = `.dram0.data + .dram0.bss`**（平台亦如此计）。

### 2.2 增量（相对 base）

| 项 | **A−base**（P2-2 Facade 加性） | **B−A**（Homie 净增量） | **B−base**（P2 总增量，**裁决口径**） |
|---|---|---|---|
| `firmware.bin` | +1,712 | +11,616 | **+13,328** |
| `.flash.text` | +1,272 | +8,440 | **+9,712** |
| `.flash.rodata` | +444 | +3,160 | **+3,604** |
| **`.text + .rodata` 合计** | **+1,716** | **+11,600** | **+13,316** |
| `.xt.lit` | +24 | +96 | +120 |
| `.iram0.text` | **0** | **0** | **0** |
| `.dram0.data` | 0 | +16 | **+16** |
| `.dram0.bss` | **+1,560** | **+888** | **+2,448** |

### 2.3 逐模块归因（map / 保留符号）

**Homie-enabled 构型（B）**：

| 模块 | `.flash.text` | `.flash.rodata` | `.xt.lit` | `.dram0.data` | `.dram0.bss` |
|---|---|---|---|---|---|
| Device Model（P2-1）`device_model.cpp.o` | 1,298 | **1,546** | 128 | **0** | **0** |
| Homie Bridge（P2-3/4）`homie_bridge.cpp.o` | 6,669 | 1,290 | 272 | 4 | 884 |
| Transport Facade（P2-2）`mqtt_transport.cpp.o` | 81 | — | 64 | 0 | 0 |
| `cloud_manager.cpp.o`（P2-2 加性） | +1,761（TU 级） | — | — | 0 | **+1,527** |
| `system_state.cpp.o`（阶段 A） | +29 | — | — | 0 | **+32** |
| `weight.cpp.o`（阶段 A） | +32 | — | — | 0 | 0 |
| `main.cpp.o`（接线） | +18 | — | — | 0 | 0 |

**Homie disabled 构型（A）的零代价证据**：
- `device_model.cpp.o` 在最终 ELF 中**保留符号数 = 0**（整模块被 `--gc-sections` 丢弃）
- `homie_bridge.cpp.o` **17 B**（仅 3 个空实现存根）
- ⇒ 默认构型（`GF_HOMIE_ENABLED=0`）**零 Homie 代价**成立

**P2-2 的静态 buffer 归属澄清**：`mqtt_transport.cpp.o` 本体仅 145 B；其 `.bss` 增量 **1,527 B 实际定义在 `cloud_manager.cpp.o`**（foreign 队列 / 订阅注册表 / LWT 缓冲 / 重组器）。**该 +1,527 B 与 Homie 无关**（A 构型即已存在）。

### 2.4 归因限制（如实声明，不伪造数字）

三套口径数字**互不相等**，原因如下，报告不强行凑平：

1. **`--gc-sections` 丢弃**：`.o` 级尺寸含未被引用的代码（如 `device_model.cpp.o` 总 2,972 B，最终仅保留 2,844 B 可归因）；
2. **header-only 纯逻辑跨 TU 内联**：`homie_projection.h` / `mqtt_routing.h` 的函数被内联进调用方 TU，其代码尺寸不出现在自身 `.o` 的符号里；
3. **匿名字符串字面量**：`.LC*` 一类局部字面量不进入符号表，无法按符号名归因（`.rodata` 增量中约 **2.0 KB 属此类**）；
4. **map 续行解析**：部分长段名的续行会落到相邻条目，`homie_bridge.cpp.o` 有约 **2.8 KB** 未能归类（均为 Flash 侧 `.xt.lit`/`.text`，**不影响 DRAM 结论**）。

**因此**：`B−base` 的段级增量（§2.2）是**权威裁决依据**；逐模块数字（§2.3）用于**解释成因**，不作为预算判据。

---

## 3. 正式裁决

### 3.1 Flash 预算 = **FAIL**

| 冻结项（§17.2） | 预算上限 | 实测（B−base） | 裁决 |
|---|---|---|---|
| Flash `.rodata` | +1.2 … 2.5 KB | **+3,604 B（+3.52 KiB）** | **FAIL** |
| Flash `.text` | +3 … 6 KB | **+9,712 B（+9.48 KiB）** | **FAIL** |
| **Flash 合计** | **+4 … 8.5 KB** | **+13,316 B（+13.0 KiB）** | **FAIL** |

**数学依据（任何口径下均超）**：

- 上限 **8.5 KiB = 8,704 B** ⇒ 超出 **+4,612 B**；若按 8.5 KB = 8,500 B ⇒ 超出 **+4,816 B**；
- 即便只计**纯 Homie 净增量**（B−A = **+11,600 B ≈ 11.3 KiB**），仍超 8,704 B；
- 因此**不存在"inclusive upper bound"的解读空间**——本裁决**不通过文字解释为 PASS**。

> **正式结论**：**`FAIL — resource budget exceeded`**

### 3.2 物理 Flash 空间的定位（**不构成豁免**）

app 分区 `app0/app1` 各 `0x200000 = 2,097,152 B`（2 MB）；B 构型 `firmware.bin = 1,402,656 B`，占用 **66.9%**，余量 **694,496 B**。

> **明确**：物理空间充足是**事实描述**，**不是** budget 豁免依据。§17.2 是 **FROZEN 上限（非目标）**，超限即 FAIL，需由**独立优化任务**消除差量，或由授权方**显式授予 waiver**（本阶段**不予授予**）。

### 3.3 P2-7 = **BLOCKED**

P2-6 未 PASS ⇒ §20 依赖链 **P2-6 → P2-7 不满足** ⇒ **`P2-7 = BLOCKED`**。本阶段**不进入 P2-7**。

---

## 4. DRAM 判据正式裁决（消除歧义）

### 4.1 歧义原文

§17.2 该行原文为：

> **新增 DRAM 静态数组** | **0 B** | 铁律；**所有表 `const` → Flash**

歧义点：字面可读作"任何新增 static 数组 = 0 B"，与说明行的"**表** `const` → Flash"口径不一致。

### 4.2 **本次正式裁决（FROZEN for P2 后续）**

> 该约束针对**静态语义 / 描述数据表**（static semantic / description data tables），要求 **Device Model 等静态模型数据驻留 Flash**，不得因为实现方式错误而进入 DRAM。

据此：

| 类别 | 定义 | 实测 | 裁决 |
|---|---|---|---|
| **Device Model static model data** | node / property 静态描述表、命令映射表、枚举映射表 | `device_model.cpp.o`：`.dram0.data = 0`、`.dram0.bss = 0`；最终 ELF 中该模块 **无任何符号落在 DRAM**；`kProperties`（896 B）→ `.flash.rodata` | **`0 B DRAM → PASS`** |
| **Homie runtime buffers** | property runtime state（`s_prop_state[32]`）、pending table（`s_pending`）、base / runtime 缓冲（`s_base`） | `.dram0.bss +2,448`、`.dram0.data +16` ⇒ **`+2,464 B`** | **runtime state/cache，不属"语义描述表"** ⇒ **不构成 static-table budget 违规** |

### 4.3 正式记录

- **Device Model 不产生新增 DRAM 静态模型数组** ⇒ 满足 §17.2 该行在本裁决口径下的要求（**PASS**）。
- **Homie runtime DRAM = +2,464 B**（P2-2 Facade 1,560 + Homie 888 + data 16），**如实记录**，供资源规划参考。
- **不得**把这部分解释成 Device Model static-table budget violation。
- 本解释为**本次 P2-6 的正式裁决**，用于消除后续歧义；**未修改 §17.2 原文**（冻结预算未被改动），仅在解释层面定案。

---

## 5. PSRAM 与 DRAM fallback（如实记录）

### 5.1 nominal 路径 = PASS

`$description` 缓冲位于 `src/homie/homie_bridge.cpp`：

- `heap_caps_malloc(4096, MALLOC_CAP_SPIRAM)`（`:306`），容量常量 `HOMIE_DESC_CAP = 4096`
- **一次分配**：由 `if(s_desc_buf == nullptr)` 守卫（`:304`）⇒ 每 boot 至多一次
- **后续复用**：MQTT 重连后重走 `init` 相位时，`s_desc_buf != nullptr` ⇒ 直接复用，不重新分配
- **无 `free()` / 无 reallocation**：全文件无释放调用（永久占用，设计如此）
- **正常路径不占 DRAM heap** ✓

⇒ **`PSRAM description = PASS`**

### 5.2 fallback 路径 = WARNING

同一处代码存在 **DRAM 回退**：

```
s_desc_buf = heap_caps_malloc(HOMIE_DESC_CAP, MALLOC_CAP_SPIRAM);   // :306
if (s_desc_buf == nullptr)
    s_desc_buf = heap_caps_malloc(HOMIE_DESC_CAP, MALLOC_CAP_8BIT); // :310  ← DRAM 回退
```

⇒ 若 4096 B PSRAM 分配失败，将退化为 **4096 B DRAM 堆**占用。

- **不写 `DRAM heap = 0 B PASS`**
- 正式状态：**`DRAM fallback = WARNING`**
- **隐藏 fallback 是被禁止的**；该路径在当前构型下**理论可达**（PSRAM 耗尽 / 分配失败），因此 §17.2「DRAM 堆 0 B」仅在 **nominal（PSRAM 可用）路径**成立。

---

## 6. 其他验收结果（完整保留）

| 项 | 冻结要求 | 实测 | 裁决 |
|---|---|---|---|
| `homie_task()` 栈帧 | ≤ 512 B | **240 B**（`entry a1, 240`） | **PASS**（余量 272 B） |
| `$description` 单包 | ≤ 4096 B | **1,907 B**（16 属性全可用）/ **1,618 B**（weight 未装，13 属性） | **PASS** |
| MQTT 分片 | 不分片（< 8192 缓冲） | 1,907 B < `CLOUD_MQTT_BUFFER_SIZE = 8192` | **PASS** |
| 稳态 state 取值 | ~21 次 / loop | **13 次 / loop**（`DM_SRC_STATE` 属性数） | **PASS**（低于预算） |
| IRAM 增量 | — | `.iram0.text` 82,311 **三构型不变（+0）** | **PASS** |
| 静态 PSRAM | ❌ 禁止 `EXT_RAM_BSS_ATTR` | 无 `.ext_ram.*` 段；全仓无 `EXT_RAM_*` | **PASS** |
| boot description 构建耗时 | < 5 ms | **`NOT DEVICE-MEASURED`** | 无设备判据 |
| 网络稳态零流量 | 变化驱动 | **`NOT RUN`**（未做设备侧抓包） | 未验证 |
| OQ-3 weight 死区 | 1.0 g + 1000 ms（建议值） | `deadband = 1.0f` · `min_pub_ms = 1000`，且被编译期 `static_assert` pin 死 | **静态 PASS** |
| OQ-3 真机校验 | — | **deferred**（HX711 未安装） | 未验证；默认值未改 |

### 6.1 栈帧补充（同构型内其它 Homie 函数，均为参考）

| 函数 | 帧 |
|---|---|
| `homie_task` | **240 B** |
| `homie_publish_values` | 256 B |
| `homie_desc_build`（投影层，boot 路径） | 240 B |
| `homie_on_foreign`（loop 上下文） | 272 B |
| `homie_init`（setup 层，非 loop） | 304 B |

均显著低于 512 B；本构型**无大局部对象导致的栈风险**。

### 6.2 boot description 耗时的宿主代理（**明确标注为 proxy，不得当作 device measurement**）

- 宿主（x86-64 + node/wasm，非 ESP32-S3）执行 `homie_desc_build` **200,000 次**，总墙钟 0.880 s；扣除 node 启动基线 0.287 s ⇒ **≈ 3.0 µs / 次**。
- **该数字仅为宿主机上的相对量级代理**，**不是**设备实测（`NOT DEVICE-MEASURED`），**不得**改写为 device measurement，也不作为 §17.2 `< 5 ms` 的判据。

---

## 7. P2-6 Optimization Required（**仅记录，不实施**）

Flash budget 当前 **FAIL**，需要**独立的 optimization task**（不属于 P2-6、不属于本轮 commit）。候选方向（**仅为候选，未经实施与验证**）：

| # | 方向 | 依据 |
|---|---|---|
| 1 | `$description` 生成路径的**字段名 / 字符串常量收敛** | `.rodata` 超预算 +3,604 B，其中约 2.0 KB 为匿名 `.LC*` 字面量；`$description` 实测 1,907 B，字段名重复出现于多属性 |
| 2 | **Homie capacity 常量审查** | `HOMIE_MAX_PROPERTIES = 32` vs 实际 16 属性 ⇒ `s_prop_state` 静态 640 B（DRAM 侧）；若收敛可同时降低 runtime buffer |
| 3 | **Homie logging / string literal 收敛** | Homie 日志当前走 `Serial.printf` 格式串（EventId 未冻结）；格式串字面量计入 `.rodata` |
| 4 | **header-only projection logic 的 code-size duplication 审查** | `homie_projection.h` / `mqtt_routing.h` 纯逻辑跨 TU 内联，可能造成同一逻辑多份副本（`.text` 侧） |
| 5 | 其他由 **map / ELF 证据**支持的优化点 | 例如 `cloud_manager.cpp.o` 在 Homie 构型下多保留 331 B（因 Facade 调用链拉入）等 |

> **特别注意**：以上**仅为候选方向**，**本 commit 不实施任何优化**。任何优化须另立任务、重新走测量与验收。

---

## 8. 范围与纪律声明

**本阶段未修改、且未触碰**：firmware · Device Model · Homie Bridge · Projection · Transport Facade · CommandManager · Weight · `platformio.ini` · Flash partition · ACL · Cloudflare · EMQX · P2-0 冻结预算。

**未做**：进入 P2-7 · 处理 P2-4-OPEN-1 · 授予 waiver · 为降低 Flash 而重构代码。

**证据位置（全部 scratch / 非受跟踪）**：

- `.pio/p26res/`：`build_all.sh` · `attr.py` · `mapattr.py` · `desc_len.cpp` · `desc_bench.cpp`
- `.pio/build/p26off`、`.pio/build/p26on`、`.pio/build/p26prod`（各含 `firmware.elf` / `firmware.map`）
- 基线 worktree：`/d/Guo_Feeder_Project/.p26base`（`623da59`）

---

## 9. 最终状态

```
P2-6 Overall            = FAIL  （Flash budget exceeded，frozen §17.2）
  .text 增量            = +9,712 B   （预算 +3…6 KB）           FAIL
  .rodata 增量          = +3,604 B   （预算 +1.2…2.5 KB）        FAIL
  Flash 合计增量        = +13,316 B ≈ 13.0 KiB（预算 +4…8.5 KiB）FAIL
Device Model DRAM       = 0 B                          PASS  （正式裁决口径，见 §4）
Homie runtime DRAM      = +2,464 B（runtime state/cache，非预算违规项）
stack (homie_task)      = 240 B                        PASS
$description            = 1,907 B / 1,618 B             PASS
steady-state reads      = 13 / loop                    PASS
IRAM 增量               = 0                            PASS
static PSRAM            = 0                            PASS
PSRAM description       = PASS （4096 B，一次分配，复用，无 free）
DRAM fallback           = WARNING （MALLOC_CAP_8BIT 回退可达）
boot description timing = NOT DEVICE-MEASURED
network steady capture  = NOT RUN
OQ-3 deadband           = static PASS / real-HX711 validation deferred

P2-7                    = BLOCKED
Waiver                  = NOT GRANTED
```
