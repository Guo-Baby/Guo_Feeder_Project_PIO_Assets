# Workflow Cloud Sync Progress

> 任务中断后，**首先读本文件**，即可知道做到哪一步、下一步做什么。
> 对应需求：`workflow修改需求文档.md`

## Current Phase

**Phase 5 — README 更新 + 脚本化回归 + 最终报告**

## Task Summary

| 阶段 | 内容 | 状态 |
|---|---|---|
| Phase 1 | Workflow RAM `valid` 标志 + `workflow.list` 增 `valid` + `workflow.sync_info` | ✅ 完成 |
| Phase 2 | `set` 无变化不增 variant（canonical 比较）+ `save` 全 Dirty 语义核对 | ✅ 完成 |
| Phase 3 | PSRAM / 栈安全优化 + save 可观测性 + 保存失败故障注入 | ✅ 完成 |
| Phase 4 | `workflow_cloud_interface.md` 云端适配文档 | ✅ 完成 |
| Phase 5 | README 更新 + 回归测试 + 最终报告 | 🚧 进行中 |

## Completed

### Phase 1（commit `57973aa`）

- `Workflow` 增加 RAM `valid` 标志（非持久化字段，BIN 里不存在）。
  - `create` / `set` / BIN 加载 / JSON 加载 → `true`
  - `delete`（逻辑删）→ `false`
- 新增 `workflow_is_valid(idx)`。
- `workflow.list` 每项返回 `valid`；`workflow.get` 顶层返回 `valid`
  （刻意放在 `workflow{}` **外面**：`workflow{}` 是可回传 `set` 的纯持久化 Schema）。
- 新增 `workflow.sync_info`：只返回 `registry_version` / `registry_checksum`
  / `count` / `dirty`，不返回列表、不返回完整 Workflow。
- `workflow_find_free_slot` 增加第二轮：回收已 `delete` 的 Slot
  （否则连续 create/delete 填满 16 槽后，重启前永远无法再 create）。

### Phase 2（commit `57973aa`）

- 新增 `workflow_definition_matches_json()`：canonical 文本比较
  （固定字段顺序 + 参数按 name 排序 + 值带 `i/f/b/s` 类型前缀）。
- `workflow.set` 幂等：内容完全一致 → 不 `variant++`、不重写、返回 `changed:false`。
  动机：云端 UI"打开编辑→原样保存"若无条件 `variant++`，会让
  registry checksum 无意义变化 → `registry_version` 虚增 → 云端反复拉取。

### Phase 3（commit `56b3d55`）

- `workflow_init()` 打印三大实例池实际落点：
  ```
  [Workflow] pools: trigger=PSRAM(114688 B) action=PSRAM(108544 B)
                    step_def=PSRAM(105472 B) | PSRAM total=8386007 B
  ```
- `WorkflowDefinition`（约 8.6KB）由 `workflow_storage_alloc_definition()`
  统一 PSRAM 优先 + DRAM 回退；调用方**无栈上实例**。
- `workflow_save_transaction()` 打印 dirty 清单 + `critical_held`，
  每个对象单独打印落盘结果。
- 新增 `workflow_storage_test_fail_wf(wf)`：令指定 Workflow 的**整次** save
  返回 `WRITE_FAILED`（一次性），区别于已有的 `fail_step`（单 Step 失败）。
- `wfst` 控制台新增 `failwf <n>`。

### Phase 4（本文件同批提交）

- 新增 `workflow_cloud_interface.md`（15 章 + 2 附录）：
  分层架构、概念模型、JSON Schema、Definition/Runtime、
  修改限制、Variant、Capability Registry & Stable ID、
  传输格式（明文 + 云端紧凑键映射）、7 个命令详解（请求/参数/返回/错误/
  variant 行为/dirty 行为）、错误码、同步流程、Action/Trigger 清单、
  UI 对接注意事项、边界限制、测试方法。

## Current Commit

| commit | 说明 |
|---|---|
| `56b3d55` | perf(workflow): PSRAM 池分配可见性 + save 全 Dirty 可观测性 + 保存失败故障注入 |
| `57973aa` | feat(workflow): 云端同步协议完善 — valid 字段 / sync_info / set 幂等 |
| `8a1e4c4` | perf(registry): 临时缓冲改 PSRAM 优先 + load 去掉栈上 800B tmp |
| `6555a60` | docs: CommandManager/version 适配改造进度 + 修改报告 |
| `3b0ba11` | fix(storage): load 未回填 variant → 重启后版本倒退为 1 |
| `beff2d3` | fix(registry): rescan 栈上 2.4KB 对象改堆分配（修 loopTask 栈溢出崩溃） |
| `82afacb` | fix(cmd): 补回 workflow.* 一级路由分支 |
| `d3835cf` | fix(workflow): apply_json 忽略 workflow_count 推进 + 补串口 cm 分发 |
| `ca168cf` | feat(workflow): variant 版本机制 + CommandManager workflow.* 云端同步命令 |

## Tests

### 已执行（上板 COM8，真实固件）

