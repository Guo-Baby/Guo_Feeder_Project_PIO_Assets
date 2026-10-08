# P2-6 Optimization Feasibility Audit

> **性质**：只读可行性审计（analysis-only）。
> **本阶段未修改任何 firmware / test / Cloudflare / EMQX 代码；未修改 P2-0、未修改预算、未 waiver、未删除功能或诊断；未进入 P2-7 / P2-8。**
> 唯一产物 = 本文件（documentation-only commit）。

---

## 0. 结论摘要（先读这一段）

| 问题 | 回答 |
|---|---|
| 是否存在**不改变任何 frozen 行为**、可省 ≥ **3,844 B** 的路径？ | **不存在**（低风险 + 严格行为不变的中风险合计仅 **≈ 1.4–2.1 KB**） |
| 是否存在**接受明确 trade-off**后可触及 8,704 B 的路径？ | **存在，但仅以极小 margin 触及**（乐观上界 ≈ **3,916 B**，比阈值高 **72 B / 1.9%**；且需修订一项冻结措辞） |
| 是否需要人工裁决？ | **需要**（见 §9 / §10 / §11） |

**最终结论（三选一）**：

> ### `B. POSSIBLE WITH TRADE-OFF`

即：达到冻结上限 **必须** 同时接受下列代价 —— `-fno-exceptions`（异常/可靠性语义）、诊断可观测性削减、Device Model representation 修订（需改冻结措辞）、多处中等风险重写 —— 全部接受后**仅余 ~2% margin**，极可能仍需小幅功能面缩减。本轮**不自行裁决**，交人工决定。

**P2-7 / P2-8 继续 `BLOCKED`。**

---

## 1. 基线（本轮固定，不重测）

| 项 | 值 |
|---|---|
| Git HEAD | **`6775bb3`**（`perf(homie): reduce bridge and projection flash footprint`） |
| 上游 | `dd4ec45`（P2-6 验收文档）· `28aec44`（仓库补齐） |
| base 参照 | **`623da59`**（P2 之前），独立 worktree `D:/Guo_Feeder_Project/.p26base` |
| working tree | **clean**（0 modified / 0 untracked） |
| remote | **未 push**（`origin/wb` 仍停在 `623da59`） |

**B − base（本轮唯一权威裁决口径 = 段级 ELF 增量）**

| 段 | base | Homie-on (`p27on`) | Δ |
|---|---:|---:|---:|
| `.flash.text` | 1,021,171 | 1,030,435 | **+9,264** |
| `.flash.rodata` | 260,380 | 263,664 | **+3,284** |
| **合计** | 1,281,551 | 1,294,099 | **+12,548** |
| `firmware.bin` | 1,389,328 | 1,401,888 | **+12,560** |
| frozen 上限 | — | — | **8,704** |
| **缺口** | — | — | **3,844 B（需砍 30.6%）** |

> **口径声明**：`.flash.text` + `.flash.rodata` 的**段级增量**是唯一权威裁决依据；
> `.o` 级 / 符号级数字仅用于**归因**，本文件**不强行**让两者相等（§3.4 给出对账）。

---

## 2. 测量方法（可复现）

