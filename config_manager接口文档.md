# ConfigManager 接口文档

> 项目：Guo Feeder Project（ESP32-S3 N16R8）
> 版本：配置模块定版（含 P0 + P1）
> 面向：AI 辅助开发 / 云端对接 / 后续维护
> 关联源码：`src/config_manager.h`、`src/config_manager.cpp`、`src/command_manager.cpp`
> 关联文档：`config manager开发架构.md`、`jsonstorage开发架构.md`、`cloud_protocol.md`

---

## 1. 模块定位

ConfigManager 是**持久化配置的唯一管理者**。职责边界：

- 负责：配置文件的读、写、删除、版本管理、备份/回滚、出厂副本
- **不负责**：判断业务取值是否合法（GPIO 范围、端口范围等由上层决定）

配置修改**只允许**通过 ConfigManager 进行，其他模块不得直接写配置文件。

### 分层位置

```
CloudManager（MQTT / JSON 转换）
    ↓
CommandManager（命令解析 / 路由 / 生命周期）   ← 配置命令挂在这里
    ↓
ConfigManager（异步命令队列 + 文件读写）
    ↓
JsonStorage（文件原子读写 / 分块）
    ↓
LittleFS（文件系统）
```

**禁止跨层**：ConfigManager 不得直接操作业务模块；业务模块不得直接写配置。

---

## 2. 配置模块清单（8 个，编译期固定）

| 模块名 | 常量 | 说明 |
|---|---|---|
| `wifi` | `CONFIG_MODULE_WIFI` | WiFi SSID / 密码 / 超时 |
| `oled` | `CONFIG_MODULE_OLED` | I2C 引脚 / 帧率 / 旋转 |
| `valve` | `CONFIG_MODULE_VALVE` | 阀门 GPIO / 电平 / 安全超时 |
| `time` | `CONFIG_MODULE_TIME` | 时区 / NTP 服务器 / 同步周期（秒） |
| `weight` | `CONFIG_MODULE_WEIGHT` | HX711 引脚 / 比例 / 零点 |
| `mqtt` | `CONFIG_MODULE_MQTT` | 服务器 / 端口 / 主题 / CA |
| `mi_thermo` | `CONFIG_MODULE_MITHERMO` | 米家 BLE key / MAC |
| `rtc` | `CONFIG_MODULE_RTC` | PCF8563T 使能 / SDA / SCL / I2C 地址 / 校准阈值（秒） |

> `rtc` 的 sda / scl 供人阅读与一致性校验，实际 I2C 总线由 `oled` 初始化（RTC 复用同一总线，不重复 `Wire.begin()`）。

**模块集合是固定的，不支持运行时新增模块**（动态新增没有 getter 支撑，属于死数据）。
workflow 的增删改查属 WorkflowManager 职责，不在本模块。

---

## 3. 文件布局

| 路径 | 用途 |
|---|---|
| `/config/<module>.json` | Active，当前生效配置 |
| `/config/<module>.json.bak` | Backup，上一次保存前的配置（每次保存自动滚动） |
| `/config/version.json` | 各模块版本号 |
| `/config/.bootok` | 启动成功标记（Boot Validation） |
| `/config/.commit` | 提交状态：`PENDING` / `DONE` / `FAILED` |
| `/factory/<module>.json` | factory 副本，永不滚动的初始基线 |

**注意**：
- `/config/` 随 `uploadfs` 烧录（`data/config/*.json`）
- `/factory/` **不烧录**，由云端下发 `config_backup` 创建；首次写入前代码会自动 `mkdir`

---

## 4. 云端命令参考

### 4.0 命令通用格式

本章节全部使用**紧凑格式**（云端实际使用）：

```json
{"c":"system","i":"<cmd_id>","p":{"o":"<config_xxx>", ...参数}}
```

