# WF-4 范围审查 —— Registry 日志责任边界界定

> **状态**：📋 **审查完成，待用户确认**（**未修改任何代码**）
> **日期**：2026-09-20
> **触发**：Phase 3 第 ④ 项 Registry 埋点接入前置问题 `WF-4`
> **WF-4 原文**：`矩阵 §7 作用域含 capability_registry.cpp（Registry 段）与 workflow_storage.cpp ⇒ 范围界定不清`
> **方法要求**：**不按文档模块名判断，按代码实际调用链判断**
> **冻结目标 EventId**：`LOG_REG_REBUILT`(0x0B01, INFO) / `LOG_REG_SAVE_FAILED`(0x0B02, ERROR)

---

## 一、结论速览（TL;DR）

| 问题 | 结论 |
|---|---|
| **WF-4 是否成立？** | ❌ **不成立（可关闭）** —— `workflow_storage` 与 Registry **零耦合**，只是矩阵把它列在同一行 |
| Registry 是否拥有完整生命周期？ | ✅ **是** —— 初始化 / 重扫 / 读 Flash / 算 CRC / 建表 / 落盘 / 查询，**全部自持** |
| workflow_storage 是否只是消费者？ | ❌ **连消费者都不是** —— 它 **完全不引用 Registry**，仅有一条**注释**描述下游后果 |
| workflow_storage 是否应产生 Registry 类日志？ | ❌ **绝对不应该** —— 无调用关系，硬加即**架构污染** |
| 是否存在多模块记录同一事实？ | ⚠️ **存在 2 个真实风险点**（见 §五），但**均可通过"只埋 Registry、不埋调用方"规避** |
| 改动量 | ✅ **只需增加 `log_emit` 调用**（+2 处），**无需新增 callback、无需改接口、不破坏分层** |

---

## 二、Registry 模块实际代码位置与调用链（按代码事实）

### 2.1 涉及的文件

| 文件 | 行数 | 与 Registry 的实际关系 |
|---|---|---|
| `src/capability_registry.cpp` | 1007 | ★ **Registry 本体**（全部生命周期在此） |
| `src/capability_registry.h` | 240 | 公开接口声明 |
| `src/workflow_storage.cpp` | 1583 | ⛔ **零引用**（`#include` 不含 `capability_registry.h`） |
| `src/workflow_storage.h` | ~300 | ⛔ **零引用**（仅 `:268` 一行**注释**提及 "Capability Registry checksum 变化"） |
| `src/command_manager.cpp` | 3204 | 调用方（读 + 触发 rescan） |
| `src/cloud_manager.cpp` | — | 调用方（读） |
| `src/main.cpp` | — | 调用方（init，1 处） |

**证据（`workflow_storage.cpp` 的 include 全清单）**：

```c
1: #include "workflow_storage.h"
2: #include "workflow.h"   // 仅用于常量一致性 static_assert，不调用其任何函数
4: #include <string.h>
5: #include <stdio.h>
6: #include <time.h>
7: #include <esp_heap_caps.h>
```

⇒ **`workflow_storage.cpp` 不 include `capability_registry.h`，也不调用其任何函数。**

**证据（全仓库对 Registry 的引用，排除自身）**：仅有 `cloud_manager.cpp` / `command_manager.cpp` / `main.cpp` 三处，**`workflow_storage` 一次都没出现**。

### 2.2 完整调用链（代码实测）

