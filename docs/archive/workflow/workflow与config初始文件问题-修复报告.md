# ConfigManager version 初始化 & Workflow 初始 BIN — 修复报告

**日期**：2026-09-13
**硬件**：ESP32-S3 N16R8（COM8）
**基线 commit**：`139e48d` → 本次提交 `17b8823`、`23889ac`
**结论**：`[CFG][E] version reload failed` **已消除**；Workflow 初始 BIN 基线已建立并验证。

---

## 1. 问题现象

上机串口每次启动都打印：

```
[CFG][E] version reload failed
[CFG][I] init done, loaded 8/8 modules
```

---

## 2. 根因（如何产生的）

### 2.1 直接原因：烧录镜像里没有 version 文件

`data/config/`（uploadfs 的源目录）只有 8 个模块 JSON，**没有 `version.json`**：

```
data/config/  mi_thermo.json  mqtt.json  oled.json  rtc.json
              time.json  valve.json  weight.json  wifi.json
```

而 `config_init()` 步骤 3 无条件调用 `load_version_file()`：

```cpp
// src/config_manager.cpp（修复前）
if (!load_version_file())
{
    cfg_log("E", "version reload failed");
}
```

`load_version_file()` 在 `json_storage_read("/config/version.json")` 失败时
直接 `return false`。文件不存在 → 打 E 级日志，所有模块 `version` 停在 **0**（"未知"）。

底层证据（ESP-IDF VFS 直接报文件不存在）：

```
[E][vfs_api.cpp:105] open(): /littlefs/config/version.json does not exist, no permits for creation
```

### 2.2 关键：为什么它永远不会自愈

`save_version_file()` 里有一段**正确但致命**的保护：

```cpp
// version == 0 表示"版本未知"（例如 version.json 读取失败）。
// 不能用未知的 0 覆盖文件里已记录的版本 ——
// 否则一次读取失败就会抹掉所有模块的版本历史。
if (s_modules[i].version == 0)
{
    continue;
}
```

于是形成死循环：

```
version.json 缺失
      ↓
所有模块 version = 0
      ↓
config_save → save_version_file() 把 8 个模块【全部跳过】
      ↓
version.json 依然不存在
      ↓
下次启动再次 [CFG][E] version reload failed
```

**只有某个模块被真正修改（version++ ≥ 1）后，文件才会被创建。**
在此之前，这条 E 级日志每次启动都会出现。

### 2.3 实际影响

| 影响 | 说明 |
|---|---|
| 日志噪声 | E 级，容易被误判为硬件/Flash 故障 |
| **乐观锁失效** | `config_get_version()` 恒返回 0，`expect_version` 在文件生成前形同虚设 |
| UI 显示异常 | 首次烧录后所有模块版本号显示为 0 |

---

## 3. 修复（如何修复的）

### 3.1 代码自愈（根本修复）—— `17b8823`

新增 `bootstrap_version_file()`：为**已加载**的模块建立版本基线（=1）并落盘。

```cpp
static bool bootstrap_version_file()
{
    bool any = false;
    for (int i = 0; i < CONFIG_MODULE_COUNT; i++)
    {
        if (!s_modules[i].loaded)      continue;   // 未加载的不污染"未知"语义
        if (s_modules[i].version == 0) s_modules[i].version = 1;
        any = true;
    }
    if (!any) return false;
    return save_version_file();
}
```

接入三个调用点：

| 位置 | 处理 |
|---|---|
| `config_init()` | 步骤 3 失败只记 **W** 并置 `version_ok=false`；**步骤 5 模块加载完成后**新增步骤 6 执行自愈 |
| `config_reload()` | 同样改为"先记状态 → 模块重载后自愈" |
| `commit_recover_from_backup()` | 该路径模块已重载完毕，直接自愈 |

> ⚠️ **顺序是关键**：模块在步骤 5 才加载。若把自愈放在步骤 3，
> 此时 `loaded` 全为 `false`，一个模块都处理不了，`any=false` 直接返回失败。

日志级别调整：首次烧录 / 文件损坏属**预期场景**，由 `E` 降为 `W`；
只有**连写入都失败**时才打 `E`。

### 3.2 数据基线 —— `data/config/version.json`

```
{"mi_thermo":1,"mqtt":1,"oled":1,"rtc":1,"time":1,"valve":1,"weight":1,"wifi":1}
```

随 uploadfs 一起烧录，使版本号**首次启动即就位**，不依赖自愈。

### 3.3 可复现的生成脚本 —— `23889ac`

`data/` 被 `.gitignore` 忽略（含 WiFi 密码 / CA 证书），初始文件不在版本库中。
因此提供两个脚本，clone 后可一键重建：

| 脚本 | 作用 |
|---|---|
| `tools/gen_config_version.py` | 扫描 `data/config/*.json` → 生成 `version.json`（新增模块自动纳入） |
| `tools/gen_workflow_bin.py` | 从 `data/workflow.json` 生成 `data/workflow/` 初始 BIN |

### 3.4 Workflow 初始 BIN

**先回答"烧录会不会清除板载文件"**：

| 烧录方式 | 是否清除 LittleFS |
|---|---|
| `esptool write_flash 0x10000 firmware.bin`（日常刷固件） | **否**，只写 app 分区 |
| `pio run -t uploadfs` | **是**，整分区擦除 |

