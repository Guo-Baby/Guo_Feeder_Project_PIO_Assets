# Workflow 架构适配 CommandManager 及 Version —— 修改报告

日期：2026-09-12　板：ESP32-S3（COM8）　基线：`1a344e5`

---

## 一、改了什么

| # | 模块                           | 改动                                                                                                                                    |
| - | ---------------------------- | ------------------------------------------------------------------------------------------------------------------------------------- |
| 1 | `workflow_storage.h/.cpp`    | `WorkflowDefinition` / `WorkflowMetaEntry` 新增 `uint32_t variant`；Meta 格式 v2 → **v3**（entry 85B → 89B），兼容读 v1/v2；新增 `get/set variant`  |
| 2 | `workflow.h/.cpp`            | `struct Workflow` 新增 `variant`；create=1 / set=+1 / delete=+1；JSON 导入导出带 `variant`；新增 5 个 API                                          |
| 3 | `capability_registry.h/.cpp` | `object_version = workflow.variant`；文件条目新增 `object_version`（格式 v2，魔数 CAPA→AP2A 触发一次性重建）；新增 `capability_get_workflow_object_version()` |
| 4 | `command_manager.h/.cpp`     | 新增 `workflow.*` 二级路由 + 6 个命令；新增错误码 11/12/13；新增结果回显开关                                                                                  |
| 5 | `main.cpp`                   | 注册 CommandManager 日志回调；新增串口 `cm {json}` 直通                                                                                            |
| 6 | `platformio.ini`             | `-DARDUINO_LOOP_STACK_SIZE=16384`                                                                                                     |

**提交**：`ca168cf` → `d3835cf` → `82afacb` → `beff2d3` → `3b0ba11`（工作区干净）

---

## 二、上板实测暴露并修复的 3 个缺陷

### 1. loopTask 栈溢出崩溃（`workflow.create` 必崩）

```
[WF][DBG] apply_json wf=3 id=cm_test create=1 steps=4 variant=1
Guru Meditation Error: Core 1 panic'ed (Unhandled debug exception)
Debug exception reason: Stack canary watchpoint triggered (loopTask)
```

根因：`capability_registry_rescan()` → `registry_sync()` 在栈上放三个大对象  
（`CapabilityMapping current[32]` ≈ 800B + 两张 `CapabilityRegistryTable` ≈ 800B×2），  
命令路径本身已消耗大半 8KB 栈。

修复（两道保险）：

- 三个工作区改 `heap_caps_malloc()` + **placement new** 就地构造（String 成员必须构造），  
  返回前**显式析构再释放**；不用 `static` —— 云端命令（MQTT 任务）与串口命令（loop 任务）可并发进入
- `platformio.ini` 加 `-DARDUINO_LOOP_STACK_SIZE=16384`（DRAM 余量充足：静态 130KB / 327KB）

验证：create 前后 `stack_hwm = 9876 → 9140`，全程 0 崩溃。

### 2. 新建 Workflow 重启前对云端不可见

`workflow_apply_workflow_json()` 写入槽位后没有推进 `workflow_count`，  
而 CapabilityRegistry 以 `workflow_get_count()` 为扫描上限 ——  
create 走的是本函数（不是 `workflow_create()`），于是新建项在重启前  
既拿不到 `stable_id`，也不会出现在 `workflow.list` 里。已在 variant 推进前补上。

### 3. variant 重启后倒退为 1（致命）

`workflow_storage_load()` 从 Meta Entry 拷贝 `id/name/enable/timeout_ms/step_count`，  
漏掉 v3 新增的 `variant` → 恒为 0 → 上层按"0 提升为 1"处理。

实测：`persist_test` 改到 variant=2 并落盘 → 重启读到 **variant=1**。  
云端会认为"版本倒退"，永远同步不到正确内容。修复后实测重启仍为 **variant=2**。

---

## 三、debug 串口打印清单（预埋）

| 前缀                         | 位置                                                          | 示例                                                                       |
| -------------------------- | ----------------------------------------------------------- | ------------------------------------------------------------------------ |
| `[WF][DBG]`                | workflow.cpp：BIN 加载 / create / delete / apply_json / export | `[WF][DBG] apply_json wf=3 id=cm_test create=0 steps=3 variant=2`        |
| `[CMD][WF]`                | command_manager.cpp：6 个命令入口与结果                              | `[CMD][WF] list: registry_version=2 checksum=2907697010 count=4 dirty=1` |
| `[CMD][INFO/ERROR/RESULT]` | command_manager 日志回调（main.cpp 注册）                           | `[CMD][RESULT] {"cmd":"result",...}`                                     |
| `[CapRegistry]`            | registry 复用 / 重建                                            | `[CapRegistry] WORKFLOW rebuild version=2 count=4 checksum=2907697010`   |
| `[Bin][E]`                 | BinStorage 错误（此前静默）                                         | —                                                                        |

