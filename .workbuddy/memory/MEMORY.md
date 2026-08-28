# 项目长期笔记：锅氏自动猫粮机（Guo Feeder Project）

## 项目定位
ESP32-S3 N16R8 智能宠物供水/投喂设备，当前主攻「自动猫咪饮水系统」：水箱 + 重力供水 + 电磁阀 + HX711 称重。核心目标：按重量变化精确控水，异常时不持续漏水。当前 v0.7。

## 关键文档（改架构必须同步更新）
- `readme.md`：架构总纲、编码规范、MQTT 协议、System State / Trigger 注册规范
- `需求文档.md`：V2.1，8 项待开发计划 + 推荐顺序
- `config manager开发架构.md`：**Config 存储架构定版，当前行动基线**

## 软件架构（四层）
应用/能力层（Valve / Weight / DispenseGuard / Mijia / OLED）→ 自动化层（Workflow Manager + Capability Registry）→ 服务层（System State / Config / Event / Time / WiFi / Command / Log）→ 云通信层（Cloud Manager / MQTT）。

## 硬性编码规范
- 零全局裸变量，状态统一走 System State（`state_set_xxx` / `state_get_xxx`）
- 全程非阻塞：禁 `delay()`、禁 while 死等
- 模块单一职责，禁止跨模块直接读写内部状态，数据单向流动
- Action / Trigger 生命周期 `reset → start → poll → SUCCESS/FAILED`，状态放 `inst->runtime`，禁 static
- 新增 System State 必改三处：`system_state.h` 枚举（STATE_MAX 前）、`.cpp` state_map、产生模块
- src 只留有效代码，测试代码进 backup，一模块一 .cpp/.h

## Config 存储架构（已定版，写代码以此为准）
分层：**业务模块管"参数是否合法" → ConfigManager 管"配置" → JsonStorage 管"文件" → LittleFS**。
- ConfigManager：load / get / set / save / backup / recovery / version / cloud command / boot validate
- JsonStorage（已实现）：exists / size / remove / rename / read / write / write_atomic / read_chunk / write_chunk + 轻量流式接口；不理解任何业务语义。backup/recovery 由 ConfigManager 用 rename 实现，JsonStorage 不提供
- **双版本**：永远保留 Active + Backup（最新 + 上一个），版本 uint8_t 递增循环，不依赖时间戳（启动期时间不可信）
- **原子写**：写临时文件 → 完整写入 → 旧 Active 转 Backup → 临时文件转 Active，禁止直接覆盖
- **Boot Validation**：setup 全部模块初始化完成后才标记 Config Valid；未到达则配置可疑，下次启动走 Backup Recovery
- **文件组织**：按模块拆分（`/config/wifi.json`、`oled.json`...），Workflow 拆为 `/workflow/workflow_N.json`（最多 16 个）
- **版本独立**：`config_version.json` 单独管版本，业务 JSON 保持纯数据
- **云端修改**：config_set → 原子保存 → Active/Backup 滚动 → 重启生效（不做局部 reload / 实时生效）
- **Flash 优化**：set 时比对旧值，无变化不写盘
- **大文件**：一律分块流式（FILE_BEGIN / CHUNK / END），禁止整文件 malloc 进 RAM
- 明确不做：业务参数合法性判断、debug 开关（走编译期宏）、实时通知配置变化

## 当前开发顺序（Config 专项）
① JsonStorage（**已完成**：`src/json_storage.h/.cpp`，编译通过，待上板验收）→ ② ConfigManager → ③ Workflow JSON 存储改造 → ④ Config 云端 Command → ⑤ Log Manager

### JsonStorage 实现要点
- init 用 `LittleFS.totalBytes()==0` 探测挂载，不重复 begin（main.cpp:31 已挂载）
- 原子写 = tmp 完整写入 + 大小校验 + `rename(tmp,path)`，失败清理 tmp，不生成 backup
- write_chunk：offset==0 用 `"w"` 截断，offset>0 用 `"r+"` 且拒绝越界 offset
- read 有 32KB 上限；不依赖 ArduinoJson；日志走 `JsonStorageLogCallback` 预留

### 环境注意（PlatformIO）
- 沙箱 `safe-delete` 拦截删除 `.o`/`.elf`，**增量编译必然失败**（SAFE_DELETE_FAIL_CLOSED），非代码问题
- 解法：`PLATFORMIO_BUILD_DIR=.pio/build/xxx pio run`（PlatformIO 6 无 `--build-dir`），全量约 130s
- board 识别为 N8、**PSRAM 未启用**，实际硬件 N16R8

## 开发计划（需求文档 V2.1）
1. Config 模块完善（见上，拆为 JsonStorage + ConfigManager）
2. WiFi 初始化优化　3. Cloud Protocol 文档同步　4. 系统内置命令
5. Log 日志模块（分级 + Ring Buffer + 串口 / MQTT）　6. 米家温湿度定版
7. RTC 芯片支持（横切影响 Workflow 定时 / Event 时间 / Cloud 时间字段）
8. 按键 + OLED UI（按键走事件队列，统一 CommandManager 入口）

## 冲突与依赖
- 协议冲突：README 记 `{cmd,ob,id,pl,src,ts}`，Cloud Manager 已重构为 `{c,i,v,k,p}`；以代码 + 需求文档为准
- Config 是全局底座：米家参数、Log 等级、RTC 参数都要进 Config
- 需求文档聚焦饮水系统；README 含投喂 / 出粮 / App 等远期目标，排期以需求文档为准
