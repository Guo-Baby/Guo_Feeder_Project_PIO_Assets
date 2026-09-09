# WorkflowStorage Review 修正报告（三个存储模块）

日期：2026-09-10
范围：`file_storage.*` / `bin_storage.*` / `workflow_storage.*`（+ main.cpp 测试控制台）
原则：**不重构整体架构，只修正影响 Workflow 持久化正确性的关键问题。**

---

## 1. Review 结论逐条核对

| # | Review 要求 | 现状核对 | 处理 |
|---|---|---|---|
| 1.1 | FileStorage 通用抽象，不含业务语义 | ✅ 已满足 | 无改动 |
| 1.2 | BinStorage 作为 BIN 便利层 | ✅ 已满足 | 无改动 |
| 1.3 | Workflow/WorkflowStorage 不得直接用 LittleFS | ✅ 已满足 | 新增的恢复扫描统一走 `bin_storage_foreach`（新包装），保持严格分层 |
| 1.4 | `file_storage_write(path,0,…)`=截断重写；offset>0 原地 | ✅ 实现正确（`file_storage.cpp:373` offset==0 用 `"w"` 打开截断） | 无改动 |
| 1.5 | 验证 LittleFS.rename 目标存在时的行为 | ⚠️ 无法上板实测 | 已做静态核实：`FS::rename` → `_impl->rename` → VFS → esp_littlefs → `lfs_rename`（POSIX 原子替换语义，目标存在则覆盖）。依赖链见 `~/.platformio/packages/framework-arduinoespressif32/libraries/FS/src/FS.cpp:263`。**仍建议上板做一次 rename 覆盖快速验证**（见 §5 测试项 0） |
| 1.6 | read 短读语义保持 | ✅ 已满足 | 无改动 |
| 1.7 | 不大规模重构 | ✅ | 全部改动为"新增 + 单函数重写" |
| 2.x | `bin_storage_write_atomic()` 只保证单文件原子替换 | ✅ 已满足（`bin_storage.cpp:531` tmp→verify→rename→verify） | 注释语义已收敛，不再被当作多文件事务使用 |
| 3 | **`workflow_storage_save()` 逐个写正式文件不是事务** | ❌ **问题成立**：旧实现逐个 `save_step`（直接覆盖正式 stepNN.bin），S03 写失败时 S00~S02 旧版已被覆盖 | **已重写为暂存式事务（见 §2）** |
| 4 | 真正的事务语义 | ❌ 缺失 | **已实现：暂存 + txn_id + Meta 原子提交 + 发布 + 启动恢复** |
| 5 | Step BIN 格式保留 | ✅ 格式未动 | 无改动（Header magic/version/header_size/payload_size/crc + payload 归属校验） |
| 6 | Meta 字段保留；step_count 语义；不写 runtime | ✅ 已满足 | 无改动（仅新增 `txn_id`） |
| 7 | **CRC 不能用 `crc32 != 0` 判断**；统一 LE 序列化 | ❌ **问题成立**：`workflow_storage.cpp` load() 用 `e.crc32 != 0 &&`；save/load 用 `(const uint8_t*)&step_crc` 直读内存 | **已修正：始终校验 + `crc_append_u32_le()` 显式小端折叠** |
| 8 | `save_step` 语义边界（单 Step ≠ 事务） | ⚠️ 注释未明确 | **头文件已加边界声明**；`workflow_save_transaction()`（workflow.cpp）从未使用 save_step，Dirty 清理仍以整 Workflow 保存事务为界 |
| 9 | 删除只置 valid=false、不删 BIN | ✅ 已满足 | 无改动 |
| 10 | 独立测试（7/8/9/10 最重要） | ⚠️ 无上板条件 | **新增 `wfst` 串口测试控制台 + 故障注入 API**，测试序列见 §4 |

---

## 2. Workflow 级事务机制（重写核心）

### 文件命名

```text
正式文件   /workflow/wfNN/stepMM.bin
暂存文件   /workflow/wfNN/stepMM.bin.t<txn_id 8 位十六进制>
Meta       /workflow/meta.bin   （v2：每 entry + txn_id(4B)）
```

### `workflow_storage_save()` 事务时序（严格）