```
【初始化路径】
  main.cpp:541  capability_registry_init()          ← setup 第七层
                    │
                    ▼
  capability_registry_init()                        ← :723
     ├─ table_reset(×3)
     └─ capability_registry_rescan()                ← :741
            └─ registry_sync(CAP_ACTION)            ← :526 ★核心
            └─ registry_sync(CAP_TRIGGER)
            └─ registry_sync(CAP_WORKFLOW)

【运行期路径（Workflow 变更后）】
  command_manager.cpp:1823  workflow create 成功  ─┐
  command_manager.cpp:1932  workflow set 成功     ─┤
  command_manager.cpp:2020  workflow delete 成功  ─┼→ capability_registry_rescan()
  command_manager.cpp:2076  workflow save 成功    ─┘        │
                                                             ▼
                                                   registry_sync(×3)

【读取路径（只读，不改 Registry 状态）】
  command_manager.cpp  → capability_get_*(...)      ← 大量查询
  cloud_manager.cpp    → capability_get_*(...)      ← 2 处
  command_manager.cpp  → capability_export_*(...)   ← 导出结构化文本
```

### 2.3 `registry_sync()` 内部：**唯一的"重建/保存"发生地**

`registry_sync(CapabilityType type)` (`:526-715`) 是本问题**唯一**的核心函数。它每轮为**一种类型**执行：

| 步骤 | 行 | 动作 | 结果分叉 |
|---|---|---|---|
| 1 | :547-573 | 从 **PSRAM 优先**分配 3 块工作区（**不能放栈**，命令可能跑在 mqtt 任务上；**不能用 static**，mqtt 与 loop 任务可能并发） | 分配失败 ⇒ `sync oom` + `return false` |
| 2 | :576-582 | placement new 就地构造（`String` 成员） | — |
| 3 | :589/595/601 | `scan_*_ids()` 扫描当前注册表 | — |
| 4 | :616-636 | 排序 → 分配 stable_id → 算 checksum | — |
| 5 | :639-643 | `load_registry_file()` 读 Flash | — |
| 6a | **:645-667** | **checksum 一致** ⇒ 复用 Flash 的 version，**不写盘** | 打 `reuse`（**正常路径**） |
| 6b | **:668-702** | **checksum 不一致 / 文件不存在 / 损坏** ⇒ `version = old+1`（无旧版则 1），**调用 `save_registry_file()` 落盘** | 打 `rebuild`；保存失败打 `flash save failed, RAM cache kept` |
| 7 | :704-712 | 析构 + 释放 | — |

**⇒ Registry 的"建表"与"落盘"两个关键事件，都只发生在 `registry_sync()` 的 6b 分支内。**

---

## 三、逐事件归属判定（★ 用户点名要求的 6 项）

> 判定原则：**按"谁执行了这个动作"归**，而不是按"谁的名字里有 Registry"。

| # | 用户列出的"事件" | 代码实际状况 | **归属** | 判定依据 |
|---|---|---|---|---|
| 1 | **Registry 构建完成** | = `registry_sync()` 6b 分支完成建表（可能已落盘，也可能落盘失败但 RAM 可用） | **`capability_registry.cpp` / `registry_sync()`** | 只有此处真正"构建"了 Mapping 表 |
| 2 | **`REG_REBUILT`** | 冻结于 `log_events.h:329`（INFO） | **`registry_sync()` 的 6b 分支（else 块）** | 与 #1 是同一事实，即**该 EventId 的宿主就是 else 分支** |
| 3 | **Registry 保存失败** | `save_registry_file()` 有 **4 个 `return false` 出口**（:165 tmp 文件 open 失败 / :200 写数据失败 / :223 backup rename 失败 / :242 tmp rename 失败；另 :177 数据非法时置 `ok=false` 由 :195 统一返回）+ 1 个 `return true`（:251） | **`registry_sync()` 6b 内对 `save_registry_file()` 返回值的判定处** | ★ **不应埋进 `save_registry_file()` 内部**——内部 4 个出口若各埋一条会**同一事实多条记录**（见 §五-①） |
| 4 | **`REG_SAVE_FAILED`** | 冻结于 `log_events.h:330`（ERROR） | **同上**——`registry_sync()` 里 `if(!save_registry_file(...))` 的**单个**失败上报点 | 一处上报 ≠ 丢失信息：`LOG_P_ERR_CODE` 可带粗粒度原因 |
| 5 | **Workflow 读取 Registry** | `command_manager.cpp` 大量 `capability_get_workflow_*()`；工作流**本体**（`workflow.cpp`）**完全不调用** Registry | ⛔ **不埋** —— 这是**高频只读查询**（每次 `workflow.list` / `registry` 命令都会触发） | 埋了必成风暴；且"读取成功"是**正常高频行为**，无观测价值 |
| 6 | **Workflow Storage 保存 Registry 相关数据** | ⛔ **该事实不存在** —— `workflow_storage` 保存的是 **Workflow 本体**（`/workflow/*.bin`），Registry 数据由 `capability_registry` 自己存到 **`/registry/*.bin`**。**两套完全独立的文件与代码路径** | ⛔ **不埋（无此事件）** | 见 §四 核心结论 |