| 字段 | 含义 |
|---|---|
| `c` | 固定 `"system"`，配置命令挂在 system 命令族 |
| `i` | 命令 ID，**每次必须不同**（重复会被 `cloud_check_duplicate_cmd` 丢弃） |
| `p.o` | object，即具体配置命令：`config_query` / `config_set` / … |
| 其余 | 命令参数，与 `o` 同级放在 `p` 内 |

> 也支持未压缩格式 `{"cmd":"system","id":"1001","ob":"config_query","pl":{...}}`，
> CloudManager 会走"原样透传"分支。两种格式等价，**推荐紧凑格式**。

### 4.0.1 两段式响应（重要）

所有配置命令都是异步的，云端会收到**两条**上行：

**① ACK**（CloudManager 立即回，表示已收到）
```json
{"c":"ack","i":"1001","o":"config_query","p":{"result":"received"},"t":1788109446}
```

**② 第一阶段 result（accepted）** —— 表示已入队，**不代表执行成功**
```json
{"s":1,"c":"result","i":"1001","m":"config query queued","command":"system"}
```
> `s:1` = STATUS_ACCEPTED

**③ 第二阶段 result（最终成败）** —— 执行完毕后回
```json
{"s":0,"c":"result","i":"1001","o":"config_query","m":"query module ok","e":0,
 "type":"config_result","command":"system","data":{...},"version":8}
```
> `s:0` = STATUS_OK，`s:3` = STATUS_FAILED

**UI 必须以第二阶段的 `s` 判断成败，不能看第一阶段的 accepted。**

### 4.1 config_query —— 查询

```json
{"c":"system","i":"1001","p":{"o":"config_query","module":"weight","key":"scale"}}
```

| 参数 | 必填 | 说明 |
|---|---|---|
| `module` | 否 | 不填 = 查全部模块 |
| `key` | 否 | 不填 = 查整个模块 |
| `keys_only` | 否 | `true` = 只返回键名，不返回值 |

三种组合：

| 组合 | 返回 |
|---|---|
| 无 module | 全部模块内容 + `versions` 对象 |
| 有 module 无 key | 该模块内容 + `version` |
| 有 module 有 key | 单字段：`{"data":{"module":"weight","key":"scale","value":741},"version":8}` |

**只查键名**（降低报文长度，避免触发 1024 字节分片）：
```json
{"c":"system","i":"1002","p":{"o":"config_query","keys_only":true}}
```
→ `{"data":{"modules":["wifi","oled","valve","time","weight","mqtt","mi_thermo"]}}`

```json
{"c":"system","i":"1003","p":{"o":"config_query","module":"weight","keys_only":true}}
```
→ `{"data":{"keys":["dt","sck","sample_interval","scale","zero_offset","filter_samples"],"version":8}}`

### 4.2 config_set —— 新增 / 修改单个字段

```json
{"c":"system","i":"1004","p":{"o":"config_set","module":"weight","key":"scale","value":750}}
```

| 参数 | 必填 | 说明 |
|---|---|---|
| `module` | ✅ | |
| `key` | ✅ | 不存在则新增，存在则覆盖 |
| `value` | ✅ | **只支持标量**：int / bool / float / string |
| `expect_version` | 否 | 乐观锁，见 4.8 |

- `value` 为 null / object / array 会在入队阶段被拒绝
- **类型必须与已有字段一致**：string 字段不接受数字，bool 字段只接受 bool
  （防止 `"123" → int`、`123 → bool` 这类危险隐式转换）
- 修改后**需要重启生效**，自动进入 5 分钟倒计时

### 4.3 config_module —— 批量写入模块字段

```json
{"c":"system","i":"1005","p":{"o":"config_module","module":"weight",
 "value":{"dt":6,"sck":7,"sample_interval":50,"scale":741,"zero_offset":-800750,"filter_samples":10}}}
```

| 参数 | 必填 | 说明 |
|---|---|---|
| `module` | ✅ | |
| `value` | ✅ | 必须是 object |
| `expect_version` | 否 | 乐观锁 |

