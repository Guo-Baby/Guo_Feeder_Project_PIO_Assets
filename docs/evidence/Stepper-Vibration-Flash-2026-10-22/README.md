# 步进电机 + 振动马达 固件烧录记录（2026-10-22）

## 1. 目标与前置

- 目标：把 `stepper`（DRV8825）与 `vibration`（单 GPIO）两个新模块烧入当前 USB
  连接的 ESP32-S3（COM8 / USB-Enhanced-SERIAL CH343）。
- 基线：固件仓 `wb` @ `99c0401`，工作区仅有两个未提交的规划文档。
- ★ 设备当前运行的是**遥测 gate-ON 固件**（2026-10-21 22:30 烧录，
  `-DGF_TELEMETRY_ENABLED=1`）。为避免功能回退，本次烧录镜像**必须**用同一编译门构建。

## 2. 构建

| 变体 | 构建目录 | 编译门 | Flash | RAM | 结果 |
|---|---|---|---|---|---|
| 默认（自检用） | `.pio/build/step_vib` | 无 | 1,431,589 B / **68.3%** | 135,576 B / **41.4%** | SUCCESS（0 error） |
| **烧录目标** | `.pio/build/step_vib_on` | `-DGF_TELEMETRY_ENABLED=1` | 1,433,505 B / **68.4%** | 135,624 B / **41.4%** | SUCCESS（0 error） |

- 资源门禁（H-WP4-FW-08 覆盖后的整体余量检查）：G-1 Flash < 85% ✅；G-2 RAM 不放宽 ✅。
- 镜像：`flash-target-e84c0c28.bin`
  sha256 = `e84c0c289a9d5b3c406eeb11516da37588744472fa2b2e9fdea2ee738eea44b3`
  size = 1,433,872 B
- 生效判据（bin 内字符串命中）：`STEPPER_DISPENSE_ROTATE` / `STEPPER_ROTATE_STEPS` /
  `STEPPER_EMERGENCY_STOP` / `VIBRATION_START` / `VIBRATION_STOP` 各 2 次命中。

## 3. 框架事实（重要）

- `platformio.ini` 锁 `platformio/framework-arduinoespressif32@3.20017.241212`
  ⇒ 实际是 **Arduino-ESP32 2.0.17 / ESP-IDF 4.4.7**（不是 IDF 5.x）。
- 因此 RMT 只能用**旧版 API**（`driver/rmt.h`）：`rmt_config` / `rmt_driver_install` /
  `rmt_set_tx_loop_mode` / `rmt_set_tx_loop_count` / `rmt_enable_tx_loop_autostop` /
  `rmt_write_items` / `rmt_tx_stop` / `rmt_register_tx_end_callback`。
- `SOC_RMT_SUPPORT_TX_LOOP_COUNT = 1`，每通道 48 字 ⇒ 采用循环模式（单 item +
  loop_count），不需要大缓冲。`loop_count` 上限 1023，超出分段续发。

## 4. 烧录执行

```
esptool.py --chip esp32s3 --port COM8 --baud 921600 \
    --before default_reset --after no_reset \
    write_flash -z 0x10000 flash-target-e84c0c28.bin

> Wrote 1433872 bytes (917271 compressed) at 0x00010000 in 15.7 seconds
> Hash of data verified.
> Staying in bootloader.
```

- **只写 app 分区 0x10000**：未 `erase_flash`、未 `uploadfs`、未触碰配置分区。
- 阻塞与处理：`COM8` 被 `pio device monitor`（PID 19352/33996，3:32 启动）占用，
  关闭后烧录成功。

## 5. 烧录后验证（均未执行任何电机/马达动作）

| 项 | 方法 | 结果 |
|---|---|---|
| 启动无异常 | 串口探针（单次复位） | **PASS**：无 panic / 栈溢出 / 看门狗；WiFi + MQTT 已连 |
| 配置未回滚 | `config_query` | **PASS**：`rtc.enable=True`、`mqtt.telemetry_enable=True` |
| 新 Action 已注册 | `{"c":"registry","k":0}` | **PASS**：`v=2 / n=11` |
| 新配置模块可写 | `config_query stepper.enable` | **PASS**：返回 `key not found`（而非 `module not found`）⇒ 模块已引导 |

### Action Registry（烧录后实测）

```
v=2 n=11
k=0  COMPUTER_RESET          k=6  STEPPER_ROTATE_STEPS
k=1  MI_THERMO_START_SCAN    k=7  VALVE_CLOSE
k=2  MI_THERMO_STOP_SCAN     k=8  VALVE_OPEN
k=3  MQTT_TEST               k=9  VIBRATION_START
k=4  STEPPER_DISPENSE_ROTATE k=10 VIBRATION_STOP
k=5  STEPPER_EMERGENCY_STOP
```

⚠️ **Stable ID 已重排（R-1 风险兑现）**：`VALVE_OPEN` 由 k=5 → **k=8**，
`VALVE_CLOSE` 由 k=4 → **k=7**，且 `v` 由 1 → **2**。
所有云端 / 客户端脚本里硬编码的 `v`/`k` 必须同步更新，否则返
`action registry version mismatch` 或触发错误的 Action。

## 6. 未执行 / 未验证

- 未执行任何会让 STEP 输出、电机转动、继电器吸合或马达振动的动作。
- 未用示波器测量 STEP 波形（RMT 输出**未经真机测量**，仅编译与代码级验证）。
- 未验证 120° 真实机械转角、无失步/卡步、继电器有效电平、马达实际运行。
- 结构化日志码（P1.1 冻结契约 `src/log/log_events.h`）未新增，
  本模块日志走 `Serial.printf`；如需结构化落 Flash + 上云，需另开变更。