**补充（用户未列但代码里真实存在的"事件"）**：

| 事件 | 归属 | 说明 |
|---|---|---|
| `Registry 复用（reuse）` | `registry_sync()` 6a 分支 | 正常路径。**建议不埋 INFO**（否则每次 boot 3 条 + 每次 workflow 变更 3 条，见 §六 频率分析） |
| `sync oom`（:564） | `registry_sync()` 分配失败 | 有观测价值（PSRAM 耗尽），但**无对应冻结 EventId** ⇒ 本项目**守"不新增 EventId"** ⇒ **不埋、登记为缺口** |
| `stable_id 分配变化` | `registry_sync()` :632-634（`stable_id = i`，由排序决定） | ★ 这**不是独立事件**，而是 checksum 变化的**结果**。已在 `LOG_REG_REBUILT` 的语义内（`count`/`checksum` 参数可体现） ⇒ **不单独埋** |

---

## 四、核心结论：WF-4 不成立

### 4.1 「Registry 是否拥有完整生命周期？」 —— ✅ **是，完整自持**

| 生命周期阶段 | 实现位置（均在 `capability_registry.cpp`） |
|---|---|
| 初始化 | `capability_registry_init()` :723 |
| 重扫触发入口 | `capability_registry_rescan()` :741 |
| 扫描能力列表 | `scan_action_ids()` :349 / `scan_trigger_ids()` :388 / `scan_workflow_ids()` :426 |
| 排序与 ID 分配 | `sort_capability_mapping()` :468 |
| 校验和计算 | `registry_checksum()` :49 |
| Flash 读 | `load_registry_file()` :260 |
| Flash 写（含原子替换） | `save_registry_file()` :145 |
| 内存管理 | `registry_buf_alloc()` :506 |
| 对外查询 | `capability_get_*()` :790-959 |
| 导出 | `export_table()` :961 / `capability_export_*()` :994-1007 |

⇒ **从头到尾无一阶段依赖 `workflow_storage`。** Registry 的数据来源是 **workflow / action / trigger 的注册表（RAM）**，落盘目标是 **`/registry/*.bin`**。

### 4.2 「workflow_storage 是否只是消费者？」 —— ❌ **连消费者都不是**

- `workflow_storage` **不 include** `capability_registry.h`
- `workflow_storage` **不调用**任何 `capability_*` 函数
- 全文件唯一提及 Registry 的地方是 `workflow_storage.h:268` 的**注释**：

  ```c
  //   内容变化 → variant++ → Capability Registry checksum 变化
  ```

  这是**描述"改了 workflow 内容会导致 Registry 的 checksum 变化"这一下游后果**，
  属于**跨模块影响的文字说明**，**不是调用关系**。

- 反向也成立：`capability_registry.cpp` 只 include `workflow.h`（读注册表），**不 include `workflow_storage.h`**。

⇒ 二者是**平行关系**，通过 **Workflow 的 RAM 注册表 + `variant` 字段**间接联系，**无直接耦合**。

### 4.3 「workflow_storage 是否应该产生 Registry 类日志？」 —— ❌ **不应该**

理由（三条，按强度排序）：

