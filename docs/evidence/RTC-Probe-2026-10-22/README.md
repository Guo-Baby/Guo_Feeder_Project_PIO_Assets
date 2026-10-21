# RTC 探测性启用证据（2026-10-22 02:11–02:24，COM8）

## 1. 背景与结论

用户提出「把 RTC 芯片支持纳入固件」。核查结果推翻前提：**固件侧 RTC 支持早已完整实现**
（`src/services/time_manager.cpp/.h`，PCF8563T 真实驱动），文档也齐全。真正的缺口是
`rtc.enable=false` + 文档 NC-5 记载「板上无芯片」。

本轮用「只改一个配置项、不动硬件」的方式做了探测性启用，结论：

> **✅ 板上确实有 PCF8563T（0x51），且时间有效，boot 硬同步成功。NC-5 解除。**

## 2. 操作时间线

| 时刻 | 动作 | 结果 |
|---|---|---|
| 02:11:14 | `config_query rtc.enable` | `False`（设备实值基线） |
| 02:11:53 | 基线 uptime | 135,986 ms |
| 02:12:0x | `config_set rtc enable true` + `config_save` | 终帧 `s=0`，设备重启 |
| 02:14:34 | 复核 | `rtc.enable=True`（落盘成功） |
| 02:15:09 | 跑 `rtc_probe.py`（**旧版：drain 3 s + RTS 脉冲**） | **UNKNOWN** —— RTC 相关日志一行都没有 |
| 02:17:05 | 复核 | `rtc.enable=**False**`、`mqtt.telemetry_enable=**None**` ⇒ **配置双双丢失** |
| 02:18:18 | 重设两个键（`rtc.enable`/`mqtt.telemetry_enable` 均 true） | 两次 `s=0` |
| 02:18:35 | `config_save` | `s=0 m=config saved`，设备重启 |
| 02:19:45 | 复核 | uptime 64,052（新 boot），`rtc.enable=True` ✓ |
| 02:20:03 | 跑 `rtc_probe.py`（**修正版：不做 RTS 脉冲，只抓 open 自带的那次复位**） | **CHIP_PRESENT** |
| 02:22–02:24 | 复核 | `rtc.enable=True`、`mqtt.telemetry_enable=True`（均保持） |

## 3. RTC 探测结果（决定性证据）

```
>>> PORT-OPENED 02:20:03 (port=COM8, window=80s, 单次复位不复脉冲)

>>> 关键行（RTC 相关 + 连接状态）
    [RTC 探测成功（芯片在）] PCF8563T detected at 0x51
    [RTC boot 硬同步成功] RTC boot restore OK: 2026-10-22 02:20:06
    [WiFi 已连] WiFi connected
    [MQTT 已连] MQTT connected
    [boot 完成（无 panic）] [CFG][I] boot validated

>>> 判定
    RTC 探测结论 = CHIP_PRESENT
    → 芯片在且时间有效：已硬同步 System Time
```

对应源码：
- `time_manager.cpp:369` `Serial.printf("[Time] PCF8563T detected at 0x%02X ...")`
- `time_manager.cpp:715` `Serial.printf("[Time] RTC boot restore OK: %s\n", ...)`

## 4. ★★ 副产品：配置回滚根因坐实（CFG-ROLLBACK-SUSPECT → 已定位）

02:15:09 那次探针导致 `rtc.enable` 与 `mqtt.telemetry_enable` **同时丢失**，
与 2026-10-21 22:26:45 的事件完全同构。两者的共同点都是探针脚本的
「**开串口复位 #1 + 3 s 后 RTS 脉冲复位 #2**」：

1. `serial.Serial()` 打开 COM8（CH343）⇒ 板子复位 #1，boot 开始；
2. 脚本 drain 3 s 后脉冲 RTS ⇒ 复位 #2；
3. 复位 #1 的 boot **在 3 s 内走不到 `config_boot_validate()`** ⇒ boot flag 未写入；
4. 复位 #2 时 `config_manager.cpp:1113` 读到 `!last_boot_ok` ⇒ `need_recover`
   ⇒ **从 Backup 恢复所有模块，覆盖 Active** ⇒ 新设的配置键回退到 Backup（旧值）。

⇒ **这不是固件 bug，是设计行为**（boot flag + Backup 恢复机制）被「3 秒内连续两次复位」触发。
**修正**：探针改为**只利用 open() 自带的一次复位，不再脉冲**（已改入 `rtc_probe.py`）。
`telemetry_flash_probe.py` 仍用旧流程，**待同改**。

## 5. NC-5 修订（已写入 `docs/issues/未修复的问题.md`）

- 旧记载「板上无 RTC 芯片（ERR_CODE=2 NACK）」= **误判**。
- 真实原因：`time_init()` 当时在第二层、**早于 `oled_init()`**；RTC 与 OLED 共用 I2C，
  `Wire.begin()` 由 `oled_init()` 完成 ⇒ 探测时总线未初始化 ⇒ NACK。
  该顺序已修复（`main.cpp:556 oled_init()` → `:561 time_init()`，注释明载）。
- 已验证：`RTC_PROBE` 成功分支、`RTC_BOOT_RESTORE`；
  仍待验证：`RTC_CALIBRATED`（需 SNTP 后漂移 > `calibrate_threshold_sec`）、
  `RTC_WRITE_FAILED`、`RTC_VL_FLAG`（需电池耗尽）、`RTC_BCD_INVALID`。

## 6. 当前设备状态（02:24）

- `rtc.enable = true`（已落盘，跨重启保持）
- `mqtt.telemetry_enable = true`（已落盘）
- RTC 时间有效，boot 时硬同步 System Time；后续 SNTP 成功会按阈值校准 RTC。