**严格校验**：传入的每个字段都必须在目标模块中**已存在**，
任一字段不存在则**整体拒绝**，不写入任何内容，串口打印具体字段名：

```
[CFG] module write rejected, unknown field: [typo_field]
```

返回：`{"e":5,"m":"unknown field, module write rejected"}`

**语义 = 覆盖式更新（merge），不是完全替换**：
只覆盖传入的字段，未提及的字段保持不变。
> 这样即使 UI 少传字段也不会误删配置。需要删字段请用 `config_delete`。

`value` 超过 96 字节时会在 PSRAM 暂存（上限 8KB），任务执行完立即释放。

### 4.4 config_delete —— 删除字段

```json
{"c":"system","i":"1006","p":{"o":"config_delete","module":"weight","key":"skc"}}
```

- **真正从配置中移除该字段**（不是置默认值）
- 字段不存在 → `e:4` `key not found`
- 删除后查询该字段会返回 `key not found`
- 需要重启生效

### 4.5 config_save —— 立即保存到 Flash

```json
{"c":"system","i":"1007","p":{"o":"config_save"}}
```

- 把 dirty 模块写入 Flash（Active + Backup + version + commit 状态）
- 无 dirty 时幂等返回成功：`{"data":{"saved":false,"dirty":false}}`

> **什么时候需要显式 save？**
> `config_set` **只改 RAM，不写 Flash**。写 Flash 只发生在：
> ① 本命令 ② 5 分钟倒计时到点 ③ 主动 `config_restart`
> 所以：想立刻落盘就发 save；不发的话重启前也会自动保存，但中途断电会丢。

### 4.6 config_restart —— 重启 / 取消重启

```json
{"c":"system","i":"1008","p":{"o":"config_restart"}}
{"c":"system","i":"1009","p":{"o":"config_restart","cancel":true}}
```

- 不带 `cancel`：**先自动 `config_save()`** → 回调 SUCCESS → **等 10 秒安全延迟** → `ESP.restart()`
- `cancel:true` 取消待重启（清倒计时）

### 4.7 config_backup / config_reset —— factory 副本

**备份当前配置为 factory**：
```json
{"c":"system","i":"1010","p":{"o":"config_backup","module":"weight"}}
```
- 写 `/factory/weight.json`，**不改动当前配置、不标记 dirty、不重启**

**从 factory 恢复**：
```json
{"c":"system","i":"1011","p":{"o":"config_reset","module":"weight"}}
```
- **完全替换**模块内容为 factory 内容（与 merge 语义不同，重置就是要回到基线）
- factory 不存在 → `e:8` `factory copy not found`
- 需要重启生效

> **factory 与 Backup 的区别**（关键）：
> Backup 每次保存自动滚动，只回退一步，用户不可控；
> factory 只在显式要求时更新，是永不滚动的初始基线。
> ```
> 初始:  active=Home(出厂),  backup=无
> 改1:   active=Office,      backup=Home
> 改2:   active=Bad,         backup=Office   ← Home 已丢失
> ```
> 若 Office 与 Bad 都有问题，只有 factory 还能回到 Home。

### 4.8 expect_version —— 乐观锁

`config_set` / `config_delete` / `config_module` 都支持：

```json
{"c":"system","i":"1012","p":{"o":"config_set","module":"weight","key":"scale",
 "value":750,"expect_version":8}}
```

- 版本号匹配 → 正常执行
- 不匹配 → 拒绝，返回 `e:7`，并带上当前版本：
  ```json
  {"s":3,"e":7,"m":"version mismatch","data":{"current_version":9}}
  ```
- UI 据此提示"配置已变更，请刷新"后重新查询

**建议流程**：查询拿 version → 带 version 下发修改 → 失败则重新查询。

---

## 5. 错误码（上行 `e` 字段）