1. **无调用关系** —— 它不执行任何 Registry 动作，无权代表 Registry 记账。硬加会**凭空发明一个不存在的事件源**。
2. **会造成同一事实双份记录** —— `workflow_storage` 保存成功 → `variant++` → 触发 `capability_registry_rescan()` → Registry 重建。若两边都埋，**同一次内容变更会产生 2+ 条记录**（正 §五-② 要避免的）。
3. **违反"一个业务事件只由一个最贴近的模块记账"** —— 该原则已在 P2-J（Command 排除异步）确立。Registry 的重建事实**最贴近 `registry_sync()`**。

### 4.4 「是否存在多个模块同时记录同一事实？」 —— ⚠️ **有 2 个真实风险点，但可规避**

**风险点 ①：`save_registry_file()` 内部有 4 个失败出口**
若把 `REG_SAVE_FAILED` 埋进 `save_registry_file()` 内部每个 `return false` 前
⇒ 一次失败可能产生多条（当前 4 个出口是互斥的，各报 1 条尚可，但语义上"一次保存失败"被拆成 4 种事实）
⇒ **正解：只在调用方（`registry_sync()`）判定返回值处埋 1 条。**

**风险点 ②：`command_manager.cpp` 的 4 个 `rescan()` 调用点**
`create`(:1823) / `set`(:1932) / `delete`(:2020) / `save`(:2076) 各调一次 `rescan()`。
若在这些**调用点**再埋一条 `REG_REBUILT`
⇒ 与 `registry_sync()` 内部埋的**重复**（一次 create 命令 → 3 次 `registry_sync` → 若触发重建则最多 3 条）
⇒ **正解：绝不在 `command_manager.cpp` 埋 Registry 事件。** 调用方已有自己的 `LOG_CMD_*` 段（P2-J 已完成）。

---

## 五、A. 推荐宿主位置

### A-1 `LOG_REG_REBUILT` (0x0B01, INFO)

**位置**：`src/capability_registry.cpp` → `registry_sync()` → **`else` 块（:668-702）内**，与既有 `Serial.printf("[CapRegistry] %s rebuild version=...")` **并列**。

**推荐插入点**：紧跟 `table->entries` 填充完成、`save_registry_file()` 调用**之前或之后**（两者都可，因为 rebuild 事实已成立；**建议放在 `save_registry_file()` 之前**，这样"重建发生"与"保存成败"在日志顺序上符合因果）。

**原因**：
1. **该分支即"重建"的结构性定义** —— 只有 checksum 不一致 / 文件缺失 / 损坏才会进入，进入即代表重建。
2. **天然边沿** —— `registry_sync()` 一次调用最多进入一次该 `else` 块，**无需去重门控**（符合 P2-K 的"守卫即边沿"论证）。
3. **覆盖全部触发源** —— `init()`（boot）与 `command_manager` 4 个调用点**都**经此路径，**一次埋点全覆盖**。
4. **与既有 Serial 输出同址** —— 便于上板对照，且将来若删串口打印，埋点不会丢。

**参数建议**（复用既有 ParamId）：`LOG_P_REG_TYPE`（区分 ACTION/TRIGGER/WORKFLOW）+ `LOG_P_COUNT` + `LOG_P_VERSION`。

> ⚠️ **INFO 级的频率问题**：见 §六。**建议评估是否需降为"仅首次/变化时"**，但因
> `rebuild` 本身**只在内容真变时发生**（实测 91 轮 sync 仅 3 次 rebuild），
> **INFO 且不加门控是可接受的**。

### A-2 `LOG_REG_SAVE_FAILED` (0x0B02, ERROR)

**位置**：`src/capability_registry.cpp` → `registry_sync()` → **`if (!save_registry_file(path, magic, *table))` 的失败分支（:687-693）内**，与既有 `Serial.printf("[CapRegistry] %s flash save failed, RAM cache kept")` 并列。