```text
1. txn_id = Meta.txn_id + 1                 ← 新事务标识
2. 逐个 Step 序列化 → 写入暂存文件 stepMM.bin.t<txn_id>
     写完即读回校验：大小 + CRC（bin_storage_crc32）
     任一失败 → 清本次暂存，不提交，正式文件保持旧版本
3. 【提交点】Meta 原子提交（txn_id 更新为新值，step_count/crc32/info 同步）
     提交失败 → 清本次暂存，正式文件保持旧版本
4. 发布：逐个 rename 暂存 → 正式文件（原子替换）
     任一 rename 失败 → 事务已提交，剩余暂存保留，返回 RENAME_FAILED
5. 清理历史暂存残留（其它 txn_id 的 .t*）
```

### 掉电安全矩阵

| 掉电点 | 磁盘状态 | 恢复行为 |
|---|---|---|
| 2 中间 / 2 与 3 之间（提交前） | 正式文件=旧、Meta=旧、残留 `.t<X>` | 旧版本完整可读 ✅；残留 id≠Meta.txn_id → 下次 save/recover 删除 |
| 3 之后、4 完成前（提交后） | Meta=新(txn_id=X)、部分正式文件旧、残留 `.t<X>` | 下次启动 `load_meta()` 自动 `recover()`：id==Meta.txn_id 的 `.t<X>` 发布为正式文件 ✅ |
| 4 全部完成后 | 正式文件=新、Meta=新、无残留 | 正常 ✅ |

恢复规则（`workflow_storage_recover_internal`）：
- valid Workflow：`id == Meta.txn_id` 的暂存 → rename 发布；其它 id → 删除
- invalid Workflow：全部暂存 → 删除（不复活已删除的 Workflow）

### Meta 格式兼容

- 头部 `version` 字段与文件长度双重判定：v1=1308B / v2=1372B。
- **v1 文件仍可读**（`txn_id` 视为 0），读后不自动迁移；下次 `save()` 写出 v2。
- v2 长度但头部 version<2、或版本>2 等矛盾 → 拒绝并保持 Invalid。

### CRC 修正

- `workflow_storage_load()`：删除 `e.crc32 != 0 &&`，**始终**把重算 CRC 与 Meta 比对。
- save()/load() 聚合 Workflow 级 CRC 时用 `crc_append_u32_le()` 显式折叠 4 字节小端，
  不再 `(const uint8_t*)&step_crc` 直读内存（跨平台格式漂移风险）。
- 说明：ESP32 为小端，旧固件写入的聚合 CRC 字节序与 LE 折叠一致，v1 数据读取不受 CRC 语义变化影响。

---

## 3. 各文件改动清单

| 文件 | 改动 |
|---|---|
| `src/file_storage.h/.cpp` | 新增 `file_storage_foreach()`（只读目录遍历；回调收名字副本，可安全 remove/rename） |
| `src/bin_storage.h/.cpp` | 新增 `bin_storage_foreach()` 薄包装（维持 WorkflowStorage→BinStorage→FileStorage 分层） |
| `src/workflow_storage.h` | MetaEntry 增加 `txn_id`；`WF_STG_META_VERSION=2`（`WF_STG_VERSION=1` 仍是 Step 格式版本）；错误码 `WF_STG_ERR_TEST_ABORTED`；`save_step` 边界语义注释；新增 `workflow_storage_recover()` 与 3 个测试钩子声明 |
| `src/workflow_storage.cpp` | 常量区（v1/v2 Meta 尺寸、`.t` 后缀、故障注入状态）；`serialize/deserialize_meta` 支持 v1/v2 双布局；`crc_append_u32_le`；暂存文件路径构造/解析、`stage_process`（发布或清理）；`load_meta()` 成功加载后自动恢复 + 首次/损坏时清理残留；`recover()`；`workflow_storage_save()` 整体重写为暂存式事务；`load()` CRC 修正；故障注入实现（文件尾） |
| `src/main.cpp` | 新增 `wfst` 串口测试控制台（`wfst_console`，追加于文件尾 + 前置声明 + 解析分支） |