| 手段 | 用途 | 工具 |
|---|---|---|
| `objdump -h firmware.elf` | 段级增量的权威来源 | xtensa-esp32s3-elf-objdump |
| **符号级差分**（`nm -S --demangle`，base vs Homie-on，key = (demangled name, type)） | 精确归因每个新增/变化符号的字节数 | `.pio/p26res/symdiff.py` |
| **map 输入段族拆解**（在 `Linker script and memory map` 之后，按 `.text` / `.literal` / `.rodata*` / `.eh_frame` / `.gcc_except_table` 分组） | 区分「真代码 / 地址常量池 / 字符串 / 展开表」 | `.pio/p26res/secbreak.py` |
| map 逐 object 归因（路径折断 `\`+换行已复原） | 逐模块 text/rodata/dram 增量 | `.pio/p26res/mapattr2.py` |
| `objcopy -O binary --only-section=.rodata.str1.1` + 串提取 | 字符串常量清单 | `.pio/p26res/*.str.bin` |
| 本地单 TU 快编（`localcc.sh`）+ 全量 `pio run` | 编译/链接选项实验 | `.pio/p26res/localcc.sh`、`flagbuild*.sh` |

所有 scratch 产物位于 `.pio/`（gitignored），**未进入版本库**。

---

## 3. Flash 根因分析（最终 ELF 中的真实构成）

### 3.1 逐模块增量（`p27on − base`）

| object | Δ`.flash.text` | Δ`.flash.rodata` | ΔDRAM(`bss`+`data`) |
|---|---:|---:|---:|
| `src/homie/homie_bridge.cpp.o` | **+6,231** | **+1,308** | +888 |
| `src/homie/device_model.cpp.o` | **+1,298** | **+1,546** | 0 |
| `src/cloud/cloud_manager.cpp.o` | **+1,224** | +398 | +1,527 |
| `src/cloud/mqtt_transport.cpp.o` | +81 | +136 | 0 |
| `src/services/system_state.cpp.o`（阶段 A） | +46 | +29 | +32 |
| `src/app/weight.cpp.o`（阶段 A） | +16 | 0 | 0 |
| `src/main.cpp.o`（接线） | +7 | −5 | 0 |
| 其余 src（command_manager / log / config） | +4 | −20 | 0 |
| **src 小计** | **+8,907** | **+3,392** | **+2,447** |
| 库 / 对齐 / 残差 | +357 | +212 | +1 |
| **合计** | **+9,264** | **+3,604**※ | **+2,448** |

※ 段级 rodata 增量 3,604 与 src 侧 3,392 差 212 B 落在**库侧**（archive 成员未被 map 归因捕获，路径以 `.a(x.o)` 结尾）。

### 3.2 按「段族」拆解 —— 3,844 B 到底在哪里

| 段族 | 字节 | 占 Δ |
|---|---:|---:|
| **真实代码**（`.text.*`） | **8,443** | 67.3% |
| **只读字符串/表**（`.rodata.*`） | **2,750** | 21.9% |
| **地址常量池**（`.literal.*`，Xtensa 特有、随被引用指针数增长） | **460** | 3.7% |
| **异常展开表**（`.eh_frame`，**.flash.rodata 中**） | **580** | 4.6% |
| 异常表（`.gcc_except_table`） | 82 | 0.7% |
| 残差（库/对齐/appdesc） | 233 | 1.9% |
| **合计** | **12,548** | 100% |

> `.xt.prop` / `.xt.lit`（Xtensa 重定位松弛元数据，`.o` 内约 2 KB）**全部落在 “Discarded input sections”** ⇒ 不占 Flash。
> ⇒ 结论：**没有「元数据泄漏」型浪费**；增量 96% 是有效代码 + 有效只读数据 + 异常表。

### 3.3 结构性拆分：无条件代价 vs Homie 条件代价 ★

对比 **Homie 关闭构型**（`p27off`，`GF_HOMIE_ENABLED=0`）与 base：

| 构型 | `.flash.text` | `.flash.rodata` | DRAM `bss` |
|---|---:|---:|---:|
| base `623da59` | 1,021,171 | 260,380 | 107,072 |
| `p27off`（Homie=0） | 1,022,443 | 260,824 | 108,632 |
| `p27on`（Homie=1） | 1,030,435 | 263,664 | 109,520 |

⇒ **Δ 可一分为二**：

| 部分 | text | rodata | 合计 | 占 Δ |
|---|---:|---:|---:|---:|
| **P2-2 Transport Facade（无条件，Homie 关闭也存在）** | +1,272 | +444 | **1,716** | 13.7% |
| **Homie Bridge + Device Model（仅在 Homie 启用时）** | +7,992 | +2,840 | **10,832** | 86.3% |
| **合计** | +9,264 | +3,284 | **12,548** | 100% |

> ★★ **最关键的一条事实**：**Homie 功能本身（10,832 B）就已经超过 P2 的全部冻结预算（8,704 B）2,128 B** —— 即使 P2-2 的 1,716 B 完全不花，Homie 单靠自身也已超预算。

### 3.4 `.o` / 符号 / 段级三方对账（不做「强行相等」）

| 口径 | homie_bridge | device_model | cloud_manager(P2-2 加性) | 说明 |
|---|---:|---:|---:|---|
| 段级（权威） | 6,231 t / 1,308 r | 1,298 t / 1,546 r | 1,224 t / 398 r | 用于预算裁决 |
| 符号级（命名符号合计） | **5,887 t（= 该 `.o` 的 `.text` 代码量，100% 对上）** | 1,258 t | ~1,550 t（新增符号 − 消失符号） | 用于功能归因 |
| 差额去向 | `.literal` 344 + rodata 1,308 | `.literal` 40 | `.literal` 76 + 库侧 212 | 见 §3.2 |

**限制声明**：archive 成员（`.a(x.o)`）未被 map 归因捕获 ⇒ 无法逐库对象拆分；本文件对该部分只给**合计**，不伪造明细。

---

## 4. 热点 A —— Homie Bridge（7,539 B：text 6,231 + rodata 1,308）

### 4.1 内部段族

| 族 | 字节 |
|---|---:|
| `.text` 真实代码 | 5,887 |
| `.literal` 地址常量池 | 344 |
| `.rodata` 字符串（含 80 B `kGates` 门控表） | 867 |
| `.eh_frame`（异常展开） | 360 |
| `.gcc_except_table` | 81 |

### 4.2 逐功能点拆分（**命名符号合计 5,887 = `.text` 代码量，完全对上，无未归因残差**）

| # | 功能 | 符号（ELF 保留尺寸） | 代码 B | 可优化 | 预估可省 | 改 frozen 行为 | 风险 |
|---|---|---|---:|---|---:|---|---|
| 1 | description 生成 | `homie_desc_build` 932 + `homie_desc_version` 78 | **1,010** | 是（表驱动发射） | 200–300 | 否（须 JSON 逐字节一致） | Med |
| 2 | 属性发布 | `homie_publish_values` 858 + `homie_publish_state` 72 + `homie_state_topic` 44 | **974** | 有限（已很紧） | 60–120 | 否 | Low |
| 3 | lifecycle / 相位机 | `homie_task` 726 + `homie_init` 302 | **1,028** | 有限 | 60–120 | 否 | Low |
| 4 | `/set` 解析与处理 | `homie_on_foreign` 711 + `homie_parse_set_topic` 321 + `homie_id_valid` 109 | **1,141** | 是（解析合并、守卫前置） | 100–200 | 否 | Med |
| 5 | 可用性/有效性门控 | `homie_action_available` 103 + `homie_property_available` 93 + `homie_gate_find` 50 | **246**（+80 B `kGates` rodata） | 是（并入 DM 元数据） | 150–200 | 否（语义等价） | Med |
| 6 | 数值 / JSON 序列化（投影层内联） | `homie_float_to_str` 519 + `float_decimals` 100 + `hp_json_append_string` 179 + `hp_u64_dec` 162 + `hp_eq` 79 + `hp_append` 111 + `hp_copy` 50 + `hp_len` 25 | **1,286** | 是（去除泛型安全网，特化本工程取值域） | 250–400 | 否（须输出逐字节一致） | Med |
| 7 | 杂项 | `homie_fnv1a32_u32` 70 + `homie_pending_find` 60 + `homie_log_outcome` 54 + `homie_log` 18 | **202** | 很少 | ≤ 60 | 否 | Low |
| 8 | **诊断（横跨 1–5 的字符串 + 调用点）** | 见 §6 | **≈ 600–700**（估） | 是（改走 LogManager 或删） | 600–700 | **是（可观测性）** | **High** |
| | **合计** | | **5,887** | | **1,420–2,100** | | |

### 4.3 关键判据

- **代码密度**：5,887 B / 695 行源 = **8.5 B/行** —— 嵌入式 C++ 的紧致区间（典型 10–20 B/行）⇒ **不存在“实现臃肿”型浪费**；`6775bb3` 已做过一轮收敛。
- **`.eh_frame` 360 B + `.gcc_except_table` 81 B = 441 B 是纯异常机制开销**，本 TU 不使用异常语义（见 §7）。
- **`homie_projection.h` 无跨 TU 重复**：全仓 include 审计显示仅 `homie_bridge.cpp` 使用（`main.cpp` 只 include `homie_bridge.h`）⇒ **“header-only inline duplication” 在本工程不存在**，该项**可省 = 0**。

---

## 5. 热点 B —— Device Model（2,844 B：text 1,298 + rodata 1,546）

### 5.1 构成

| 类别 | 项 | 字节 |
|---|---|---:|
| rodata 表 | `kProperties` 896（16 × **56 B**）· `kNodes` 48 · `kEnumWifiSignal` 40 · `kCmdsValve` 24 · `kCmdsScan` 24 · `kEnumValve` 16 · `kCmdsZero` 12 · `kCmdsComputerReset` 12 | **1,072** |
| rodata 字符串 | 节点 id / 属性 id / 中文展示名 / 单位 / format / action_id / payload（**.rodata.str1.1 = 686 B**，70 条） | ~474（保留后） |
| 代码 | `dm_validate_core` 349 · `dm_parse_float_strict` 273 · `dm_range_check` 182 · `dm_parse_num_segment` 104 · `dm_resolve_command` 91 · `dm_find` 77 · `cstr_eq` 55 · `dm_validate_set` 32 · `dm_property_at` 30 · `dm_node_at` 26 · `cstr_len` 25 | **1,258** |
| `.literal` | 地址常量池 | 40 |

### 5.2 representation 是否必须如此？（逐问回答）

| 问题 | 结论 | 可省 |
|---|---|---:|
| `DeviceModelProperty` 是否存在 padding？ | **是**。实测 `896 / 16 = 56 B`；按字段重排可达 **44 B**（消除 12 B/项 padding） | **192** |
| 但与冻结关系？ | `device_model.h:83` 明确写着「**字段顺序即冻结契约**」⇒ **重排字段 = 触犯冻结措辞**，需人工裁决（属 §9 裁决项） | — |
| enum/字符串指针可否更紧凑？ | `node`/`id`/`name`/`format`/`unit`/`enum_state_map`/`cmds` 共 7 个指针（28 B）是**public 语义契约**的一部分（Homie 直接读 `p->node` / `p->id`），不可改类型 | ~0 |
| 重复字符串可否共享？ | 编译器已在 TU 内合并相同字面量；跨 TU 合并只对 `"0"…"4"`/`""` 等极短串有效 | 50–100 |
| node/property metadata 是否重复存储？ | 未发现重复；`kNodes` 仅 48 B | ~0 |
| command mapping 可否压成更紧凑 constexpr？ | 已是最小 `{const char*, const char*, const char*}`（12 B/条）；无推导逻辑 | ~0 |
| dtype/flags 可否打包？ | 可（`settable`+`retained`+`src_kind`+两个 count → 1 个 flags 字），但**改变字段类型 = 改契约** | ~30（需裁决） |
| accessor 是否产生重复 machine code？ | 无重复；4 个 accessor 合计 90 B | ~0 |
| `dm_find` / validation / resolve 可否合并？ | 可小幅合并（`dm_find`+`dm_validate_set`+`dm_resolve_command` 的重复 cstr 比较） | 50–100 |
| 严格浮点/范围解析可否重写？ | `dm_parse_float_strict`+`dm_range_check`+`dm_parse_num_segment` = **559 B**，是三处最重的解析；合并为单遍扫描可省 —— 但必须保持严格语义（拒绝非法输入、拒绝越界） | 150–250 |
| header-inline duplication | 无（`device_model.h` 仅被 `device_model.cpp`/`homie_bridge.cpp` 各取一次声明） | 0 |

**量化档位（回答任务 §三 的 100 / 300 / 500 / 1 KB 问）**

| 档位 | 是否可达 | 路径 |
|---|---|---|
| **100 B** | ✅ 可达 | 解析路径小合并 + 字符串共享（**不改契约**） |
| **300 B** | ⚠️ 需裁决 | 100 B 路径 + 校验器重写 |
| **500 B** | ⚠️ 需裁决 | 300 B + struct 重排去 padding（**触犯「字段顺序即冻结契约」**） |
| **1 KB+** | ❌ 不可达 | 唯一途径是删属性/删校验/改契约 —— 属功能缩减，非优化 |

**结论**：Device Model **实际上被其自身契约锁死**，**不改冻结措辞**时现实可省 **≈ 150–350 B**；**允许 representation 修订**时上限 **≈ 400–550 B**。

---

## 6. 热点 C —— Devices 之外的 CloudManager P2-2 加性（1,622 B）

### 6.1 逐项（新增符号尺寸，ELF）

| 项 | 类别 | 字节 | 必需性 | 可压缩 |
|---|---|---:|---|---|
| `mqtt_event_handler` 增大 | 事件回调：topic 捕获 + business/foreign 分类 | **+315** | **必需**（P2-2 冻结：分片 topic 捕获、路由） | 有限 |
| `cloud_transport_subscribe` | Facade 实现（注册表 add） | **210** | 必需 | 中（逻辑可更薄） |
| `esp_mqtt_client_publish` | 新引入的 IDF 包装 | **269** | 必需（新发布路径） | 否 |
| `cloud_mqtt_publish_binary` / `_text` | retain/store 参数化 | **+143 / +119** | 必需（P2-2 retain 支持） | 有限 |
| `cloud_init` / `cloud_task` / `cloud_connect` 增大 | foreign 队列出队 / replay / LWT 写入 | **+80 / +76 / +52** | **必需**（loop 上下文交付、重连 replay、LWT） | 有限 |
| `cloud_transport_set_lwt` / `_publish` / `set_foreign_handler` | Facade 转发 | **62 / 49 / 10** | 必需（P2-2 API 冻结） | 中（可内联） |
| `mqtt_topic_matches_filter`（`mqtt_routing.h` 内联） | topic 匹配 | **158** | 必需 | 否 |
| `cloud_process_rx_message` | 重构（新旧互抵） | **+8** | — | — |
| `cloud_send_log` 缩小 | — | **−108** | — | — |
| `.literal` / rodata | 新字面量 / 表 | **+76 / +313** | — | 中 |
| **净增量（段级）** | | **+1,224 t / +398 r** | | **可省 ≈ 150–300** |

### 6.2 任务 §四 的五问

1. **哪些是 P2-2 必需功能？** foreign registry / foreign queue / assembler / LWT / routing / retain / reconnect replay —— **全部**是 P2-2 已验收的 frozen behavior（且被宿主测试 45/45 + 74/74 覆盖）。
2. **哪些是为未来 P2-5/P2-7 预留但当前不必要？** **未发现**。`set_lwt` 当前默认 inert（`s_lwt_topic` 空则不写 cfg），但它属 P2-2 冻结 API，且**未产生额外 code**（仅 128 B **bss**）。
3. **哪些可减实现体积但保持 facade contract？** 仅「薄转发内联化」与 `cloud_transport_subscribe` 内部瘦身，**≈ 150–300 B**。
4. **是否存在重复 buffer / duplicated logic？** **否**。`s_rx_asm` 复用既有 `mqtt_large_rx_buffer`；`s_foreign_q` 776 B 与 `s_foreign_subs` 404 B 均为 **bss（DRAM）**，**不占 Flash**。
5. **能否让未启用 Homie 时继续完全 GC？** **不能** —— 实测 Homie 关闭构型相对 base 仍 **+1,272 t / +444 r / +1,560 bss**（`p27off` 数据见 §3.3）：Facade 的订阅/路由/LWT 由 `cloud_connect()`/`cloud_init()` 无条件调用，`--gc-sections` **无法剔除**。
6. **`#if GF_HOMIE_ENABLED` 能否合法包住？** 语法上可以，但 **对预算零收益** —— 预算度量的是 **Homie-启用构型**，被门控掉的代码在该构型里依然存在。⇒ **本项不计入可省。**

---

## 7. 编译器 / 链接器级实验（**均在 base 与 Homie 两侧用同一 flag 重建**）

| flag set | base text | base rodata | Homie text | Homie rodata | Δtext | Δrodata | **Δ合计** | 结论 |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| 默认 | 1,021,171 | 260,380 | 1,030,435 | 263,664 | 9,264 | 3,284 | **12,548** | 基准 |
| `-fno-stack-protector` | 1,021,171 | 260,380 | 1,030,435 | 263,664 | 9,264 | 3,284 | **12,548** | **无效**（两侧 `firmware.bin` **md5 相同**；`__stack_chk_fail` 引用仍存在） |
| `-fno-exceptions` | 998,739 | 210,152 | 1,007,839 | 212,776 | 9,100 | 2,624 | **11,724** | **有效：−824 B**（同时全镜像 **−73.5 KB**） |
| `-flto` | — | — | — | — | — | — | **构建失败** | `ld: undefined reference to app_main`（link 阶段，两次复现） |

**已启用（无需再争取）**：`-Os` · `-ffunction-sections` · `-fdata-sections` · `--gc-sections` · `-fno-rtti` · `-fno-jump-tables` · `-fno-tree-switch-conversion` · `-freorder-blocks`。
**未启用**：LTO（不可用）· 无 `--relax` 生效（`.xt.prop`/`.xt.lit` 全被丢弃）。

### 7.1 为什么「全局压缩」在**增量口径**下最多只能省 ~1 KB ★

设变换 T 使 base 缩减 `f_b`、Homie 缩减 `f_n`，且两镜像总量近似相等（1.021 / 1.030 MB）：

```
Δ' = Δ − (f_n · N − f_b · B)        且  N ≈ B  ⇒  Δ' ≈ Δ · (1 − f)
```

⇒ **要把 Δ 从 12,548 压到 8,704，需要全镜像整体缩小 30.6%**。而本轮实测的现实量级是：
`-fno-exceptions` 全镜像 −5.7%（text −2.2% / rodata −19.3%），在 **Δ 口径**上只换来 **−824 B（−6.6%）**。
LTO / relax 类通常 1–5%，在 Δ 口径上对应 **125–630 B**，且 LTO 在本工程**不可用**。

> ⇒ **不存在任何“全局编译/链接开关”能单独补上 3,844 B。** 这是算术性结论，不是估计。

### 7.2 `-fno-exceptions` 的代价（必须显式记录）

- 全镜像 `-fno-exceptions` 是**可靠性/语义 trade-off**：`new` 失败由 `std::bad_alloc` 变为 `abort`；任何依赖 unwinding 的清理路径失效；ESP-IDF/Arduino 默认**开启**异常（`-fexceptions`）。
- 本工程现状：`.eh_frame` + `.gcc_except_table` 全镜像约 **73.5 KB**。
- ⇒ **属 §9 人工裁决项，不得作为默认推荐。**

### 7.3 `-fno-stack-protector` 的结论

- 经 `PLATFORMIO_BUILD_FLAGS` 传入**实测无效**（两侧二进制 md5 完全一致；框架的 `-fstack-protector` 在本集成方式下优先生效）。
- 单 TU 快编显示：若真能关闭，`homie_bridge.cpp` 可省 **≈ 450 B 代码**（新代码 buffer 密集 ⇒ canary 密度高）。
- ⇒ 需改 `platformio.ini` 才能生效（**本阶段禁止**），且属**安全边界削减**，**不计入可用方案**。

---

## 8. 字符串 / 诊断成本（任务 §六）

### 8.1 归因

| 来源 | 字符字面量 | 说明 |
|---|---:|---|
| `homie_bridge.cpp.o` `.rodata.str1.1` 各族 | **787**（+80 `kGates` = 867） | **全部由 Homie 代码引入** |
| `device_model.cpp.o` `.rodata.str1.1` | **686** | 静态语义表必需（节点/属性/单位/format/action_id） |
| `device_model.cpp.o` 数据表 | **1,072** | `kProperties` 896 等 |
| `cloud_manager.cpp.o` rodata 增量 | **+313** | P2-2 新增字面量 |
| 库 / Arduino / ESP-IDF 间接引入 | — | **不在 src 归因内**（§3.1 残差仅 212 B） |

### 8.2 Homie 诊断明细（**估**）

- 日志相关字符串：`homie_on_foreign` 197 · `homie_task` 138 · `homie_init` 129 · `homie_log_outcome` 43 · `homie_log` 10 · `homie_publish_values` 11 ≈ **528 B**（其中含少量非日志常量，故估计净日志文本 **≈ 450 B**）。
- 代码侧：`homie_log` 18 + `homie_log_outcome` 54 + 21 个调用点（每条 ≈ 8 B）≈ **240 B**。
- **合计 ≈ 600–700 B**。

### 8.3 可共享 / 可替代项（**只提方案，不实施**）

| 方案 | 量级 | 备注 |
|---|---|---|
| 日志改走 `LogManager` event（复用既有字符串池与格式化路径） | 600–700 | 需 **LogManager EventId 冻结流程**（`LOG_HOMIE_DESC_STALE` 等尚无编号，P2-3 遗留 OPEN） |
| 把 `"set"` / `"true"` / `"false"` / `"open"` / `"close"` 等跨模块共享 | 80–150 | `open`/`close`/`0`…`4` 已在 DM 与 Homie 各存一份 |
| 直接删除 Homie 诊断 | 600–700 | **禁止**（可观测性回退；本轮不做） |

---

## 9. 优化机会矩阵（汇总）

| 热点 | 当前 Δ 字节 | 可优化估计 | 优化方式 | 行为变化 | 架构变化 | 风险 |
|---|---:|---:|---|---|---|---|
| **Homie Bridge** — 格式化/JSON 特化 | ~1,286 | **250–400** | 去掉泛型式安全网，按本工程取值域特化（输出须逐字节一致） | 无 | 无 | Med |
| **Homie Bridge** — description 发射器 | ~1,010 | **200–300** | 表驱动发射，去除重复键/格式串 | 无 | 无 | Med |
| **Homie Bridge** — `/set` 解析 | ~1,141 | **100–200** | 单遍解析、守卫前置、id 校验复用 | 无 | 无 | Med |
| **Homie Bridge** — 门控表并入 DM 元数据 | ~326 | **150–200** | 把 `kGates` 语义并入 `DeviceModelProperty` flags | 无 | 轻微（跨模块元数据） | Med |
| **Homie Bridge** — 发布/相位机 | ~2,002 | **120–240** | 小重构 | 无 | 无 | Low |
| **Homie Bridge** — 杂项 | ~202 | **≤ 60** | — | 无 | 无 | Low |
| **Homie Bridge** — 诊断 | ~600–700 | **600–700** | 改走 LogManager / 删除 | **是（可观测性）** | 无 | **High** |
| **Device Model** — 解析/校验合并 | 1,258 | **150–250** | 单遍扫描，保持严格语义 | 无 | 无 | Med |
| **Device Model** — struct 重排去 padding | 896 | **192** | 56 → 44 B/项 | 无 | **触犯「字段顺序即冻结契约」** | **需裁决** |
| **Device Model** — 字符串共享 | ~474 | **50–100** | 跨 TU 共享短串 | 无 | 无 | Low |
| **CloudManager P2-2** — 转发/订阅瘦身 | 1,224 t / 398 r | **150–300** | 内联薄转发、合并守卫 | 无 | 无 | Low |
| **`.rodata` / 字符串**（跨模块共享） | 2,750 | **80–150** | 共享常量 | 无 | 无 | Low |
| **LTO** | — | **不可测量** | 构建失败（`undefined reference to app_main`） | — | — | — |
| **`-fno-stack-protector`** | — | **≈ 450（不可达）** | 需改 `platformio.ini` | 是（安全） | 无 | **High** |
| **`-fno-exceptions`**（全工程，**已实测**） | — | **824** | 编译选项 | **是（异常/可靠性语义）** | 无 | **High** |
| **功能面缩减**（删 pending 结果分类 / fingerprint 哨兵 / 门控表 / 状态缓存） | — | **1,000–1,500** | 删除已验收功能 | **是** | **是** | **不可接受（需人工授权）** |

### 9.1 三档合计

| 档位 | 组成 | 保守 | 乐观 |
|---|---|---:|---:|
| **低风险**（无行为变化、无架构变化、无需裁决） | DM 字符串共享 50–100 + DM 解析/校验合并 150–250 + P2-2 瘦身 150–300 + 跨模块字符串共享 80–150 + Homie 发布/相位机 120–240 + Homie 杂项 ≤60 | **550** | **1,100** |
| **中风险**（无行为变化，但发射器/解析重写） | 格式化特化 250–400 + description 表驱动 200–300 + `/set` 解析 100–200 + 门控表并入 DM 150–200 | **700** | **1,100** |
| **低 + 中** | 以上之和 | **1,250** | **2,200** |
| **需裁决项** | Device Model struct 重排去 padding（须先修订「字段顺序即冻结契约」措辞） | **192** | **192** |
| **高风险 / trade-off** | 诊断改 LogManager（600–700）+ `-fno-exceptions`（**824，实测**） | **1,424** | **1,524** |
| **全方案合计（不含需裁决项）** | 低+中+高 | **2,674** | **3,724** |
| **全方案合计（含需裁决项）** | 上 + 192 | **2,866** | **3,916** |
| **需求** | — | — | **3,844** |
| **残差** | — | **≈ 1,000–1,170 缺口** | **不含裁决项：−120（仍差）· 含裁决项：+72（仅 1.9% margin）** |

> ⚠️ 上表各档存在**重叠风险**（格式化特化与 description 表驱动共享同一批字符串与 `.literal` 常量池；门控表并入 DM 与 DM 元数据修订同源），故「全方案合计」应视为**上界**，而非可加总期待值。
> ⚠️ **即使接受全部 trade-off + 一项冻结措辞修订，乐观上界 3,916 B 也只比 8,704 阈值高约 72 B（1.9%）** —— 落在估计误差带内；实际执行极可能仍需**再叠加少量功能面缩减**才能稳妥越过。

---

## 10. 任务 §九 三选一裁决

### ❌ `A. CLEAR PATH` —— 不成立
保守估计仅 **≈ 2,674 B**（且已含两项 high trade-off），远低于 3,844 B；**「零行为变化」路径的上限更低，仅 ≈ 1,250–2,200 B**。

### ✅ `B. POSSIBLE WITH TRADE-OFF` —— **本轮结论**
- **无需任何 trade-off 的路径**：**不存在**（低+中风险上限 ≈ 2,200 B，缺口 ≥ 1,644 B）。
- 跨过 8,704 B **必须**同时接受下列**全部**：
  1. `-fno-exceptions`（全工程异常/可靠性语义变化，**实测 −824 B**）；
  2. Homie 诊断改走 LogManager 或删除（**可观测性削减，−600~700 B**）—— 依赖 LogManager EventId 冻结流程；
  3. 多处中等风险重写（格式化特化 / description 表驱动 / `/set` 解析收敛 / 门控表并入 DM，−700~1,100 B）；
  4. 低风险项全量执行（−550~1,100 B）。
- 以上全部接受的乐观上界 **= 3,724 B < 3,844 B（仍差 120 B）**；
  **额外**再修订「字段顺序即冻结契约」措辞以做 Device Model struct 重排（+192 B）后才达到 **3,916 B**，仅比阈值高 **72 B（1.9%）**。
- ⇒ 本路径**不是效率提升，而是「安全性 + 可观测性 + 冻结措辞 + 高密度重写」四重代价换取一个 2% 的 margin**。

### ⚠️ 为什么不直接判 `C. NO REASONABLE PATH`？
C 的定义要求「系统量化后**仍无法**获得 ≥3,844」。本轮乐观上界（3,916，含裁决项）**略高于**阈值 72 B，**不足以断言“不可达”**；但保守侧差 1,000 B 以上、且**完全不含 trade-off 的路径不存在**，也**不足以断言“有清晰路径”**。⇒ 恰属 B：**有路径，但需人工接受明确的 trade-off 清单，且 margin 极小**。

---

## 11. 推荐下一步（**不实施，仅建议；本轮不改代码**）

按「先无争议、后需裁决」排列：

1. **（无需裁决，建议纳入下一轮优化任务）**
   - DM 字符串共享 + 解析/校验单遍合并（−200~350）
   - CloudManager P2-2 薄转发内联 + 订阅路径瘦身（−150~300）
   - Homie `/set` 解析收敛、门控表并入 DM、发布/相位机小重构（−200~400）
   - 跨模块字符串常量共享（−80~150）
   → 预期 **≈ 600–1,150 B**，**行为与架构均不变，有完整宿主测试可守**。
2. **（需人工裁决 A）** Device Model 内部 representation（去 padding / flags 打包）：
   **需先修订 `device_model.h:83` 的「字段顺序即冻结契约」措辞**，收益上限 **≈ 220 B**。
3. **（需人工裁决 B）** 诊断路径：Homie 日志改走 `LogManager`（**前置依赖 LogManager EventId 冻结流程**），收益 **≈ 600–700 B**，代价 = 串口直出的可观测性。
4. **（需人工裁决 C）** 全工程 `-fno-exceptions`：**实测 −824 B**，代价 = 异常/可靠性语义变化；**不建议作为默认**。
5. **（需人工裁决 D）** 冻结预算本身：若上述 1–4 全部采纳后仍差约 0–1 KB，则需在
   - ① 修订 §17.2 上限（并给出实测依据），或
   - ② 授予 waiver，或
   - ③ 授权功能面缩减（删 pending 结果分类 / fingerprint 哨兵 / 门控表 / 状态缓存之一）
   三者中择一。
6. **不推荐**：LTO（不可用）· `-fno-stack-protector`（不可达 + 安全边界削减）。

---

## 12. 证据清单（scratch，`.pio/` gitignored，未入库）

| 路径 | 内容 |
|---|---|
| `.pio/build/p26base`（worktree `/d/Guo_Feeder_Project/.p26base`） | base `623da59` 构建（.elf/.map/.bin） |
| `.pio/build/p27{off,on,prod}` | 当前 HEAD 三构型 |
| `.pio/build/p27{noexc,nsp,lto,lto2,lto3}` · `.pio/build/p26{noexc,nsp,lto}` | 编译/链接选项实验（含双侧重建） |
| `.pio/p26res/symdiff.py` · `mapattr2.py` · `secbreak.py` · `symattr.py` | 归因工具 |
| `.pio/p26res/lto_full.txt` | LTO 失败完整日志（`undefined reference to app_main`） |
| `.pio/p26res/*.str.bin` | `device_model` / `homie_bridge` 字符串池转储 |
| `.pio/p26res/flagbuild*.sh` · `localcc.sh` | 构建/vs 快编脚本 |

---

## 13. 本阶段纪律确认

- 未修改任何 firmware / test / Cloudflare / EMQX / platformio.ini 代码；
- 未修改 P2-0 冻结文档、未修改 §17.2 预算、未 waiver、未修改任何测试判据；
- 未删除任何功能、未删除任何诊断；
- 未进入 P2-7 / P2-8；未处理 P2-4-OPEN-1；未 push / amend / squash / rebase / reset；
- working tree **clean**；唯一变更 = 本文件（documentation-only commit）。
