# 证据目录 · G4-2026-10-20（日志可信度 + 内存观测 + 烧录验证）

> 全部为本轮（`2026-10-20`）实跑产物。**不含**任何推测值。
> 关联报告：`docs/architecture/G4-Log-Credibility-and-Observability-Report-2026-10-20.md`

## 1. 云端测试

| 文件 | 内容 | 结果 |
|---|---|---|
| `cloud-run_all.log` | `node test/run_all.mjs` 全量输出（24 套件） | 全绿 |
| `cloud-real_d1-dedup.log` | `bash test/real_d1/mqtt_dedup_real_d1.sh`（本地真实 D1 引擎） | 19/19 |

★ 两个脚本都**未**连接远端 D1（dedup 脚本内含"可执行行 `--remote` 出现 0 次"自检）。

## 2. 固件测试

| 文件 | 内容 | 结果 |
|---|---|---|
| `fw-mem_watch_host.log` | `test/mem_watch_host`（wasm32 freestanding + node） | 40/40 全 PASS |

## 3. 构建产物（**两次烧录**）

| 文件 | sha256 | 说明 |
|---|---|---|
| `g4r4-target-2987a66f.bin` | `2987a66f98b362ad6242ad75123c214d9ccf3e3cb1b88c402d2902b96001a31a` | 第 1 次烧录（**暴露 3 项缺陷**，已被取代） |
| `g4r4-target-94ba711c.bin` | `94ba711cfda8284087ce9857f632f115819120d211826558a76dd45990267c31` | **第 2 次烧录（当前设备运行中）** |

构建参数：`PLATFORMIO_BUILD_DIR=.pio/build/g4r4` · `pio run` · 源码 `313e82e`
指标：RAM 41.1%（134 720 / 327 680 B）· Flash 67.5%（1 416 605 / 2 097 152 B）

烧录命令（仅 app 分区，不碰 nvs/otadata/littlefs）：

```bash
esptool.py --chip esp32s3 --port COM8 --baud 921600 \
  --before default_reset --after no_reset \
  write_flash 0x10000 .pio/build/g4r4/esp32-s3-devkitc-1/firmware.bin
```

## 4. 串口会话

| 文件 | 内容 | 关键点 |
|---|---|---|
| `serial-post-flash-2987a66f.log` | 第 1 次烧录后：`mw show` + `system.memwatch` + `system.stats` | `boot_seq 23→24`；**暴露** `alloc.calls=0`、`psram.reason=ok` 两缺陷 |
| `serial-post-flash-94ba711c.log` | 第 2 次烧录后：同上 + `mw probe` | `boot_seq 24→25`；`calls=18`、`unit=bytes`(算术)、`probe delta=2432`⇒bytes |
| `serial-drift-window-94ba711c.log` | 预热结束后的漂移采样 | `samples=2 span=60000`、`valid=false reason=too_few_samples` |

`g4r4_probe.py`：本轮用的串口探针（一次开端口、按序发多条命令；
`--reset` 用 RTS 脉冲复位以抓启动日志）。

## 5. 烧录前基线（引用上一轮证据）

- 运行固件：`docs/evidence/S-3-Flash-2026-10-20/flash-target-72d7a610.bin`
  sha256 `72d7a610761c8dd76c4c5857d12615c839f19633bee0b2388cb9e3b330dd744c`（**已实测核对**）
- 更早回退件：`docs/evidence/S-3-Flash-2026-10-20/rollback-firmware-a2c8a038.bin`
- 上上轮预烧录镜像：`docs/evidence/G-4-Stats-2026-10-20/pre-flash-firmware.bin`（`a7ad2d24…`，本轮补入库）

## 6. 未归档项（诚实边界）

- ★ **串口 dump（`serial-*.log`）不在仓库里**：`2026-10-21` 核查发现 `.gitignore` 的 `*.log`
  把它们全部挡在外面（上一轮误以为已入库）。同日起策略明确为：
  `docs/evidence/**` 下的**非串口**日志入库，**串口 dump 仍排除**（可能含 WiFi/MQTT 凭据与内网地址）。
  ⇒ 本文表格里三个 `serial-*.log` 是**本机留存**（`.pio/` 同源），**仓库内没有**。
  需要复现时按其"命令"列重跑串口会话即可。

- 生产 D1 的只读统计**未**存成文件（仅在本轮会话中执行）：
  `msg_id` 覆盖分布、payload 分组统计、抽样分组时间跨度。
  ⇒ 具体数值已逐条写进报告 §C.2 表格；若需**原始输出**须重跑（查询语句见
  `G4-Production-Change-Plan-2026-10-20.md` §5.2）。
- EMQX 侧运行统计：**无法获取**（API 403），未归档。
