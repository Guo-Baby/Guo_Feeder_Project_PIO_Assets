# G-4 · `system.stats` 烧录与一次性压力验收 · 证据索引

- 执行日期：`2026-10-19 16:21Z – 2026-10-19 17:0xZ`（本地 `2026-10-20 00:21 – 01:0x`）
- 设备：`288485896ce4`（ESP32-S3 N16R8，COM8 / CH343）
- 授权范围：一次核验 + 一次烧录 + 必需重启 + 启动健康检查 + 一轮分级受控压力测试
- 主报告：`docs/architecture/G-4-System-Stats-Flashing-and-Stress-Acceptance-Report.md`

## 文件清单

| 文件 | 内容 | 取证方式 |
|---|---|---|
| `git-state.txt` | 三仓 HEAD + 工作区状态（烧录前封存） | `git rev-parse` / `git status --short` |
| `pre-flash-firmware.bin` | 烧录前设备在跑的固件（备份，**不入库**，1.4 MB；`sha256=a7ad2d24…efd49be` 已固定，可由 `7f6b1fe` 重建） | 从 `.pio/build/esp32-s3-devkitc-1/firmware.bin` 复制 |
| `pre-flash-device-state.txt` | 烧录前设备/云端快照（boot=21、epoch=2、rewind=0、EMQX 计数） | EMQX `/clients` + D1 `log_history` / `log_boot_watermark` |
| `p2stats-implementation.diff` | `system.stats` 完整源码差异（5 文件，+158 行） | 从 `docs/evidence/G-4-RootCause-2026-10-19/` 复制，`sha256=d9b899ab…2bf3baa` |
| `build.log` | 本次重建的完整 PlatformIO 日志（`[SUCCESS]`，`error:` 0 处） | `pio run`（`PLATFORMIO_BUILD_DIR=.pio/build/g4stats`） |
| `serial-session.log` | **单次会话**串口原始捕获（含唯一一次启动 + 全部四级压力 + 静置期） | `.pio/g4s_serial.py`（只读，`dtr/rts=False`） |
| `burst-L1..L4.json` | 四级突发的逐条命令台账（发起/完成时刻、HTTP 码、响应体） | `.pio/g4_burst.py` |
| `post-flash-state.txt` | 烧录后设备/云端状态（boot=22、watermark 观测、D1 交叉核对） | EMQX + D1 |

## 关键哈希与版本

| 项 | 值 |
|---|---|
| 源码基线 commit | `7f6b1fe03d77b666f4894305f9790f8ee494e848`（`wb`） |
| 本补丁 commit | `82123a3b7b03dc2b8a6a4232c27e49c5bba6a647` |
| 补丁 diff SHA-256 | `d9b899abbd99b1bd557cb176509404656034c58b3f908da38a63af3d52bf3baa` |
| 烧录前固件 SHA-256 | `a7ad2d24c3877b39102dfbffe9ffd237cebf2b86564672d6010cae857efd49be`（1 400 800 B） |
| **烧录固件 SHA-256** | `a2c8a0388e1e4b621a0e2e7d28a4b2269e1d84f96044211ace96532e61040b4f`（1 402 672 B） |
| 构建资源占用 | RAM 132 744 B / 327 680（40.5%）· Flash 1 402 305 B / 2 097 152（66.9%） |
| 固件体积增量 | **+1 872 B（+0.1337%）** —— 与历史记录「+1872 B（+0.13%）」逐字节吻合 |

## ⚠️ 未入库的两个原始日志

仓库 `.gitignore` 含 `*.log` ⇒ 以下两个文件**只能留在本地工作区，不入 git**：

| 文件 | 体积 | 可复现性 |
|---|---|---|
| `serial-session.log` | 224 KB | **不可复现**（含唯一一次启动与四级压力原文）⇒ **请勿删除**；如需长期留存建议压缩后另行归档 |
| `build.log` | 512 KB | 可由 §「只读复现命令」第 2 步重建（含 `safe-delete` 噪声，属环境产物） |

## 只读复现命令

```bash
# 1) 复现源码
git checkout 7f6b1fe && git apply docs/evidence/G-4-Stats-2026-10-20/p2stats-implementation.diff
# 2) 复现构建
PLATFORMIO_BUILD_DIR=.pio/build/g4stats /d/platformIO/penv/Scripts/pio.exe run
# 3) 复现串口会话
"C:/Users/wang/.workbuddy/binaries/python/envs/default/Scripts/python.exe" .pio/g4s_serial.py COM8 2400 <out>
# 4) 复现一级突发（只读 config_query）
"C:/Users/wang/.workbuddy/binaries/python/envs/default/Scripts/python.exe" .pio/g4_burst.py 20 1.0 Lx <out.json>
```
