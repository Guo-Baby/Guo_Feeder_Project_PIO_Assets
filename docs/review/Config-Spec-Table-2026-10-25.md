# Guo Feeder · 配置规格表（Config Spec）· 2026-10-25

> **权威数据源**：`Cloudflare_Assets/guo-feeder-api/src/config-spec.js`（单一事实来源）
> **对齐测试**：`Cloudflare_Assets/tests/api/config_spec_parity.test.mjs`（30 断言）
> 阶段：阶段三（P4-0）产物 · 状态：已实现并测试通过，**未部署**

---

## 1. 这份表是什么 / 为什么必须有

固件的 `ConfigManager` **不做统一 schema 校验**：
- 范围散落在各业务模块的 `init()` 里，且多为**静默钳位**（写进去看着成功，设备实际改了值）；
- 死参数（getter 存在、但**没有任何消费者**）照样能写、照样返回成功。

⇒ UI 若自己猜范围 / 自己猜哪些键有效，必然出现"界面说改了、设备没改"的欺骗。
本表把**固件事实**（键、类型、钳位、消费者、风险）逐条固化下来，
前端与 Worker 都从这里取，**不复制第二份**。

## 2. 抽取方法（可复现）

| 事实 | 来源 | 判定方式 |
|---|---|---|
| 键清单 / 类型 / 兜底值 | `src/services/config_manager.h` 的 `config_get_*` 声明 + `.cpp` 里的 `module_ro(...)[key] \| fallback` | 机械解析 |
| 出厂值 | `data/config/<module>.json` | 直接读文件 |
| 钳位范围 | 各业务 `init()`：`valve.cpp` / `stepper.cpp` / `vibration.cpp` / `oled.cpp` / `weight.cpp` | 读 `if` 判断 |
| **死参数** | 该 getter 在 `src/services/config_manager.cpp` **之外**的调用点 = 0 | 全仓 grep（只看 `.cpp`，`.h` 只是声明） |
| 模块清单 | `kModuleNames[CONFIG_MODULE_COUNT]`（在 `.cpp`） | 解析数组，**顺序也校对**（`config_query` 全量返回按此顺序） |

## 3. 统计

| 分类 | 数量 | 说明 |
|---|---|---|
| 键总数 | **66** | 10 个模块 |
| 可改（`editable:true`） | **59** | D-6：一次性全开，不分批 |
| 死参数（`dead:true`） | **7** | 写了对设备**没有任何效果** ⇒ UI 必须禁用并说明原因 |
| 其它不可改 | 0 | — |
| 敏感（掩码显示） | **3** | `wifi.password` / `mqtt.password` / `mi_thermo.BLE_key` |
| 固件有钳位的键 | **9** | 只有这 9 个能在 `min/max` 里写范围 |
| 固件**不**校验的键 | 57 | 只能给 UI 建议区间（`suggest_min/max`），**不得**冒充固件校验 |

## 4. 死参数清单（写了对设备没有任何效果）

| 键 | 中文 | 为什么是死参数 |
|---|---|---|
| `mqtt.retry_interval` | 重连间隔 | `cloud_manager.cpp` 的 `static retry_interval` **从未被赋值** ⇒ 实际用编译期常量 |
| `mqtt.retry_max` | 最大快速重试次数 | `cloud_manager.cpp:2437` 用 `(retry_max > 0)` 判断，但该变量从未被赋值 ⇒ 恒用 `MQTT_RETRY_MAX` |
| `mqtt.sleep_retry_interval` | 休眠重试间隔 | 变量从未被赋值（调用点 0） |
| `mqtt.mqtt_keep_alive` | KeepAlive | `keep_alive` 变量从未被赋值 ⇒ `cfg.keepalive` 恒为 0（调用点 0） |
| `valve.open_duration_ms` | 单次开阀时长（预留） | 全仓无消费者 |
| `weight.sample_interval` | 采样间隔 | `weight.cpp` 不读该键（采样周期由 loop 节拍决定） |
| `weight.filter_samples` | 滤波窗口 | 滤波窗口用编译期 `WEIGHT_FILTER_WINDOW`，不读该键 |

> ★ 服务端 `validateConfigParams()` 对死参数直接 400（`reason` 可定位），
> **不下发到设备**；UI 层同时禁用按钮 + 显示原因（双保险）。

