# S-3 完整烧录验证 · 证据卷宗

> 窗口：`2026-10-19T18:33Z – 18:51Z`（本地 `2026-10-20 02:33 – 02:51`）
> 报告：`docs/architecture/S-3-Flash-Verification-and-Design-Goal-Assessment.md`
> ★ 本轮**只**做：读基线 → 烧 app 分区 → 1 次启动 → 只读命令验证。**未改配置、未改 D1、未部署云端、未改 EMQX。**

## 1. 文件清单

| 文件 | 说明 | 关键值 |
|---|---|---|
| `serial-flash-boot.log` / `.gz` | **串口原文 60 350 B**：启动 + 20 条命令 3 帧 + 4 次 `system.loop` + 全部 ACK；含 `# open/@18:34:35` 与 `# closed/@18:49:35` | `ESP-ROM` ×1 · `panic` ×0 |
| `loop-stats-analysis.txt` | 4 快照 + 2 窗口的恒等式/分位/自洽性计算 | 见文件 |
| `pre-flash-state.txt` | 烧录前：`boot_seq=22`、固件 `a2c8a038…`、云端 146 行 seq 5377–5522 | — |
| `post-flash-state.txt` | 烧录后：`boot_seq=23`、`seq base=5633`、计数/内存/`system.loop` 摘要 | — |
| `flash-log-excerpt.txt` | esptool 原文：`Hash of data verified` / `Staying in bootloader` | — |
| `flash-target-72d7a610.bin` | 本次烧录件 | `72d7a610761c8dd76c4c5857d12615c839f19633bee0b2388cb9e3b330dd744c`（1 407 472 B） |
| **`rollback-firmware-a2c8a038.bin`** | **回退件** = 烧录前在跑的 `system.stats` 构建（`82123a3`） | `a2c8a0388e1e4b621a0e2e7d28a4b2269e1d84f96044211ace96532e61040b4f`（1 402 672 B） |

> `.log` 受 `.gitignore`（`*.log`）限制，同目录保留 `.gz`（`gunzip -k` 可复原）。

## 2. 回退（一条命令）

```bash
esptool.py --chip esp32s3 --port COM8 --baud 921600 \
  --before default_reset --after no_reset write_flash -z 0x10000 \
  docs/evidence/S-3-Flash-2026-10-20/rollback-firmware-a2c8a038.bin
```
回退后预期：`boot_seq` 23→24；`system.loop` 回 `Unknown system object`；`system.stats` 正常。

## 3. 只读复现（查询真机统计）

```bash
curl -sS -X POST https://guo-feeder-pagesdev.pages.dev/api/command \
  -H 'Content-Type: application/json' \
  -d '{"device_id":"288485896ce4","c":"system","i":"<唯一ID>","p":{"o":"loop"}}'
```

## 4. 本卷宗**没有**的东西

- 2 h 稳态 + 30 min 突发的正式门禁证据（**未授权本轮执行**）。
- `ENABLED=0/1` 的 A/B 开销对照（需再烧 1 次）。
- 人为"注入对照"（刻意制造已知长阻塞）。