---

## 四、怎么测试

### 4.1 准备

```bash
# 编译（必须用全新 build 目录，避开 safe-delete 拦截增量编译）
TS=$(date +%s); BD=".pio/build/t_$TS"
PLATFORMIO_BUILD_DIR="$BD" pio run

# 烧录（本环境 pio run -t upload 只编译不烧！必须用 esptool 直烧 app 分区）
D:/platformIO/penv/Scripts/python.exe D:/platformIO/packages/tool-esptoolpy/esptool.py \
  --chip esp32s3 --port COM8 --baud 921600 --before default_reset --after hard_reset \
  write_flash -z 0x10000 "$BD/esp32-s3-devkitc-1/firmware.bin"
```

### 4.2 串口直通测试（不需要 MQTT/云端）

串口 115200。新增 `cm` 命令把云端 JSON 原样喂给 CommandManager，  
走的是**与云端完全相同的路由链**，并临时打开结果回显，串口直接看到结果 JSON：

```
cm {"cmd":"workflow.list","id":"t1"}
[CMD][INFO] Received command
[CMD][RESULT] {"registry_version":1,...,"workflows":[{"stable_id":0,"id":"daily_valve_test","variant":1}],"status":"success",...}
cm: ret=1
```

批量脚本（只复位一次、保留 boot 日志、自动判定期望）：

```bash
python .pio/serial_batch.py COM8 .pio/cm_run.log .pio/cm_tests.txt 2.5
# cm_tests.txt 每行: 命令 ||| 期望子串
```

> 注意：打开串口会复位板子，单条命令脚本会和 boot 抢时序，必须用批量脚本。

### 4.3 已执行的用例与结果（16/16 通过，日志 `.pio/cm_run5.log`）

| 用例                                  | 期望                          | 结果 |
| ----------------------------------- | --------------------------- | -- |
| `workflow.list` 基线                  | status=success，含 variant    | ✅  |
| `workflow.get` by stable_id / by id | 返回完整对象                      | ✅  |
| `workflow.create`                   | variant=1                   | ✅  |
| 建后 `list` 出现新条目                     | 含 cm_test                   | ✅  |
| `workflow.set`                      | variant=2                   | ✅  |
| 改后 `get`                            | 新内容                         | ✅  |
| 错误：stable_id 不存在                    | error_code=5                | ✅  |
| 错误：未知命令                             | error_code=1                | ✅  |
| `workflow.save`（restart=false）      | saved=true，dirty=false      | ✅  |
| `workflow.delete`                   | variant=3                   | ✅  |
| 重启持久化                               | variant=2 保持、已删项消失          | ✅  |
| 无变更重启                               | `reuse version=15`（不误 bump） | ✅  |

### 4.4 云端 / MQTT 通道怎么测

串口直通测的是 CommandManager 之后的链路。要测 CloudManager 段，  
按 `cloud_manager.cpp:1048` 的两种下行格式发 MQTT：

- **旧格式（调试首选，无需 version / stable_id）**
  ```json
  {"cmd":"workflow.list","id":"cloud_1","ob":""}
  ```
- **新格式（压缩）**：`{"c":...,"i":...,"v":...,"k":...}`，需与 registry version 匹配

设备应答统一为 `{"cmd":"result","id":<原 id>,"command":"workflow.xxx",...}`，  
CloudManager 会转成上行 JSON 发给云端。

---

## 五、支持的命令格式与示例

### 通用约定

- 命令名：`workflow.list` / `workflow.get` / `workflow.create` / `workflow.set` / `workflow.delete` / `workflow.save`
- 参数放 `p`（新格式）或 `pl`（旧格式），可以是 `stable_id`、`id` 或 `workflow` 对象
- 定位优先级：`p.stable_id` > `p.id` > `ob`
- 应答统一由框架补 `cmd=result`、`id`、`command`、`timestamp`；失败另有 `error_code` + `message`

### 5.1 workflow.list

请求：