**未改动**：`workflow.cpp` / `workflow.h` / `config_manager.*` / `json_storage.*`。
**对已完成 workflow.cpp 改造的影响：无。** workflow.cpp 使用的 7 个存储 API
（`init / load_meta / get_valid / load / save / delete / result_name`）签名全部不变，
编译通过即证明适配无虞。唯一行为变化是 save 语义从"逐个覆盖正式文件"升级为
"事务式"，对上层返回码不变（失败仍返回对应错误码且不清 Dirty、不 release Critical）。

---

## 4. 独立测试（上板执行）

已内置 `wfst` 串口命令（115200，回车触发）。`wfst help` 列出全部操作。

```text
# 0) rename 覆盖语义快速验证（人工确认无报错即可）
wfst seed 0 3 100      # 保存 wf0: 3 步, 参数 100/101/102
wfst overwrite 0 3 200 # 再次保存覆盖
wfst verify 0 3 200    # → PASS（覆盖成功）

# 1) 单 Step 保存/读取/CRC
wfst seed 1 1 50
wfst dump 1            # step00 id=act_1_0 p0=50

# 2) 完整 Workflow 保存并"重启读取"（重启后执行）
wfst seed 2 6 100
wfst verify 2 6 100    # 重启前后都应 PASS

# 3) step_count=8 只加载 0~7
wfst seed 3 8 100
wfst verify 3 8 100    # PASS（只读 0..7）

# 4) step_count 8→5，旧 S05~S07 BIN 保留但忽略
wfst overwrite 3 5 200
wfst verify 3 5 200    # PASS
wfst ls 3              # step-bin-exist=8（S05..S07 仍物理存在）, staged=0

# 5) 删除后 Step BIN 仍存在
wfst del 3
wfst ls 3              # step-bin-exist=8（未删 BIN）
wfst dump 3            # load NOT_FOUND

# 6) 删除后重新创建
wfst seed 3 4 300      # → OK
wfst verify 3 4 300    # PASS

# 7) 多 Step 保存中让第 N 个写失败
wfst seed 4 6 100
wfst fail 4 3          # 第 3 步注入失败 → r=WRITE_FAILED staged=0
wfst verify 4 6 100    # PASS（旧版本完整可读）

# 8) 见 #7 的 verify 结果（旧 Workflow 仍可完整读取）

# 9) 模拟事务提交前掉电
wfst abort1 4 6 200    # → r=TEST_ABORTED staged=6（暂存残留=掉电现场）
wfst recover           # 清理残留（id≠Meta.txn_id → 删除）
wfst verify 4 6 100    # PASS（仍是旧参数 —— 提交前掉电旧版本可读）

# 10) 模拟事务提交后、发布未完成掉电
wfst abort2 4 6 300    # → r=TEST_ABORTED staged=6
wfst verify 4 6 100    # 此刻应 FAIL/CRC 失败（崩溃现场：Meta 新、文件旧）——预期
wfst recover           # 识别 txn_id 匹配 → 发布
wfst verify 4 6 300    # PASS（恢复为新参数）
```

掉电场景的"重启恢复"路径（非模拟）验证：
`abort2` 后**直接断电重启**，串口应出现正常启动日志，随后：

```text
wfst verify 4 6 300    # PASS（启动时 load_meta 自动完成了发布恢复）
```

---

## 5. 已知限制与遗留

1. **rename 覆盖语义**：代码路径已静态核实为 POSIX 原子替换（littlefs），
   但**未经上板实测** —— 建议烧录后先跑测试 0。
2. **真实掉电 vs 模拟**：测试 9/10 用故障注入在"代码层掉电点"返回模拟断电；
   上板后建议再各做一次**物理断电**复测（abort 后立刻断电重启）。
3. `workflow_storage_save_step()` 保留，但仅适合单点修复/调试；
   真正的 Dirty 清理仍以 `workflow_save_transaction()`（整 Workflow 事务）为界。
4. `meta.bin` 从 v1→v2：旧 v1 文件可读，但不会自动改写为 v2；
   首次 `save()` 后自然升级。
5. 编译：SUCCESS 157.14s，0 error，0 新增 warning（唯一告警为既有
   `cloud_manager.cpp:1041` DynamicJsonDocument 弃用），RAM +80 B。未烧录。
