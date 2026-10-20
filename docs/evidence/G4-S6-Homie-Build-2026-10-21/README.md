# S-6 · Homie 启用构建矩阵（`2026-10-21 03:22–03:49`）

> **性质**：**只编译，不烧录**。本目录**只**能证明「Homie 启用配置能编过、占多少资源」。
> ❌ **不得**据此写 Homie 功能 PASS —— 该模块**从未上板运行过**（`GF_HOMIE_ENABLED` 生产默认 `0`）。

## 构建矩阵

| 配置 | 结果 | RAM | Flash | firmware.bin | 构建耗时 |
|---|---|---|---|---|---|
| **基线**（`GF_HOMIE_ENABLED` 默认 `0`） | ✅ `BUILD PASS` | **134 720 B**（41.1%） | **1 416 605 B**（67.5%） | 1 416 976 B | 130.68 s |
| **Homie 启用**（`-DGF_HOMIE_ENABLED=1`） | ✅ `BUILD PASS` | **135 624 B**（41.4%） | **1 427 757 B**（68.1%） | 1 428 128 B | 222.78 s |
| **增量** | — | **+904 B** | **+11 152 B** | **+11 152 B** | +92.1 s（全量重建，非增量成本） |
| **相对增量** | — | **+0.67%** | **+0.79%** | +0.79% | — |

> 分母说明：RAM 百分比的分母是 **327 680**（SRAM 总量，PlatformIO 惯例）；
> 这与 `G-4` 的 DRAM 占用率（分母 `internal.total` ≈ 251 KB，见 `R-3`）**不是同一口径**，不可互相换算。

## ★ 宏真的生效了吗（防"静默忽略"）

只证明"能编过"不够 —— 必须证明 `-D` 真进了编译器和链接产物：

| 证据 | 结果 | 文件 |
|---|---|---|
| 编译命令行含该宏 | `.pio/build_homie/.../*.tmp` 中命中 **386 次** `-DGF_HOMIE_ENABLED=1` | `macro-evidence.txt` |
| **链接产物 homie 符号数** | 基线 ELF **2 个** → 启用后 **21 个** | `elf-symbol-proof.txt` |
| 仅启用版出现的符号 | `homie_desc_build` · `homie_publish_state` · `homie_publish_values` · `homie_parse_set_topic` · `homie_property_available` … | 同上 |
| 资源增量非零 | RAM +904 B / Flash +11 152 B | 上表 |

⇒ 三项互相独立地证明：**启用版真的把 2 641 行实现编进了镜像**，不是"宏被忽略所以两者相同"。

## 构建环境（可复现）

| 项 | 值 |
|---|---|
| PlatformIO Core | **6.2.0** |
| 平台 | `espressif32@7.0.1`（已锁定，非 `@latest`） |
| board | `esp32-s3-devkitc-1` |
| 依赖 | `U8g2@2.36.18` · `ArduinoJson@7.4.3` · `HX711@0.7.5` · `NimBLE-Arduino@2.5.1`（全部精确版本） |
| 启用方式 | `PLATFORMIO_BUILD_FLAGS="-DGF_HOMIE_ENABLED=1"` |
| 独立构建目录 | `PLATFORMIO_BUILD_DIR=.pio/build_homie`（★ 见下） |

## 过程中的一次真实阻塞（已绕过，留档）

第一次用默认构建目录 + 该宏构建时，PlatformIO 因 build flags 变化要**全量清理**旧对象，
触发本机沙箱的**批量删除保护**：

```
[safe-delete][SAFE_DELETE_BULK_CONFIRM_REQUIRED] {"count":50,"threshold":50,"scope":"turn", ...}
```

⇒ 构建停在中途（未产出镜像）。**处置**：改用 `PLATFORMIO_BUILD_DIR=.pio/build_homie` 独立目录，
不删除任何既有对象 ⇒ 构建一次通过（`build-homie-on.log`）。
★ **未**通过关闭保护或删除文件来"强行通过"。

## 文件索引

| 文件 | 内容 |
|---|---|
| `build-baseline.log` | 基线构建全量日志（`SUCCESS` / RAM / Flash） |
| `build-homie-on.log` | Homie 启用构建全量日志（`SUCCESS` / RAM / Flash） |
| `macro-evidence.txt` | 宏进入编译命令行的证据 + 目标文件清单 |
| `elf-symbol-proof.txt` | 两个 ELF 的 homie 符号数对比（2 → 21） |
| `ab-single-file.txt` | 单文件 A/B 尝试（❌ 失败，见下） |

> `ab-single-file.txt` 记录的是一次**失败的**尝试：手工复刻 SCons 编译命令时因引号/相对路径
> 还原不完整导致 `g++` 退出码 1，**未产出对象**。该文件保留作"此路不通"的留痕，
> 结论改由 `elf-symbol-proof.txt` 承担。**不**用它当任何正面证据。

## 局限（必须随结论引用）

- **未烧录、未上板**：`$description` 体积、实际 MQTT 报文量、tile 渲染全未验证。
- RAM +904 B 是**静态**增量；运行时还有 `$description` 字符串缓冲、pending table 等动态分配，**未测**。
- 增量会吃掉 `R-3` 的 85% DRAM 余量（约 7 pp ≈ 17 KB）中的一小部分，但真正风险在**运行时**而非静态。