```json
{"cmd":"workflow.list","id":"a1"}
```

应答：

```json
{"registry_version":2,"registry_checksum":2907697010,"count":4,"dirty":true,
 "workflows":[{"stable_id":0,"id":"cm_test","variant":1},
              {"stable_id":1,"id":"daily_valve_test","variant":1}],
 "status":"success","cmd":"result","id":"a1","command":"workflow.list"}
```

### 5.2 workflow.get

请求（二选一）：

```json
{"cmd":"workflow.get","id":"a2","p":{"stable_id":0}}
{"cmd":"workflow.get","id":"a3","p":{"id":"daily_valve_test"}}
```

应答：

```json
{"workflow":{"id":"daily_valve_test","name":"每日开阀测试","variant":1,"enable":true,
  "timeout_ms":60000,
  "steps":[{"type":"trigger","id":"timer_daily","params":{"hour":7,"minute":0}},
           {"type":"action","id":"VALVE_OPEN"},
           {"type":"action","id":"DELAY","params":{"ms":30000}},
           {"type":"action","id":"VALVE_CLOSE"}]},
 "stable_id":1,"status":"success","cmd":"result","id":"a2","command":"workflow.get"}
```

### 5.3 workflow.create

请求：

```json
{"cmd":"workflow.create","id":"a4","p":{"workflow":{
  "id":"cm_test","name":"CM Test","enable":true,"timeout_ms":30000,
  "steps":[{"type":"trigger","id":"timer_daily","params":{"hour":9,"minute":30}},
           {"type":"action","id":"VALVE_OPEN"},
           {"type":"action","id":"DELAY","params":{"ms":1000}},
           {"type":"action","id":"VALVE_CLOSE"}]}}}
```

应答：

```json
{"status":"success","slot":3,"stable_id":0,"id":"cm_test","variant":1,"dirty":true,...}
```

（`p` 直接写 workflow 本体、不套 `workflow` 键也可以：只要含 `id` 字段）

### 5.4 workflow.set（整体替换，variant 自增）

请求：

```json
{"cmd":"workflow.set","id":"a5","p":{
  "id":"cm_test",
  "workflow":{"id":"cm_test","name":"CM Test v2","enable":true,"timeout_ms":45000,
    "steps":[{"type":"trigger","id":"timer_daily","params":{"hour":11,"minute":15}},
             {"type":"action","id":"VALVE_OPEN"},
             {"type":"action","id":"VALVE_CLOSE"}]}}}
```

应答：`{"status":"success","slot":3,"stable_id":0,"id":"cm_test","variant":2,"dirty":true,...}`

### 5.5 workflow.delete

请求：`{"cmd":"workflow.delete","id":"a6","p":{"stable_id":0}}`  
应答：`{"status":"success","slot":3,"id":"cm_test","variant":3,"dirty":false,...}`

> 删除不是物理删除：只置 `meta.valid=false`，不删 Step BIN；variant 仍 +1，  
> 云端 list 能发现差异。RAM 中该条目当次仍可见，重启后消失。

### 5.6 workflow.save

请求：`{"cmd":"workflow.save","id":"a7"}`  
或跳过重启：`{"cmd":"workflow.save","id":"a8","p":{"restart":false}}`

应答：`{"status":"success","saved":true,"dirty":false,"restarting":true,"registry_version":4,...}`

> 与 `config_save` 同款语义：重启由 SystemCommand 统一执行  
> （等 Critical Operation 归零 → 10s 安全窗口）。**没有 Dirty 时不请求重启**。

### 5.7 错误码

| code | 含义                                       |
| ---- | ---------------------------------------- |
| 1    | Unknown command                          |
| 5    | Workflow not found（stable_id / id 找不到）   |
| 6    | Missing 'workflow' object / 参数错误         |
| 11   | payload 不是合法 JSON                        |
| 12   | 无空槽位（16 个已满）                             |
| 13   | 修改被拒（Workflow 运行中 / Critical acquire 失败） |

---

## 六、遗留 / 注意

- `workflow.save` 默认会触发安全重启（可用 `restart:false` 关闭）
- 删除后 RAM 条目当次仍出现在 `list` 中（variant 已 +1），重启后消失 —— 云端以 list 差异 + get 结果为准
- Registry 文件格式 v2 与 v1 不兼容：旧 `/registry/*.bin` 自动失效重建（一次性）
- 所有 commit 未 push（仓库无 tracking 分支）