## 5. 出厂值 / 兜底值漂移（SPEC_DRIFT）

| 键 | 出厂 json | getter 兜底 | 后果 |
|---|---|---|---|
| `valve.safety_timeout_sec` | 300 | 60 | 老设备 300、新烧录 300，**只有缺键**才是 60 |
| `valve.gpio_pin` | 12 | -1 | 缺键 ⇒ -1（无效 GPIO） |
| `weight.enable` | **false** | **true** | ⚠ **fail-open 风险**：缺键的模块反而会启用秤（HX711 未接 ⇒ 每 5s 一条 WEIGHT_ERROR） |

> ★ 这三条**不做二选一**，UI 同时展示"设备当前值"，不拿兜底值冒充设备值。

## 6. 完整键表（66 项）

| 模块 | 键 | 类型 | 中文 | 范围 | 状态 | 生效 | 风险 |
|---|---|---|---|---|---|---|---|
| 网络连接 | `wifi.ssid` | string | WiFi 名称 | — | 可改 | 需重启 | danger |
| 网络连接 | `wifi.password` | string | WiFi 密码 | — | 可改 | 需重启 | 敏感 |
| 网络连接 | `wifi.connect_timeout` | int | 连接超时 | 1000~300000 UI建议 | 可改 | 需重启 | write |
| 网络连接 | `wifi.reconnect_interval` | int | 重连间隔 | 1000~600000 UI建议 | 可改 | 需重启 | write |
| 云连接 | `mqtt.mqtt_server` | string | 服务器地址 | — | 可改 | 需重启 | danger |
| 云连接 | `mqtt.mqtt_port` | int | 服务器端口 | 1~65535 UI建议 | 可改 | 需重启 | write |
| 云连接 | `mqtt.client_id` | string | 客户端 ID（模板） | — | 可改 | 需重启 | critical |
| 云连接 | `mqtt.subscribe_topic` | string | 下行 Topic（模板） | — | 可改 | 需重启 | critical |
| 云连接 | `mqtt.publish_topic` | string | 上行 Topic（模板） | — | 可改 | 需重启 | critical |
| 云连接 | `mqtt.log_topic` | string | 日志 Topic（模板） | — | 可改 | 需重启 | critical |
| 云连接 | `mqtt.username` | string | 用户名 | — | 可改 | 需重启 | critical |
| 云连接 | `mqtt.password` | string | 密码 | — | 可改 | 需重启 | 敏感 |
| 云连接 | `mqtt.ca_path` | string | CA 证书路径 | — | 可改 | 需重启 | danger |
| 云连接 | `mqtt.retry_interval` | long | 重连间隔 | — | **死参数** | 需重启 | read |
| 云连接 | `mqtt.retry_max` | int | 最大快速重试次数 | — | **死参数** | 需重启 | read |
| 云连接 | `mqtt.sleep_retry_interval` | long | 休眠重试间隔 | — | **死参数** | 需重启 | read |
| 云连接 | `mqtt.mqtt_keep_alive` | int | KeepAlive | — | **死参数** | 需重启 | read |
| 云连接 | `mqtt.telemetry_enable` | bool | 遥测上报总开关 | — | 可改 | 需重启 | write |
| 电磁阀 | `valve.enable` | bool | 启用电磁阀 | — | 可改 | 需重启 | danger |
| 电磁阀 | `valve.gpio_pin` | int | 继电器 GPIO | -1~48 UI建议 | 可改 | 需重启 | critical |
| 电磁阀 | `valve.active_level` | int | 开阀电平 | 取值 0/1 | 可改 | 需重启 | critical |
| 电磁阀 | `valve.open_duration_ms` | int | 单次开阀时长（预留） | — | **死参数** | 需重启 | read |
| 电磁阀 | `valve.safety_timeout_sec` | int | 自动关阀超时 | 10~300 固件钳位 | 可改 | 需重启 | danger |
| 出粮电机 | `stepper.enable` | bool | 启用出粮电机 | — | 可改 | 需重启 | danger |
| 出粮电机 | `stepper.step_pin` | int | STEP 引脚 | 0~48 UI建议 | 可改 | 需重启 | critical |
| 出粮电机 | `stepper.dir_pin` | int | DIR 引脚 | 0~48 UI建议 | 可改 | 需重启 | critical |
| 出粮电机 | `stepper.en_pin` | int | EN 引脚 | 0~48 UI建议 | 可改 | 需重启 | critical |
| 出粮电机 | `stepper.en_active_level` | int | EN 有效电平 | 取值 0/1 | 可改 | 需重启 | critical |
| 出粮电机 | `stepper.dir_invert` | bool | 方向取反 | — | 可改 | 需重启 | danger |
| 出粮电机 | `stepper.steps_per_rev` | int | 每圈脉冲数 | 1~100000 固件钳位 | 可改 | 需重启 | write |
| 出粮电机 | `stepper.dispense_angle_deg` | int | 单次出粮角度 | 1~360 固件钳位 | 可改 | 需重启 | danger |
| 出粮电机 | `stepper.pulse_period_us` | int | 脉冲周期 | 20~60000 固件钳位 | 可改 | 需重启 | danger |
| 出粮电机 | `stepper.enable_delay_ms` | int | 使能后延时 | 0~2000 固件钳位 | 可改 | 需重启 | write |
| 出粮电机 | `stepper.max_pulses` | int | 单次最大脉冲 | 1~6400 固件钳位 | 可改 | 需重启 | danger |
| 出粮电机 | `stepper.action_timeout_ms` | int | 动作超时 | 1000~30000 固件钳位 | 可改 | 需重启 | danger |
| 出粮电机 | `stepper.disable_after_action` | bool | 动作后失能 | — | 可改 | 需重启 | write |
| 振动马达 | `vibration.enable` | bool | 启用振动马达 | — | 可改 | 需重启 | danger |
| 振动马达 | `vibration.relay_pin` | int | 继电器 GPIO | 0~48 UI建议 | 可改 | 需重启 | critical |
| 振动马达 | `vibration.active_level` | int | 启动电平 | 取值 0/1 | 可改 | 需重启 | critical |
| 振动马达 | `vibration.max_duration_ms` | int | 单次最长振动 | 1~30000 固件钳位 | 可改 | 需重启 | danger |
| 振动马达 | `vibration.safety_timeout_ms` | int | 模块兜底超时 | 1000~30000 固件钳位 | 可改 | 需重启 | danger |
| 振动马达 | `vibration.min_interval_ms` | int | 启停最小间隔 | 0~60000 UI建议 | 可改 | 需重启 | write |
| 称重 | `weight.enable` | bool | 启用称重 | — | 可改 | 需重启 | danger |
| 称重 | `weight.dt` | int | HX711 DT 引脚 | 0~48 UI建议 | 可改 | 需重启 | critical |
| 称重 | `weight.sck` | int | HX711 SCK 引脚 | 0~48 UI建议 | 可改 | 需重启 | critical |
| 称重 | `weight.sample_interval` | int | 采样间隔 | 10~1000 UI建议 | **死参数** | 需重启 | read |
| 称重 | `weight.scale` | float | 比例因子（校准） | 0.01~100000 UI建议 | 可改 | 需重启 | danger |
| 称重 | `weight.zero_offset` | long | 零点偏移（校准） | — | 可改 | 需重启 | danger |
| 称重 | `weight.filter_samples` | int | 滤波窗口 | 1~64 UI建议 | **死参数** | 需重启 | read |
| 时钟芯片 | `rtc.enable` | bool | 启用时钟芯片 | — | 可改 | 需重启 | write |
| 时钟芯片 | `rtc.sda` | int | I2C SDA | 0~48 UI建议 | 可改 | 需重启 | critical |
| 时钟芯片 | `rtc.scl` | int | I2C SCL | 0~48 UI建议 | 可改 | 需重启 | critical |
| 时钟芯片 | `rtc.i2c_addr` | int | I2C 地址 | 0~127 UI建议 | 可改 | 需重启 | danger |
| 时钟芯片 | `rtc.calibrate_threshold_sec` | int | 校时阈值 | 0~3600 UI建议 | 可改 | 需重启 | write |
| 系统时间 | `time.timezone` | int | 时区 | -12~14 UI建议 | 可改 | 需重启 | write |
| 系统时间 | `time.ntp_server1` | string | NTP 服务器 1 | — | 可改 | 需重启 | write |
| 系统时间 | `time.ntp_server2` | string | NTP 服务器 2 | — | 可改 | 需重启 | write |
| 系统时间 | `time.sync_interval_sec` | int | 校时间隔 | 60~604800 UI建议 | 可改 | 需重启 | write |
| 温湿度计 | `mi_thermo.BLE_key` | string | BLE 绑定 Key | — | 可改 | 需重启 | 敏感 |
| 温湿度计 | `mi_thermo.MAC` | string | MAC 地址 | — | 可改 | 需重启 | write |
| 温湿度计 | `mi_thermo.mi_thermo_allow_collect` | bool | 允许扫描采集 | — | 可改 | 运行时 | write |
| 屏幕 | `oled.sda` | int | I2C SDA | 0~48 UI建议 | 可改 | 需重启 | critical |
| 屏幕 | `oled.scl` | int | I2C SCL | 0~48 UI建议 | 可改 | 需重启 | critical |
| 屏幕 | `oled.i2c_speed` | int | I2C 频率 | 10000~400000 UI建议 | 可改 | 需重启 | write |
| 屏幕 | `oled.frame_delay` | int | 帧延迟 | 0~1000 UI建议 | 可改 | 需重启 | write |
| 屏幕 | `oled.rotation` | int | 屏幕旋转 | 取值 0/1/2/3 | 可改 | 需重启 | write |

