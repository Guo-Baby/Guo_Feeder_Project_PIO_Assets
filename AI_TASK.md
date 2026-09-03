# TimeManager V2：PCF8563T RTC + ESP-IDF SNTP 升级需求文档

## 一、升级目标

在保持现有 TimeManager 对外接口和系统行为兼容的前提下，为 TimeManager 增加 PCF8563T 外部 RTC 支持，并将现有自行实现的 NTP 获取逻辑替换为 ESP-IDF 官方 SNTP 机制。

本次升级遵循：

1. 最小侵入现有架构。
2. 保留现有 `SystemState.time_valid` Boolean，不修改其类型和现有使用方式。
3. 不新增对外的 `TIME_RTC / TIME_NTP` 可信度等级接口。
4. TimeManager 内部可以记录当前时间来源用于日志和调试，但不影响现有公共接口。
5. 不自行实现 NTP 协议。
6. 使用 ESP-IDF 提供的 SNTP 能力。
7. 使用 ESP-IDF I2C Master Driver 与 PCF8563T 通信。
8. 不实现 RTC 漂移估算。
9. RTC 是本地持久化时间来源，NTP 是更高可信度的校准来源。
10. 保持现有 TimeManager 的时区、DST 和对外统一时间接口。

---

# 二、时间来源和可信度模型

系统实际存在两个时间来源：

### 1. RTC

PCF8563T：

* 本地持久化时间来源。
* 设备断电后仍可继续计时。
* 用于设备启动时快速恢复 System Time。
* RTC 本身不作为最终最高可信度时间来源。

### 2. SNTP/NTP

网络时间：

* 设备连接 Wi-Fi 后使用 ESP-IDF SNTP 获取。
* 作为更高可信度的时间来源。
* 每次成功 SNTP 同步后，以同步后的 System Time 为准。
* 必要时使用该时间校准 RTC。

---

# 三、SystemState.time_valid

必须保留现有：

```cpp
bool time_valid;
```

禁止将其修改为 enum、字符串或其他类型。

现有时间有效性规则继续保持：

```text
System Time >= 2026-07-01
        ↓
time_valid = true
```

否则：

```text
time_valid = false
```

TimeManager 负责根据当前 System Time 的有效性更新 `SystemState.time_valid`。

RTC 或 NTP 都可以使 System Time 进入有效范围。

TimeManager 内部可以维护时间来源状态，例如用于日志：

```text
INVALID
RTC
NTP
```

但该状态不得替换或修改现有 `SystemState.time_valid` 公共接口。

其他模块继续使用：

```cpp
system_state.time_valid
```

进行原有 Boolean 判断，不要求任何现有调用方修改。

---

# 四、启动时 RTC 恢复

TimeManager 初始化后，应尽早尝试读取 PCF8563T。

流程：

```text
TimeManager init
    ↓
RTC Read
    ↓
判断 RTC 时间是否有效
```

### RTC 有效

如果 RTC 时间有效：

```text
RTC → System Time
```

直接使用硬同步方式设置 System Time。

此时：

```text
time_valid = true
```

不等待 Wi-Fi，也不等待 SNTP。

设备可以立即获得一个可用的时间。

### RTC 无效

如果 RTC 无效：

```text
不修改 System Time
time_valid = false
```

等待 Wi-Fi 和 SNTP。

RTC 无效本身不是系统错误，不得导致设备异常或阻塞系统启动。

---

# 五、RTC 无效的处理原则

RTC 无效时不需要尝试复杂恢复。

直接：

```text
RTC invalid
    ↓
time_valid = false
    ↓
等待 SNTP
```

连接 Wi-Fi 后：

```text
SNTP 成功
    ↓
System Time 有效
    ↓
time_valid = true
```

之后再通过正常的 RTC 校准流程尝试向 RTC 写入正确时间。

如果 RTC Write 失败：

* 不影响当前 System Time。
* 不将 `time_valid` 设置为 false。
* 不认为 SNTP 失败。
* 等下一次 SNTP 同步时重新尝试。