实测 uploadfs 输出：

```
Flash will be erased from 0x00410000 to 0x00ffffff...
Wrote 12517376 bytes at 0x00410000
```

因此**跑 uploadfs 会清空 `/workflow/*.bin` 与 `/config/*`**。
按需求生成了初始 BIN 放进镜像：

```
data/workflow/meta.bin            1436 B  (12B header + 16×89B entry, Meta v3)
data/workflow/wf00..wf02/step*.bin        各 4 步
```

格式严格对齐 `src/workflow_storage.cpp`（CRC32 init `0xFFFFFFFF` /
poly `0xEDB88320` reflected / final xor `0xFFFFFFFF`；
`meta.entry.crc32` = 各 step payload CRC 依次 u32 LE 追加后的 CRC）。

---

## 4. 上板验证

### 4.1 修复前（旧固件）

```
[04:45:33.995] [E][vfs_api.cpp:105] open(): /littlefs/config/version.json does not exist
[04:45:33.995] [CFG][E] version reload failed
[04:45:33.995] [CFG][I] init done, loaded 8/8 modules
```

### 4.2 场景 A：镜像【不含】version.json → 自愈路径

> 用 `esptool --after no_reset` 烧 FS 后再开串口，
> 才能捕获到"第一次启动"——否则脚本开串口会复位，自愈发生在未被捕获的那次。

```
[05:13:45.047] [CFG][W] version file missing or corrupt, will rebuild
[05:13:45.047] [CFG][W] recovering (boot_ok=0, commit_state=0)
[05:13:45.482] [CFG][I] init done, loaded 8/8 modules
[05:13:45.483] [CFG][I] version file created (baseline)     ← 自愈成功
[05:13:47.555] [OK] Workflow loaded from Flash BIN           ← BIN 基线生效
[05:13:47.887] [CFG][I] boot validated
```

**`[CFG][E]` 计数 = 0。**

### 4.3 场景 B：镜像【含】version.json → 数据基线路径

```
[05:18:29.781] [CFG][I] init done, loaded 8/8 modules
[05:18:32.009] [OK] Workflow loaded from Flash BIN
[05:18:32.103] [CFG][I] boot validated
```

无 W、无 E。`config_query`：

```json
"versions":{"wifi":1,"oled":1,"valve":1,"time":1,"weight":1,"mqtt":1,"mi_thermo":1,"rtc":1}
```

### 4.4 BIN 内容正确性（不只是 meta）

`workflow.get p.id=1`：

```json
{"id":"daily_valve_test1","name":"每日开阀测试1","variant":1,"enable":true,
 "timeout_ms":60000,"steps":[
   {"type":"trigger","id":"timer_daily","params":{"hour":7,"minute":0}},
   {"type":"action","id":"VALVE_OPEN"},
   {"type":"trigger","id":"delay","params":{"seconds":30}},
   {"type":"action","id":"VALVE_CLOSE"}]}
```

步骤类型、参数、中文名（UTF-8）全部正确往返。断言 **4/4**，0 崩溃。

### 4.5 镜像内容字节扫描（决定性证据）

```
version.json     NOT in image   ← 场景 A 确实没烧进去，证明自愈路径被真实走到
workflow.json    IN IMAGE
meta.bin         IN IMAGE
step00.bin       IN IMAGE
```

---

## 5. 修改文件清单

| 文件 | 改动 | Commit |
|---|---|---|
| `src/config_manager.cpp` | 新增 `bootstrap_version_file()` + 前向声明；`config_init()` 步骤 3 改 W + 新增步骤 6；`config_reload()` 同步；`commit_recover_from_backup()` 同步 | `17b8823` |
| `tools/gen_config_version.py` | 新增：生成 `version.json` | `23889ac` |
| `tools/gen_workflow_bin.py` | 新增：从 workflow.json 生成初始 BIN | `23889ac` |
| `config_version与workflow初始bin-进度.md` | 新增：进度台账 | 本次 |
| 本文件 | 新增：修复报告 | 本次 |

**未入库（data/ 被 gitignore）但在本地已生成**：
`data/config/version.json`、`data/workflow/meta.bin`、`data/workflow/wf00..wf02/*.bin`

用户的 5 个文档改动全程未触碰（`git diff` 对这些文件为 0）。

---

## 6. 后续注意事项

1. **每次跑 `uploadfs` 前**：确认 `data/config/version.json` 与 `data/workflow/`
   已生成（或跑一次 `tools/` 下两个脚本），否则版本号会被清空 ——
   虽然设备能自愈，但那次启动会多一条 W。
2. **若 `WF_STG_META_VERSION` 升级（v3 → v4）**：必须同步更新
   `tools/gen_workflow_bin.py` 的 `WF_STG_META_VERSION`，否则设备会以
   `VERSION_TOO_NEW` 拒绝，自动回退到 `/workflow.json`（功能不丢，但 BIN 基线失效）。
3. **日常刷固件不会清 LittleFS**，无需每次重建 data/。
4. 建议在 `config_backup`（写 factory 副本）之后再执行 uploadfs，
   以便 uploadfs 后仍能从 factory 恢复。