## 7. 敏感项处理

| 键 | 展示 | 目录里带出厂值？ |
|---|---|---|
| `wifi.password` | 掩码（首尾各 2 位） | **否** |
| `mqtt.password` | 掩码 | **否** |
| `mi_thermo.BLE_key` | 掩码 | **否** |

> ★ 目录是**发给浏览器**的，带上出厂密码等于把凭据塞进前端缓存与 devtools。
> 目录里这三个键的 `factory`/`fallback` 恒为 `null`，只带 `masked:true`。
> （由 `tests/api/ui_security.test.mjs` 的 D8-1a/D8-1b 逐条锁住。）

## 8. 写通道与门禁

| 命令 | 作用 | 是否落盘 | 是否重启 |
|---|---|---|---|
| `system.config_set` | 写**单个键**（仅 RAM，带 `expect_version` 乐观锁） | 否 | 否 |
| `system.config_save` | 把 dirty 模块写入 Flash | **是** | 启动 5 分钟自动重启计时 |
| `system.config_restart` | 先落盘再请求重启（`cancel:true` 只取消计时） | 是 | 是 |

> ★ 白名单**只有这 3 条**。`config_delete` / `config_reset` / `config_backup` / `config_module`
> **故意不在白名单**（能一次毁掉整模块配置）。
> ★ `CONFIG_WRITE_ENABLED_BY_DEFAULT = false` —— 开放写配置的前置条件是 **P4-3 失败演练**
> （落盘失败 / 重启失败 / 非计划重启回滚 / commit 卡 PENDING），本轮**未做**（需真机）。
> ⇒ 代码链路已实现且有测试，但生产默认**只读**并明示原因。

## 9. 已知协议缺口

| 缺口 | 影响 | 当前处置 |
|---|---|---|
| `config_restart_pending()` 无云端命令入口 | 读不到设备端真实剩余倒计时 | 前端按**本地落盘时刻**估算，并明示"是前端估算，不是设备真实剩余时间"（D-7） |
| 固件无"每模块 dirty 标志" | 页面重开后无法回溯"已写内存未落盘" | 只在**本次会话**内跟踪；重开后显示"设备当前值" |
| 回滚证据只在日志里 | 无法主动查询"配置是否已回滚" | 读日志里的 `0x0102 LOG_SYS_BOOT_INCOMPLETE_PREV`（CRITICAL）并提示复核 |

## 10. 与本表相关的文档

- 实施计划与唯一进度表：`docs/review/UI-V11-Gap-Audit-and-Implementation-Plan-2026-10-25.md` §8
- 阶段三交付报告：`docs/review/UI-Stage3-P4-Config-UI-Delivery-2026-10-26.md`
- UI 设计：`docs/ui-v1/12-device-detail-and-config.md`