---

# 六、SNTP 实现

禁止继续使用现有自行实现的 NTP UDP 请求、NTP 数据包解析以及手工计算服务器时间的代码。

改用 ESP-IDF 官方 SNTP/esp_netif SNTP API。

优先使用：

```text
esp_netif_sntp_init()
esp_netif_sntp_start()
```

以及 ESP-IDF 提供的同步事件/状态机制。

TimeManager 负责：

* SNTP 初始化。
* SNTP 启动。
* NTP Server 配置。
* SNTP 同步状态管理。
* 同步完成后的后续 RTC 校准。
* 定期同步策略。

ESP-IDF 负责：

* NTP 协议。
* 网络通信。
* 系统时间同步。

禁止 TimeManager 自己重复实现 NTP 协议。

---

# 七、SNTP 同步周期

正常运行状态下：

```text
每 24 小时进行一次 SNTP 校时。
```

不需要进行 RTC 漂移率计算。

不需要根据 RTC 漂移速度动态调整 SNTP 周期。

24 小时作为固定周期。

系统时间的长期精度主要依赖网络 SNTP 校准。

---

# 八、NTP 校时后的 System Time

SNTP 成功后，系统时间以 ESP-IDF SNTP 同步后的 System Time 为准。

禁止：

```text
自己读取 NTP Server timestamp
→ 自己计算网络延迟
→ 自己 settimeofday()
```

应让 ESP-IDF SNTP 完成系统时间同步。

TimeManager 在 SNTP 同步完成后读取当前 System Time，作为后续 RTC 校准的基准。

---

# 九、System Time 同步策略

### 开机 RTC → System Time

使用硬同步。

原因：

开机时 System Time 可能仍然处于 1970 年附近，属于明显无效状态，不需要 Smooth Sync。

### 有效 System Time → SNTP

使用 ESP-IDF Smooth Sync 机制。

例如：

```text
RTC/System Time = 12:00:00
NTP 校准时间   = 12:00:04
```

允许 ESP-IDF 平滑调整 System Time。

TimeManager 不自行实现平滑校时算法。

---

# 十、SNTP 同步完成后的 RTC 校准

每次 SNTP 成功同步后，都必须进行 RTC 校准检查。

流程：

```text
SNTP Sync Success
       ↓
读取当前 System Time
       ↓
读取 RTC
       ↓
计算时间差
```

定义：

```text
difference = abs(System Time - RTC)
```

### 差值 ≤ 2 秒

```text
不写 RTC
```

原因：

避免没有必要的 RTC 写操作。

### 差值 > 2 秒

```text
System Time → RTC
```

将当前可信的 System Time 写入 RTC。

注意：

这里的写入目标必须是**当前 System Time**，而不是直接使用 SNTP Server 原始 timestamp。

---

# 十一、RTC Write 失败处理

RTC Write 失败属于可恢复错误。

如果：

```text
RTC Write Failed
```

则：

1. 保留当前 System Time。
2. 保留 `time_valid = true`（如果当前 System Time 已经有效）。
3. 记录错误日志。
4. 不进行连续快速重试。
5. 等待下一次 24 小时 SNTP 同步。
6. 下一次 SNTP 同步后再次比较 RTC 和 System Time。
7. 如果差值仍然 > 2 秒，再次尝试 RTC Write。

不得因为 RTC Write 失败而让整个 TimeManager 进入故障状态。

---

# 十二、RTC Read 与 RTC Write 的 Critical Operation

RTC Read：

```text
非 Critical Operation
```

RTC Write：

```text
Critical Operation
```

RTC Write 前：

```cpp
system_command_critical_operation_acquire()
```

RTC Write 完成后：

```cpp
system_command_critical_operation_release()
```

无论 RTC Write 成功还是失败，都必须保证 Acquire/Release 配对。

RTC Write 的 Critical Operation 生命周期仅覆盖实际写入操作，不应覆盖普通 RTC Read。

---

# 十三、PCF8563T I2C

PCF8563T 使用标准 I2C。