| 值 | 枚举 | 含义 | 处理建议 |
|---|---|---|---|
| 0 | `CONFIG_ERR_NONE` | 成功 | |
| 1 | `CONFIG_ERR_INVALID_PARAM` | 参数非法 | 检查 module/key/value |
| 2 | `CONFIG_ERR_UNAVAILABLE` | 存储不可用 | LittleFS 异常 |
| 3 | `CONFIG_ERR_MODULE_NOT_FOUND` | 模块不存在 | 检查模块名拼写 |
| 4 | `CONFIG_ERR_KEY_NOT_FOUND` | 字段不存在 | 查不到该字段 |
| 5 | `CONFIG_ERR_VALUE_REJECTED` | value 类型不被接受 | 类型不符 / 严格校验失败 |
| 6 | `CONFIG_ERR_IO_FAILED` | 文件 IO 失败 | 见串口日志 |
| 7 | `CONFIG_ERR_VERSION_MISMATCH` | 版本号不匹配 | 重新查询后重试 |
| 8 | `CONFIG_ERR_FACTORY_MISSING` | factory 副本不存在 | 先执行 config_backup |

## 6. 状态码（上行 `s` 字段，CloudStatus）

| 值 | 枚举 | 含义 |
|---|---|---|
| 0 | `STATUS_OK` | 成功 |
| 1 | `STATUS_ACCEPTED` | 已接受（异步第一阶段） |
| 2 | `STATUS_RUNNING` | 执行中 |
| 3 | `STATUS_FAILED` | 失败 |
| 4 | `STATUS_ERROR` | 协议错误（版本不匹配 / 非法命令） |

---

## 7. 本地 C++ API

### 7.1 生命周期

```cpp
bool config_init();                    // 初始化：挂载校验 + 加载全部模块 + 恢复判定
bool config_save();                    // 同步保存所有 dirty 模块（内部用；命令通道请用异步接口）
bool config_reload();                  // 丢弃 RAM 修改，从文件重新加载
```

### 7.2 同步读取（业务模块用）

```cpp
bool config_get_int   (const char* module, const char* key, int   &value);
bool config_get_long  (const char* module, const char* key, long  &value);
bool config_get_float (const char* module, const char* key, float &value);
bool config_get_bool  (const char* module, const char* key, bool  &value);
bool config_get_string(const char* module, const char* key, char *buf, size_t size);
bool config_get_json  (const char* module, JsonDocument &output);   // 深拷贝

uint8_t config_get_version(const char* module);   // 模块版本号
```

模块不存在/未加载时返回 `false`，**调用方必须使用默认值兜底**。

### 7.3 同步写入（仅限业务模块内部，如校准值）

```cpp
bool config_set_int   (const char* module, const char* key, int   value);
bool config_set_long  (const char* module, const char* key, long  value);
bool config_set_float (const char* module, const char* key, float value);
bool config_set_bool  (const char* module, const char* key, bool  value);
bool config_set_string(const char* module, const char* key, const char* value);
```

> **命令通道禁止使用这些接口**（会在 MQTT 回调里做文件 IO），必须走 7.4 的异步入队。

### 7.4 异步命令（CommandManager 用）

```cpp
// 底层统一入口
ConfigEnqueueResult config_cmd_enqueue(
    ConfigCommandType type, const char* cmd_id, const char* command,
    const char* object, const char* source, const char* module,
    const char* key, JsonVariantConst value, bool cancel,
    bool keys_only, int expect_version);

// 便捷封装
config_cmd_enqueue_query_field(cmd_id, command, object, source, module, key);
config_cmd_enqueue_query_module(cmd_id, command, object, source, module, keys_only);
config_cmd_enqueue_query_all(cmd_id, command, object, source, keys_only);
config_cmd_enqueue_set_field(cmd_id, command, object, source, module, key, value, expect_version);
config_cmd_enqueue_delete_field(cmd_id, command, object, source, module, key, expect_version);
config_cmd_enqueue_set_module(cmd_id, command, object, source, module, value, expect_version);
config_cmd_enqueue_save(cmd_id, command, object, source);
config_cmd_enqueue_restart(cmd_id, command, object, source, cancel);
config_cmd_enqueue_reset_module(cmd_id, command, object, source, module);
config_cmd_enqueue_backup_module(cmd_id, command, object, source, module);
```