**原因**：
1. **单点上报道** —— `save_registry_file()` 的 4 个内部出口在此**汇聚为 1 个判定**，天然去重。
2. **语义完整** —— 该分支的原文 "flash save failed, **RAM cache kept**" 精确表达了失败**后果**（Registry 仍可用，但重启后会再重建），这正是 ERROR 级要传达的。
3. **天然边沿** —— 仅在 `else`（重建）分支内且仅在其失败时进入；`registry_sync()` 每次调用最多一次。
4. **ERROR 级落 Flash + 上云**，符合该事件"需要运维介入"的定位（长时间持续失败 = Flash 损坏）。

**参数建议**：`LOG_P_REG_TYPE` + `LOG_P_ERR_CODE`（**注意**：`save_registry_file()` 返回 `bool`，
**不携带具体错误码**。若要细分 5 种原因，**必须改接口**（返回 `uint8_t`/枚举）。
⇒ 见 §七-C 的接口决策）。

---

## 六、B. 不应该埋点的位置（易重复记录清单）

| # | 位置 | 为什么不要埋 | 会与什么重复 |
|---|---|---|---|
| **B1** | `command_manager.cpp:1823 / 1932 / 2020 / 2076`（4 个 `capability_registry_rescan()` 调用点） | 调用方**无权代表 Registry 记账**；一次 create/set/delete/save 会触发 **3 次 `registry_sync`**，若触发重建则**最多 3 条** | 与 A-1 在 `registry_sync()` 内埋点**直接重复** |
| **B2** | `save_registry_file()` 内部（:165 / :200 / :223 / :242） | 4 个 `return false` 若各埋一条 ⇒ 语义上是"4 种失败原因"，但**同一次保存失败只应上报 1 条** | 与 A-2 **重复**；且未来若改为"重试"逻辑会放大 |
| **B3** | `load_registry_file()` 内部 | 读失败**不是 Registry 事件**，而是**重建的诱因**，已被 `LOG_REG_REBUILT` 覆盖 | 与 A-1 **语义重叠** |
| **B4** | `workflow_storage.cpp`（任何位置） | ★ **无任何 Registry 调用关系** ⇒ 硬加等于**发明不存在的事件源**，属**架构污染** | 与 A-1/A-2 概念重复 |
| **B5** | `capability_get_*()` 全部查询函数 | **高频只读**（`workflow.list` / `registry` 命令每次都调几十次） | 自重复 ⇒ **日志风暴** |
| **B6** | `capability_export_*()` | 纯数据编码，无状态变化 | 与 B5 同类 |
| **B7** | `registry_sync()` 的 `reuse` 分支（:645-667） | **正常路径且高频**（每次 boot + 每次 workflow 变更各 3 条） | 自重复；**INFO 也不建议** |
| **B8** | `capability_registry_init()` :723 | 只是 `rescan()` 的包装，**无独立语义** | 与 A-1 重复 |

> **判据通则**（本轮沉淀）：**"事件的定义点"与"事件的触发点"往往是两个不同函数。**
> Registry 的触发点在 `command_manager`（4 处），但定义点在 `registry_sync()`。
> **必须埋"定义点"**，否则 4 个触发点会各记一条同一事实。

---

## 七、C. 是否需要修改接口

### 7.1 逐项回答用户提问

| 提问 | 结论 |
|---|---|
| 是否只增加 `log_emit` 调用即可？ | ✅ **基本可以**（2 处 `log_emit`） |
| 是否需要新增 callback？ | ❌ **不需要** —— `capability_registry.cpp` 是**同步模块**，无"回调置标志 → task 消费"的既有机制；埋点直接写在函数内即可（与 Weight / Valve / ComputerReset 同构） |
| 是否会破坏当前分层？ | ❌ **不会** —— 只需 `#include "log_manager.h"`。LogManager 是**基础服务**，各模块向它上报是**既定方向**（已 10 个模块如此） |

### 7.2 唯一的接口权衡（需用户决策）