本项目只需要：

```text
SDA
SCL
```

两个 ESP32 GPIO。

以下引脚第一版不使用：

```text
INT
CLKOUT
```

连接：

```text
ESP32 SDA → PCF8563 SDA
ESP32 SCL → PCF8563 SCL
ESP32 GND → PCF8563 GND
电源       → PCF8563 VCC
```

使用 ESP-IDF I2C Master Driver。

禁止自行 bit-bang I2C。

---

# 十四、PCF8563T 与 OLED 共用 I2C

PCF8563T 可以与现有 OLED 共用同一组 SDA/SCL。

推荐结构：

```text
ESP32-S3 I2C Bus
    │
    ├── OLED
    │
    └── PCF8563T
```

不要因为增加 RTC 而额外占用两个 GPIO。

I2C Bus 上通过不同 I2C Address 区分 OLED 和 RTC。

实现时必须避免：

* 重复初始化同一个 I2C Controller。
* OLED 和 RTC 分别创建互相冲突的 I2C Bus。
* 一个模块初始化 Bus 后另一个模块再次破坏该 Bus。

优先复用现有 OLED 使用的 I2C Bus。

如果当前项目的 OLED 驱动结构不支持复用，则在最小改动范围内解决，不进行无必要的大规模架构重构。

---

# 十五、I2C GPIO 配置

SDA/SCL GPIO 必须支持通过 ConfigManager 配置。

配置写入：

```text
config.json
```

不得把 GPIO 永久硬编码在 TimeManager.cpp 中。

具体字段命名应遵循当前项目 ConfigManager 的命名规范。

至少需要支持：

```text
RTC SDA GPIO
RTC SCL GPIO
```

如果最终确认 RTC 与 OLED 共用同一 I2C Bus，优先考虑将 SDA/SCL 作为 I2C Bus 配置，而不是让每个 I2C Device 各自拥有一份重复的 GPIO 配置。

不要为了本次 RTC 改造大幅修改现有 ConfigManager 架构。

---

# 十六、Time Zone / DST

继续保留现有 TimeManager 的：

* Time Zone
* DST
* 本地时间转换
* Unix Timestamp
* 人类可读时间转换

RTC 与 System Time 均建议使用 UTC/Unix Time 作为内部基准。

Time Zone 和 DST 只用于：

```text
UTC → Local Time
```

禁止将本地时间直接写入 RTC 作为 RTC 内部时间基准。

这样可以避免修改 Time Zone 或 DST 后 RTC 时间整体发生错误偏移。

---

# 十七、TimeManager 对外接口

保持现有 TimeManager 对外统一时间接口。

其他模块不应该：

* 直接访问 PCF8563T。
* 直接访问 SNTP。
* 直接 `settimeofday()`。
* 直接修改 RTC。
* 自己判断 RTC 是否有效。

所有时间相关操作统一经过 TimeManager。

---

# 十八、离线运行

如果设备长期没有 Wi-Fi：

### RTC 有效

```text
RTC → System Time
        ↓
设备继续正常运行
```

System Time 保持运行。

即使 RTC 长期没有 NTP 校准，也不进行漂移估算。

### RTC 无效

```text
RTC invalid
    ↓
time_valid = false
```

直到：

```text
SNTP 成功
```

才恢复：

```text
time_valid = true
```

如果设备长期离线且用户通过未来的本地 OLED/UI 手动修改时间，则使用现有 TimeManager 的统一时间设置接口修改 System Time，并按设计需要同步 RTC。

---

# 十九、错误边界

必须明确以下行为：