入队结果：

| 返回值 | 含义 |
|---|---|
| `CONFIG_ENQUEUE_ACCEPTED` | 已入队 |
| `CONFIG_ENQUEUE_QUEUE_FULL` | 队列满（容量 8） |
| `CONFIG_ENQUEUE_INVALID` | 参数非法 / value 类型不支持 / 超长 |
| `CONFIG_ENQUEUE_UNAVAILABLE` | 存储不可用 |

### 7.5 回调注册

```cpp
void config_set_completion_callback(ConfigCommandCompletionCallback cb);
void config_set_log_callback(ConfigLogCallback cb);
```

完成回调签名：
```cpp
void (*)(const char *cmd_id, const char *command, const char *object,
         const char *source, ConfigCommandType type, bool success,
         ConfigCommandError error, const JsonDocument &result,
         const char *message);
```

> **生命周期**：`result` 与 `message` 仅在回调期间有效，
> 需留存必须深拷贝（CommandManager 用 `doc["data"].set(...)` 实现）。

### 7.6 主循环

```cpp
void config_task();    // 在 loop() 中调用，每次处理一个队列任务
```

### 7.7 其他

```cpp
size_t   config_cmd_queue_capacity();              // 队列总容量
size_t   config_cmd_queue_pending();                // 待执行任务数
bool     config_restart_pending(unsigned long &remain_ms);
void     config_cancel_restart();
ConfigCommitState config_commit_state();            // 最近一次提交状态
bool     config_release_module(const char* module); // 释放模块 RAM（慎用）
```

---

## 8. 异步模型

```
MQTT RX
  ↓
CloudManager（翻译：c/i/p → CommandMessage）
  ↓
CommandManager::command_manager_execute
  ↓  system_router → command_config_xxx
  ↓  ① command_runtime_insert（占槽位，cmd_id 去重）
  ↓  ② config_cmd_enqueue_xxx（只入队，深拷贝参数）
  ↓  ③ 上报 accepted
  ↓  【立即返回，不阻塞】
  ↓
主循环 config_task()
  ↓  取一个任务 → 执行文件 IO（同步，毫秒级）
  ↓
ConfigCommandCompletionCallback
  ↓
CommandManager::command_config_completion
  ↓  按 cmd_id 找回 runtime → 生成 result → 上报
  ↓  释放 runtime 槽位
```

**严禁**在 MQTT 回调中直接调用 `config_save()` 或 `ESP.restart()`。

---

## 9. 可靠性机制

### 9.1 原子写

写临时文件 → 完整写入 → 大小校验 → `rename` 原子替换。禁止直接覆盖。

### 9.2 Backup 回滚

保存顺序：`Active → Backup` → 写新 Active。
任何时刻至少有一份完整数据；写入失败则 Active 缺失、Backup 完好，下次加载自动回退。

**恢复不消耗 Backup**（读 Backup 内容重新原子写入 Active），可连续恢复多次。

### 9.3 commit 三态（`/config/.commit`）

文件内容为状态字符串（**不是"存在/不存在"**）：

| 内容 | 含义 | 启动时行为 |
|---|---|---|
| `PENDING` | 提交进行中，未确认完成 | **恢复** |
| `DONE` | 提交完整成功 | 不恢复 |
| `FAILED` | 提交失败且已回滚 | 不恢复（状态已一致） |

用内容而非存在性，是因为"存在=异常"无法表达"最后一步（写 DONE）失败"。

