# S-3 证据卷宗（`loop()` 耗时观测实施 + 本地验收）

> 采集窗口：`2026-10-19T18:05Z – 18:32Z`（本地 `2026-10-20 02:05 – 02:32`）
> 关联报告：`docs/architecture/S-3-Implementation-and-Local-Acceptance-Report.md`
> 规格修订：`docs/architecture/S-3-Spec-Revision-R2.md`
> ★ 本卷宗**不含任何设备数据**：本包全程未烧录、未开串口、未发命令。

## 1. 文件清单

| 文件 | 说明 | SHA-256 / 关键字段 |
|---|---|---|
| `source.diff` | 完整源码差异（10 文件 / +1351 / −0） | `055717b0b1d40973e110c950f8cb819235f754fb860e1e1d01bf0f0051175612` |
| `s3-loopstats-firmware.bin` | **交付件**（`GF_LOOP_STATS_ENABLED=1`），1 407 472 B | `72d7a610761c8dd76c4c5857d12615c839f19633bee0b2388cb9e3b330dd744c` |
| `s3-loopstats-CONTROL-off-firmware.bin` | **对照件**（`=0`，零开销），1 406 752 B | `b7ce129958faf6d60562797607746acbba3095036233e0f7f8e18e1e4e6cb9a5` |
| `build-clean.log` / `.gz` | 干净全量构建（417 编译单元，263 s，`SUCCESS`） | gz 12 691 B |
| `build-incremental.log` / `.gz` | 增量构建（2 526 B） | — |
| `build-control-off-incremental.log` / `.gz` | 对照构建（2 681 B） | — |
| `build-metrics.txt` | RAM/Flash/段尺寸/三组增量/可复现性/告警对照 | 见文件 |
| `host-probe.log` | 宿主机探针 **82/82 PASS** 完整输出 | `ae8057da3fab7904ed9ada0947cb796256f289d94b71cab6ddf3d19697991f1f` |
| `platformio.ini.snapshot` | 构建配置快照（含 `-DGF_LOOP_STATS_ENABLED=1`） | — |
| `git-status.txt` | 提交前工作区快照 | — |

> `.log` 受 `.gitignore`（`*.log`）限制，同目录已保留 `.gz`（`gunzip -k` 可复原）。
> ★ `S-2` 纪律：本次两个固件 **`.bin` + SHA-256 + 源码 diff + 构建配置 + 构建日志** 五项齐全，且归档在**仓库内**（不是临时 worktree）。

## 2. 三个构建的资源对照

| 构建 | RAM (PIO) | Flash | `.bin` | text | data | bss |
|---|---|---|---|---|---|---|
| `ENABLED=1`（交付） | 133 344 (40.7%) | 1 407 105 (67.1%) | 1 407 472 | 1 134 430 | 288 940 | 1 224 805 |
| `ENABLED=0`（对照） | 132 744 (40.5%) | 1 406 393 (67.1%) | 1 406 752 | 1 133 798 | 288 860 | 1 224 221 |
| 上一版生产（`system.stats` / boot 22） | 132 744 (40.5%) | 1 402 305 (66.9%) | 1 402 672 | 1 129 218 | 287 476 | 1 224 221 |

**采样器本体**（`loop_stats.cpp.o`）：`ENABLED=1` → `text 1 736 / data 8 / bss 586`；`=0` → `text 875 / data 0 / bss 0`。
**结论**：宏关闭 ⇒ **零静态 RAM**（`bss` 与上一版生产完全相等），不是"留数组只关逻辑"。

## 3. 只读复现命令

```bash
# 宿主机单测（82 项）
bash test/loop_stats_host/run.sh

# 交付件构建（独立目录，不污染既有构建）
PLATFORMIO_BUILD_DIR=.pio/build/s3loop pio run

# 清零对照构建
PLATFORMIO_BUILD_DIR=.pio/build/s3loop_off \
  PLATFORMIO_BUILD_FLAGS="-DGF_LOOP_STATS_ENABLED=0" pio run

# 哈希核对
sha256sum .pio/build/s3loop/esp32-s3-devkitc-1/firmware.bin
# 期望：72d7a610761c8dd76c4c5857d12615c839f19633bee0b2388cb9e3b330dd744c

# 热路径违禁项静态审查（先剥离注释，否则会命中文件头"禁止清单"）
python - <<'PY'
import re
s=open('src/app/loop_stats.cpp',encoding='utf-8').read()
s=re.sub(r'/\*.*?\*/','',s,flags=re.S); s='\n'.join(l.split('//')[0] for l in s.split('\n'))
for p in ['Serial','printf','log_emit','ESP_LOG','new ','malloc','String','std::','delay(','vTaskDelay','micros(']:
    print(p, s.count(p))
PY
```

## 4. 本卷宗**没有**的东西（避免误判）

- **没有** `system.loop` 的真实响应（设备未烧录）。
- **没有**目标板的采样开销 / 临界区耗时实测值（`snap_crit_max_us` 需上板）。
- **没有** P99 数值 ⇒ **没有任何** L2/L3 结论。