| 情况                    | 系统行为                        |
| --------------------- | --------------------------- |
| RTC Read 成功且时间有效      | RTC → System Time           |
| RTC Read 失败           | time_valid=false，等待 SNTP    |
| RTC 时间无效              | time_valid=false，等待 SNTP    |
| Wi-Fi 未连接             | 使用 RTC/System Time 继续运行     |
| SNTP 失败               | 保留当前 System Time，不影响已经有效的时间 |
| SNTP 成功               | 以 SNTP 校准后的 System Time 为准  |
| RTC 与 System Time ≤2秒 | 不写 RTC                      |
| RTC 与 System Time >2秒 | System Time → RTC           |
| RTC Write 失败          | 保留 System Time，下次 SNTP 再试   |
| RTC Write 成功          | RTC 被校准                     |
| I2C RTC 临时异常          | 不影响当前 System Time           |
| 长期无 Wi-Fi             | 不做漂移估算                      |
| 用户手动修改时间              | 通过 TimeManager 统一接口处理       |

---

# 二十、不要实现的功能

本次升级明确禁止加入以下复杂机制：

1. 不实现 RTC 漂移率估算。
2. 不实现 RTC 自动老化补偿。
3. 不建立 `TIME_INVALID / TIME_RTC / TIME_NTP` 公共状态接口。
4. 不修改 `SystemState.time_valid` 的 Boolean 类型。
5. 不修改现有大量依赖 `time_valid` 的业务代码。
6. 不自行实现 NTP 协议。
7. 不自行实现 Smooth Sync。
8. 不增加 RTC Alarm。
9. 不使用 PCF8563T CLKOUT。
10. 不使用 PCF8563T INT，除非后续业务明确需要。
11. 不因为增加 RTC 而重构整个 TimeManager。
12. 不因为 RTC Write 失败而使整个时间系统失效。

---

# 二十一、最终总体流程

```text
                    ┌───────────────┐
                    │ Device Boot   │
                    └───────┬───────┘
                            ↓
                     Read PCF8563T
                            ↓
                    ┌───────┴───────┐
                    │ RTC Valid ?   │
                    └───┬────────┬──┘
                       YES       NO
                        │         │
                        ↓         ↓
                 RTC → System   time_valid
                     Time          = false
                        │         │
                        ↓         │
                 time_valid=true  │
                        │         │
                        └────┬────┘
                             ↓
                       Wait Wi-Fi
                             ↓
                      Wi-Fi Connected
                             ↓
                           SNTP
                             ↓
                    NTP Sync Successful
                             ↓
                 System Time corrected
                             ↓
                    time_valid = true
                             ↓
                  Read current System Time
                             ↓
                        Read RTC
                             ↓
                 abs(System - RTC)
                             ↓
                    ┌────────┴────────┐
                    │                 │
                  ≤ 2 sec           > 2 sec
                    │                 │
                    ↓                 ↓
                No RTC Write    Critical Acquire
                                      ↓
                              System Time → RTC
                                      ↓
                                  Release
                                      ↓
                               success/failure
                                      ↓
                               wait next SNTP
                                      ↓
                                  24 hours
                                      ↓
                                    SNTP
```

## 二十二、实现原则

本次代码修改必须遵循现有项目已经验证的架构。

优先修改：

```text
TimeManager
ConfigManager
SystemState（仅在确有必要时，且不得修改 time_valid 类型）
```

如现有 I2C/OLED 初始化结构需要适配，只进行必要的最小修改。

不得修改：

```text
CloudManager
CommandManager
WorkflowManager
Capability Registry
System Command
EventManager
```

除非编译或接口依赖确实要求，否则不要扩大修改范围。

完成后必须：

1. 编译通过。
2. 检查所有现有 TimeManager 调用方。
3. 确认 `SystemState.time_valid` 的所有现有调用方式无需修改。
4. 确认 RTC Read 不持有 Critical Operation。
5. 确认 RTC Write 所有路径都正确 Release Critical Operation。
6. 确认 SNTP 不使用自定义 NTP 实现。
7. 确认 OLED 与 RTC 的 I2C Bus 不发生重复初始化或资源冲突。
8. 确认 RTC 无效不会阻塞设备启动。
9. 确认 RTC Write 失败不会导致 `time_valid=false`。
10. 确认下一次 SNTP 同步能够重新尝试 RTC 校准。
11. 确认 24 小时周期使用单调时间计时机制，不依赖可跳变的 System Time 计算周期。