### 9.4 Boot Validation

`/config/.bootok`：每次 `config_init()` 删除，完整走完 setup 后写入。
标记缺失 = 上次启动未成功 → 从 Backup 恢复。

> ⚠️ **烧录固件 = 非正常关机，会触发恢复**。
> 烧录前先发 `config_save` 可保留最新配置。

### 9.5 重启前自动保存

5 分钟倒计时到点、主动 `config_restart`，都会先 `config_save()` 再重启，
避免未保存的修改丢失。保存失败也照常重启（回滚到上一份可信配置同样是安全结果）。

### 9.6 关键常量

| 常量 | 值 | 说明 |
|---|---|---|
| `CONFIG_CMD_QUEUE_SIZE` | 8 | 命令队列容量 |
| `CONFIG_RESTART_TIMEOUT_MS` | 300000（5 分钟） | 改配置后自动重启倒计时 |
| `CONFIG_RESTART_SAFE_DELAY_MS` | 10000（10 秒） | 重启命令后给 MQTT 留的发送窗口 |
| `CONFIG_VALUE_STRING_MAX` | 96 | value 定长缓冲 |
| `CONFIG_VALUE_BIG_MAX` | 8192 | 大 value（整模块）PSRAM 上限 |
| `CONFIG_CMD_ID_MAX` | 32 | 命令 ID 长度上限 |
| `CONFIG_KEY_NAME_MAX` | 32 | 字段名长度上限 |
| 模块版本号 | uint8_t 循环递增 | 仅用于判断新旧 |

---

## 10. 开发规范（必修，踩过的坑）

### 10.1 ⚠️ 禁止把定长数组直接传给 ArduinoJson

```cpp
// ❌ 错误：会按【数组大小 N-1】匹配，而不是字符串实际长度
JsonVariantConst v = data[t.key];        // char key[32] → 按长度 31 查 "ssid"(4) → 永远查不到

// ✅ 正确：先退化成指针，走 strlen
const char *key_ptr = t.key;
JsonVariantConst v = data[key_ptr];
```

原因（`ArduinoJson/Strings/Adapters/RamString.hpp`）：
```cpp
template <size_t N>
struct StringAdapter<const char (&)[N]> {
  static AdaptedString adapt(const char (&p)[N]) {
    return RamString(p, N - 1, true);        // ← 长度取 N-1！
  }
};
// 指针版本才用 strlen
```

**这个坑不报错、不告警，只静默失配。**

### 10.2 ⚠️ 联合体缓冲区必须先清零

```cpp
// ❌ 错误：尾部残留栈上旧数据，会被一起写进配置（表现为乱码）
memcpy(out.v.s, src, len + 1);

// ✅ 正确
memset(out.v.s, 0, sizeof(out.v.s));
memcpy(out.v.s, src, len);
```

### 10.3 ⚠️ PSRAM 用后必须立即释放

大 value（>96 字节的整模块 JSON）在 PSRAM 分配，**任务执行完立即释放**：

```cpp
value_slot_free(t.value);    // 在 process_one 中，回调之前
```

三条释放路径都已覆盖：任务执行完 / 入队失败 / 内部序列化失败。
新增代码若涉及 `heap_caps_malloc`，必须确保每个出口都释放。

### 10.4 其他

- 非 const `JsonDocument::operator[]` 返回 `MemberProxy`（get-or-create 语义），
  **只读查询要用 const 引用**，否则可能往配置插入 null 成员
- 版本号不要放进 `data` 内部，否则保存时会被写进配置文件
- 字符串复制一律用 `copy_str()`（有长度检查，超长拒绝而非截断）
- 路径构造用 `snprintf` 并检查返回值

---

## 11. 完整测试脚本

> 每个命令的 `i` 必须不同。

