# `data/config/` 配置说明（公开模板）

> 本目录里的 **真实 `*.json` 被 `.gitignore` 排除**（含 Wi-Fi / MQTT 凭据、设备身份）。
> 仓库只提交 `*.json.example` 模板。**首次使用请逐个复制去掉 `.example` 再填自己的值。**

```bash
cd data/config
for f in *.json.example; do cp "$f" "${f%.example}"; done
cd ../.. && cp data/workflow.json.example data/workflow.json   # 需要自动化流程时
```

## 加载与回退机制（固件实际行为）

1. 运行时读 **`/config/<module>.json`**（`ConfigManager` 的模块路径）。
2. 缺失 / 校验失败 ⇒ 回退 **`/factory/<module>.json`**（云端首次下发时创建）。
3. 两者都无 ⇒ 使用**代码内默认值**（`config_get_*` 的 `| default`）。
4. **所以：没有 `wifi.json` 设备也能开机，只是连不上网；没有 `mqtt.json` 不会连云。**
5. `version.json` 记录各模块版本号；`version` 递增才触发"配置已变更"广播。

## 必需 / 可选

| 文件 | 必需性 | 缺失后果 |
|---|---|---|
| `wifi.json` | **必需**（要联网） | 无法连接 Wi-Fi |
| `mqtt.json` | **必需**（要连云） | 不建立 MQTT 连接；本地阀/称重仍可用 |
| `emqxsl-ca.crt`（上级 `data/`） | **必需**（TLS） | TLS 握手失败；`ca_path` 指向它 |
| `mi_thermo.json` | 可选（用米家温湿度计） | BLE 采集不启动 |
| `xiaomi_lan.json` | 可选（米家局域网联动） | 喂食器/插座联动关闭 |
| `oled / rtc / stepper / vibration / weight / time / valve .json` | 可选 | 用代码默认值（多数硬件默认可用） |
| `version.json` | 建议随附 | 首次启动自动生成 |
| `../workflow.json` | 可选 | 无自动化工作流 |

## 字段表

### `wifi.json` — 云端通信前置条件

| 字段 | 类型 | 必填 | 示例 | 说明 |
|---|---|---|---|---|
| `ssid` | string ≤32 | ✅ | `YOUR_WIFI_SSID` | 2.4 GHz SSID（ESP32 不支持 5 GHz） |
| `password` | string ≤64 | ✅ | `YOUR_WIFI_PASSWORD` | **敏感**，不要提交 |
| `connect_timeout` | int(ms) | ⬜ | `30000` | 连接超时 |
| `reconnect_interval` | int(ms) | ⬜ | `10000` | 断线重连间隔 |

### `mqtt.json` — 云端通信

| 字段 | 类型 | 必填 | 示例 | 说明 |
|---|---|---|---|---|
| `mqtt_server` | string ≤128 | ✅ | `YOUR_MQTT_HOST` | Broker 主机名（**不要提交真实值**） |
| `mqtt_port` | int | ✅ | `8883` | 8883=TLS，1883=明文 |
| `client_id` | string ≤64 | ✅ | `dev_<device_id>` | 由设备 MAC 派生，**每台唯一** |
| `username` | string ≤64 | ✅ | `YOUR_MQTT_USERNAME` | EMQX ACL 挂在这个字段上 |
| `password` | string ≤64 | ✅ | `YOUR_MQTT_PASSWORD` | **敏感** |
| `subscribe_topic` | string ≤128 | ✅ | `guo_feeder/<device_id>/down` | 下行 |
| `publish_topic` | string ≤128 | ✅ | `guo_feeder/<device_id>/up` | 上行 |
| `log_topic` | string ≤128 | ✅ | `guo_feeder/<device_id>/log` | 日志 |
| `ca_path` | string ≤64 | ⬜ | `/emqxsl-ca.crt` | LittleFS 内 CA 路径 |
| `retry_interval` | long(ms) | ⬜ | `10000` | 重试间隔 |
| `retry_max` | int | ⬜ | `30` | 最大重试次数 |
| `sleep_retry_interval` | long(ms) | ⬜ | `1800000` | 退避休眠 |
| `mqtt_keep_alive` | int(s) | ⬜ | `60` | KeepAlive |
| `telemetry_enable` | bool | ⬜ | `true` | 遥测上行；**还需编译期 `GF_TELEMETRY_ENABLED`**，只改一个无效果 |

> `<device_id>` = 设备 MAC 派生（`dev_` + 12 位 hex），**运行期生成**，不是仓库里的固定值。
> Topic 前缀 `guo_feeder/` 是协议约定，可公开；真实 `<device_id>` 不应提交。

### `mi_thermo.json` — BLE 温湿度计（可选）

| 字段 | 类型 | 必填 | 示例 | 说明 |
|---|---|---|---|---|
| `BLE_key` | string(32 hex) | ✅ | `YOUR_32_HEX_BLE_BIND_KEY` | 米家绑定密钥，**敏感** |
| `MAC` | string | ✅ | `AA:BB:CC:DD:EE:FF` | 设备蓝牙 MAC |
| `mi_thermo_allow_collect` | bool | ⬜ | `true` | 是否允许 BLE 扫描采集 |

> `BLE_key` 获取方式：米家 App 绑定设备后由第三方工具读取（本仓库不提供该工具）。

### `xiaomi_lan.json` — 米家局域网联动（可选）

| 字段 | 类型 | 必填 | 示例 | 说明 |
|---|---|---|---|---|
| `feeder_enable` | bool | ⬜ | `true` | 是否允许局域网控制米家喂食器 |
| `plug_enable` | bool | ⬜ | `true` | 是否允许局域网控制米家智能插座 |

> 这两个键是唯一可从云端写入的开关（见 Worker `CONFIG_WRITE_KEYS`）；
> 打开后仍需设备本地 Token 才能实际下发（Token 存 D1，不进仓库）。

### 硬件模块（`oled` / `rtc` / `stepper` / `vibration` / `weight` / `time` / `valve`）

全部为 GPIO 编号、电平、周期、超时等**非敏感硬件参数**，直接沿用模板默认值即可。

需要特别注意的两项：

- `valve.safety_timeout_sec`：**电磁阀自动断电时间**，不要调大（安全阀）。
- `weight.scale` / `weight.zero_offset`：**每台设备必须重新标定**，模板已置 `0`，照抄会导致读数无意义。

### `version.json`

各模块版本号。`version` 递增才触发设备重新加载该模块；首次启动自动生成，可随仓库提交（含真实值也无隐私风险，但建议用模板值）。

### `../workflow.json`

自动化工作流。`steps[].type` 取 `trigger` / `action`；`id` 为固件动作或触发器标识（如 `VALVE_OPEN`、`timer_daily`、`delay`）。

> ⚠️ 不要把真实凭据写回 `.example` 模板——模板是公开的。