`LOG_REG_SAVE_FAILED` 若要**区分 4 种失败原因**（open / 写 / backup rename / tmp rename），
则 `save_registry_file()` 的 `bool` 返回值**信息不足**，需改为带错误码。

| 方案 | 改动 | 利弊 |
|---|---|---|
| **方案 1（推荐）** | **不改接口**。`log_emit(LOG_REG_SAVE_FAILED, ERROR, {REG_TYPE, COUNT})`，**不带 ERR_CODE** | ✅ 零接口风险、不破坏分层；✅ 串口已打印具体原因，排障时可看串口；⚠️ 日志本身无法区分 4 种原因 |
| **方案 2** | 把 `save_registry_file()` 返回值改为 `uint8_t`（0=成功，非 0=错误码），并嵌入 `LOG_P_ERR_CODE` | ✅ 信息完整；⚠️ **改了一个既有函数的签名**（本项目**默认不改既有接口**，且用户明确"禁止为埋点新增…"） |
| **方案 3** | 在 `registry_sync()` 内用**已有变量**推断**粗粒度**原因 | ⚠️ 需在 `save_registry_file()` 内**新增 1 个输出参数或模块级标志** ⇒ 仍是接口改动 |

**⇒ 我倾向方案 1**（最小改动、零接口风险）。理由：`LOG_REG_SAVE_FAILED` 的**核心价值**是
"**Registry 落盘失败**"这一事实本身（ERROR 级、需介入），**而非精确区分失败原因**。

**★ 关键证据（本审查实测）**：`save_registry_file()` 的 **4 个失败出口各有 1 条专属串口打印**，
**1:1 严格对应**：

| 串口打印行 | `return false` 行 | 失败原因 |
|---|---|---|
| :161 | :165 | tmp 文件 open 失败 |
| :196 | :200 | 写数据失败 |
| :220 | :223 | backup rename 失败 |
| :231 | :242 | tmp rename 失败 |

⇒ **精确原因 100% 已由串口输出覆盖**（4 条不同文案）⇒ 日志侧只需记"**失败了**"这 1 条即可，
排障时对照串口即可定位原因。**方案 1 零信息损失。**

**若将来确有需求，再单独评审方案 2**（不阻塞本次接入）。

### 7.3 禁止事项核对

- ❌ **不新增 EventId** —— 复用 `0x0B01`/`0x0B02` ✅
- ❌ **不新增 ParamId** —— 复用 `LOG_P_REG_TYPE`(0x4D) / `LOG_P_COUNT`(0x45) / `LOG_P_VERSION`(0x4B) ✅
- ❌ **不改 System State** —— Registry **本就不在** System State 中（已核对，无 OLED/Registry 条目）✅
- ❌ **不新增配置项 / enable 开关** ✅

---

## 八、频率分析（决定"是否需门控"）

| 路径 | 触发频率 | 实测数据 |
|---|---|---|
| `capability_registry_init()` | **每次 boot 1 次** | 每 boot 产生 3 行 `CapRegistry`（3 类型各 1） |
| `capability_registry_rescan()`（workflow 变更） | **每次 create/set/delete/save 成功 1 次** | 同样 3 行 |
| **→ 其中 `rebuild` 分支** | **仅当 checksum 变化** | ★ **91 轮 sync 中仅 3 次 rebuild（≈3.3%）** |
| **→ 其中 `reuse` 分支** | **绝大多数** | 96.7% |
| `capability_get_*()` 查询 | **极高**（每次 `workflow.list` 数十次） | —— |

**结论**：
- **A-1（REBUILT）不加门控可接受** —— 因为它只在**内容真变**时触发（实测 3.3%），
  且"内容变化"本身是**低频人工操作**（云端改 workflow），不是高频路径。
  符合本项目"**高频源禁埋**"原则的反面 —— 它是**低频源**。
- **B5/B7 必须不埋** —— 它们才是高频源（查询 / reuse）。

---

## 九、待用户确认的 4 项决策