```json
// 1. 查询模块名
{"c":"system","i":"1001","p":{"o":"config_query","keys_only":true}}
// 期望: data.modules = ["wifi","oled","valve","time","weight","mqtt","mi_thermo"]

// 2. 查询模块字段名
{"c":"system","i":"1002","p":{"o":"config_query","module":"weight","keys_only":true}}
// 期望: data.keys = [...], data.version = N

// 3. 查询单字段
{"c":"system","i":"1003","p":{"o":"config_query","module":"weight","key":"scale"}}
// 期望: data = {module,key,value}, version = N

// 4. 乐观锁（故意用错版本，应被拒绝）
{"c":"system","i":"1004","p":{"o":"config_set","module":"weight","key":"scale","value":750,"expect_version":99}}
// 期望: s:3, e:7, data.current_version = 实际版本

// 5. 正确版本修改
{"c":"system","i":"1005","p":{"o":"config_set","module":"weight","key":"scale","value":750,"expect_version":<实际版本>}}
// 期望: s:1(accepted) → s:0(success)

// 6. 批量写模块（合法字段）
{"c":"system","i":"1006","p":{"o":"config_module","module":"weight","value":{"dt":6,"sck":7,"sample_interval":50,"scale":741,"zero_offset":-800750,"filter_samples":10}}}
// 期望: s:0 "module updated, restart required"

// 7. 批量写模块（含不存在字段，应整体拒绝）
{"c":"system","i":"1007","p":{"o":"config_module","module":"weight","value":{"dt":6,"typo_field":1}}}
// 期望: s:3, e:5, 串口打印 [CFG] module write rejected, unknown field: [typo_field]

// 8. 保存
{"c":"system","i":"1008","p":{"o":"config_save"}}
// 期望: data = {saved:true, dirty:false}

// 9. 备份到 factory
{"c":"system","i":"1009","p":{"o":"config_backup","module":"weight"}}
// 期望: s:0，串口可能出现 [CFG][I] factory dir created: /factory

// 10. 改乱后从 factory 恢复
{"c":"system","i":"1010","p":{"o":"config_set","module":"weight","key":"scale","value":1234}}
{"c":"system","i":"1011","p":{"o":"config_reset","module":"weight"}}
// 期望: reset 返回 s:0；重启后 scale 回到备份时的值

// 11. 删除字段
{"c":"system","i":"1012","p":{"o":"config_delete","module":"weight","key":"skc"}}
// 期望: s:0；再次查询该字段 → e:4 key not found

// 12. 重启（先自动保存，10 秒后重启）
{"c":"system","i":"1013","p":{"o":"config_restart"}}
```

验证文件状态：重启后 `/config/weight.json` = 新值、
`/config/weight.json.bak` = 旧值、`/config/version.json` 递增、`/config/.commit` = `DONE`。

---

## 12. 已知边界

1. **烧录固件会触发 Backup 恢复**（Boot Flag 机制无法区分"计划内烧录"与"崩溃"）。
   缓解：烧录前先 `config_save`。

2. **`config_release_module()` 会置 `loaded=false`**，之后若发生写入，
   `set_begin` 会先尝试从文件加载再修改（已修复早期"直接 clear 丢配置"的问题）。

3. **不支持运行时新增模块**（模块集合编译期固定）。

4. **不做整体配置导入**（断网/断电/内存风险与收益不对等）。
   导出能力由 `config_query` + version 提供；导入只做单模块替换。

5. **ConfigManager 直接调用了 `LittleFS.mkdir()`**（JsonStorage 未暴露 mkdir 接口）。
   属于跨层权宜实现，建议后续把 `json_storage_mkdir()` 收进 JsonStorage。

6. **消息长度**：巴法云限制 1024 字节，超长会触发分片。
   查全部模块时 `data` 可能接近上限，建议用 `keys_only` + 按需单模块查询。

7. **系统级资源查询**（Flash 容量、文件目录、RAM、重启等）不属于本模块，
   应另立 system command 模块。