| 用例集 | 结果 | 日志 |
|---|---|---|
| `wf_sync_tests.txt`（32 断言，覆盖 §33–§37） | **32/32 PASS** | `.pio/wf_sync_run1.log` |
| `wf_reboot_tests.txt`（7 断言，覆盖 §35） | **7/7 PASS** | `.pio/wf_reboot_run1.log` |

关键证据：

```
[Workflow] pools: trigger=PSRAM(114688 B) action=PSRAM(108544 B) step_def=PSRAM(105472 B)
[CapRegistry] sync buf 2328 B in PSRAM (hwm=9844)
[CapRegistry] ACTION   reuse    version=1  count=7
[CapRegistry] TRIGGER  reuse    version=1  count=3
[CapRegistry] WORKFLOW rebuild  version=20 count=4   ← 新增 cm_a → version++
[WFStg] TEST fail_wf injected: wf=6                  ← §34 故障注入生效
[Workflow] save transaction: dirty=[3 5 6 ] critical_held=1
```

| 验收项 | 证据 |
|---|---|
| §33-A 基础 | `sync_info` / `list(valid)` / `get(valid)` 全部命中 |
| §33-B 创建 | `create` → `variant:1`，`list` 立即可见，`get` 可读 |
| §33-C 修改 | `set` → `variant:2`，`changed:true` |
| §33-C 幂等 | 相同内容再 `set` → `variant:2`，`changed:false` |
| §33-D 删除 | `delete` → `variant:2`、`valid:false`、`list` 可见 |
| §33-E 多对象 save | 3 个 Dirty 一次 `save` → `saved:true` `dirty:false` |
| §33-E 无脏不重启 | `dirty=false` 时 `restart:true` → `restarting:false` |
| §34 保存失败 | 注入失败 → `e:10`；`dirty` 仍 `true`；重试 → `saved:true` |
| §35 重启持久化 | `variant=2` 跨重启保持；已删对象重启后不复活 |
| §36 Registry | `object_version` 随 variant 变化，`registry_version++` |
| §37 PSRAM | 三大池 + registry 缓冲全部命中 PSRAM；栈 hwm 9828/16384 |

## Known Issues

| 级别 | 问题 | 状态 |
|---|---|---|
| 中 | `enable` 缺省值两条路径不一致：JSON 导入缺省 `true`，BIN 加载缺省 `false` | 已写入接口文档 §5.3，建议云端始终显式传 `enable`；**代码未改**（改动会影响既有行为） |
| 低 | `step.id` 未注册时设备**静默接受**（保存但不执行） | 设计如此，已在接口文档 §3.5 显式说明并要求云端校验 |
| 低 | `stable_id` 按 `runtime_id` 字母序生成，新增/改名会让后续对象整体平移 | 非缺陷，是排序编号固有性质；已在接口文档 §7 强调"以 `id` 为主键" |
| P2 | Temp Action 环形队列 `queue_wr_ptr` / `queue_rd_ptr` 跨任务无保护 | **既有问题**，非本次引入，待独立立项 |

## Resolved

- ✅ 重启后 `variant` 回退为 1 → `workflow_storage_load()` 未回填 `variant`（`3b0ba11`）
- ✅ `workflow.create` 触发 loopTask 栈溢出崩溃 → `registry_sync` 栈上 2.4KB 对象改堆分配（`beff2d3`）
- ✅ `workflow.*` 全部 "Unknown command" → 一级路由分支被并行编辑覆盖（`82afacb`）
- ✅ `create` 后重启前对 registry/云端不可见 → `apply_workflow_json` 未推进 `workflow_count`（`d3835cf`）
- ✅ registry 临时缓冲只用内部 DRAM 未用 PSRAM → `registry_buf_alloc()` PSRAM 优先（`8a1e4c4`）
- ✅ `load_registry_file()` 栈上 800B `CapabilityRegistryTable tmp` → 直写调用方缓冲（`8a1e4c4`）
- ✅ `table_reset()` 漏清 `object_version` → 未用槽位残留旧值（`8a1e4c4`）

## Next

1. 更新 `readme.md` 的 Workflow 架构说明（§31）
2. 扩充/整理回归用例并复跑，输出最终测试报告（§38-10）
3. 输出最终报告，逐条回答 §39 的 18 个问题

## Notes（踩坑速查）

- 沙箱 `safe-delete` 拦截删除 `.o`/`.elf` → **增量编译必失败**；
  必须 `PLATFORMIO_BUILD_DIR=.pio/build/xxx pio run`（PlatformIO 6 无 `--build-dir`）。
- 本环境 `pio run -t upload` 只编译不烧录；烧录用
  `python .platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3
   --port COM8 --baud 921600 write_flash -z 0x10000 <build>/firmware.bin`。
- 打开串口会复位开发板 → 逐条手敲命令会与 boot 竞争，必须用
  `.pio/serial_batch.py` 批处理。
- 并行编辑同一文件会互相覆盖 → 改完务必 `grep` 复核关键分支是否还在。
- `serial_batch.py` 参数顺序：`<port> <logfile> <testfile> [wait]`。