| # | 决策点 | 我的建议 |
|---|---|---|
| **D1** | **WF-4 是否关闭？** | ✅ **建议关闭** —— `workflow_storage` 与 Registry **零耦合**，矩阵把二者列同行属**误导**，应更正矩阵 §7 的作用域描述 |
| **D2** | **`REG_REBUILT` 宿主** | ✅ 建议 = `registry_sync()` 的 `else` 块内（A-1） |
| **D3** | **`REG_SAVE_FAILED` 是否带 `ERR_CODE`（是否改接口）** | ✅ 建议 **方案 1**：不改接口，不带 ERR_CODE（信息由既有串口打印覆盖） |
| **D4** | **是否顺带在矩阵/文档中更正 `workflow_storage` 的角色** | ✅ 建议更正（把 `workflow_storage.cpp` 从 Registry 段作用域**移除**，注明它是 Workflow 本体存储，与 Registry 平行） |

---

## 十、审查方法与证据清单

| 结论 | 证据 |
|---|---|
| `workflow_storage` 与 Registry 零耦合 | `grep -rn "capability\|registry" src/workflow_storage.{cpp,h}` ⇒ **仅 1 行注释**（`.h:268`） |
| 依赖方向 | `workflow_storage.cpp` 的 7 个 `#include` 中**无** `capability_registry.h` |
| Registry 唯一重建点 | `registry_sync()` :526-715，`else` 分支 :668-702 含 `save_registry_file()` 调用 |
| 保存失败 4 出口 | `save_registry_file()` 的 `return false` 于 :165 / :200 / :223 / :242（+ 数据非法时置 `ok=false` 由 :195 统一返回），`return true` 于 :251 |
| rescan 调用点 | `capability_registry.cpp:730`（init 内）+ `command_manager.cpp:1823/1932/2020/2076`（4 处） |
| rebuild 低频 | `.pio/p15run/*.log` 统计：91 轮 `reuse` vs 3 次 `rebuild` |
| Registry 不在 System State | `grep -n "OLED\|REGISTRY" src/system_state.h` ⇒ 空 |
| 并发风险（影响埋点安全性） | `registry_sync()` :536-542 注释：命令可能跑在 loopTask **或 esp-mqtt 任务**；故**不能用 static** ⇒ 埋点参数**必须用栈上 `LogParamIn p[n]`**（局部），**不得引入模块级可变状态** |

---

## 十一、诚实标注：本次审查未覆盖的部分

| 项 | 说明 |
|---|---|
| **未做上板实测** | 本阶段仅审查（用户明确"第一步：只分析，不修改代码"）。`rebuild` 的 END-TO-END 触发验证留到实施后 |
| **`command_manager` 的 `LOG_CMD_*` 与 Registry 的重叠** | P2-J 已在 `command_manager` 埋了 `LOG_CMD_APPLIED` 等。**本次确认无语义重叠**（Command 段记"命令被应用"，Registry 段记"映射表被重建"，是**因果关系的两个不同事实**）。但**若用户希望进一步收敛**，可讨论"Registry 变更是否算 Command 的从属事件"——**当前判为独立事实，建议各自记账** |
| **`registry_buf_alloc()` 的 PSRAM 回退** | 与埋点无关，未深查 |
| **矩阵 §7 的其他行** | 本次只审查 Registry 相关行；Workflow 段其余部分未复核 |

---

## 十二、下一步

**等待用户确认 D1–D4 后**，实施 Phase 3 第 ④ 项：
1. `src/capability_registry.cpp`：+include `log_manager.h`；+2 处 `log_emit`
2. 编译验证（V1）→ 上板零回归（V2）→ 定向触发 rebuild（V3）
3. 更新 `docs/LogManager-P2-Progress.md` + `未修复的问题.md`（关闭 WF-4）
4. 独立 commit（`feat(log): integrate capability registry logging`）

**未经确认不修改**：`capability_registry.cpp` / `workflow_storage.cpp` / `log_events.h` / `EventManager`。
