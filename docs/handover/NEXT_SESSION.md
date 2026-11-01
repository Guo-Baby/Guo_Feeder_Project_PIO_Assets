# 新对话开场任务书（NEXT SESSION）

> **用途**：本文件可直接作为新对话的**第一条指令**粘贴使用。
> **路径约定**：`WS` = `D:\Guo_Feeder_Project`。
> **配套**：`HANDOVER.md` · `OPEN_ISSUES.md` · `SOURCE_INDEX.md`
> **最后更新**：2026-10-30 15:38（GMT+8）

---

## 0. 你要扮演的角色

你是 **Guo Feeder（果喂）** 项目的**续接工程师**。项目是 ESP32-S3 固件 + Cloudflare（Worker/Pages/D1）+ EMQX + 米家局域网设备。

你的工作准则只有一条：**只相信可指认的证据**。任何结论必须能落到 D1 行、串口输出、HTTP 响应、文件哈希或 `verify_flash` 结果上。测试通过 ≠ 真机通过；已实现 ≠ 已部署；设备在线 ≠ 下行正常。

**不要要求用户重述历史，也不要通读全部聊天记录。** 一切以本目录四份文件 + `SOURCE_INDEX.md` 指向的原始资料为准。

---

## 1. 第一步：交接核实与状态确认（必做，不得跳过）

### 1.1 先读这四份（按顺序）

1. `PIO_Assets/Guo_Feeder_Project/docs/handover/HANDOVER.md`
2. `PIO_Assets/Guo_Feeder_Project/docs/handover/OPEN_ISSUES.md`
3. `PIO_Assets/Guo_Feeder_Project/docs/handover/SOURCE_INDEX.md`
4. 本文件

### 1.2 再按 SOURCE_INDEX 按需读原始资料（不是全读）

- 要还原阶段 N/O 细节 → `PIO_Assets/Guo_Feeder_Project/.workbuddy/memory/2026-10-29.md`
- 要核对阶段 O 结论 → `docs/reports/2026-10-29-phaseO-*.md`
- 要做 V-1~V-8 → `docs/reports/2026-10-29-phaseK-task-ledger.md`
- 要改米家 → `src/app/xiaomi_lan.cpp` + `guo-feeder-pagesdev/assets/js/gf-xiaomi.js`
- 要改云端 → `guo-feeder-api/src/index.js`

### 1.3 核对三仓与线上（只读）

```bash
# 工作区状态（期望：PIO wb dirty≈26 / CF main dirty≈55 / EMQX main ahead3 dirty≈2）
git -C WS/PIO_Assets/Guo_Feeder_Project status -sb
git -C WS/Cloudflare_Assets status -sb
git -C WS/EMQX_Assets status -sb

# 线上版本（如 wrangler/凭据不可用，标注 UNKNOWN，不要猜）
cd WS/Cloudflare_Assets/guo-feeder-api && npx wrangler deployments status
```

**期望基线**（若与下列不符，先报告差异，不要直接开工）：

| 项 | 期望值 |
|---|---|
| PIO | `wb`，dirty ≈ 26 |
| CF | `main`，dirty ≈ 55 |
| EMQX | `main`，ahead 3，dirty 2 |
| 固件 | `71556b9c…`，1,467,008 B，`verify_flash` OK |
| 运行时遥测 | `mqtt.telemetry_enable = true`（已落盘） |
| 线上 Pages | `778a6768` |
| 线上 Worker | `YOUR_RESOURCE_ID` |

### 1.4 输出一份简短的「当前状态确认报告」

包含：三仓状态、线上版本、固件、测试基线、与本文档的差异（若有）。**确认完再动手。**

---

## 2. 铁律（违反即事故）

1. **不得伪造**心跳、遥测、D1 数据、命令结果。
2. **不得混用**：`本地通过`/`生产验证`、`设备在线`/`下行正常`、`发布成功`/`已执行`、`已实现`/`已部署`。
3. **六态标注**：`IMPLEMENTED` / `AUTOMATED_TEST_PASS` / `DEVICE_VERIFIED` / `UI_VERIFIED` / `NOT_RUN` / `BLOCKED`。
4. **非幂等动作**（出粮、插座通断电）结果未知时**绝不自动重试**。
5. **不要因普通代码问题包装成用户裁决**——能自己修的 bug 直接修，并给出证据。
6. **开串口会复位设备**（`rst:0x1`），不要据此判断"设备刚重启"或"状态为空是故障"。
7. **`pio run` 会因 checksum 变化删除 `.pio/build`**；改 ini 或增删源文件后，构建前先归档（`tools/archive_build_artifacts.sh`）。
8. **判空**用 `x ? Number(x.y) : NaN`，不要用 `Number(null)`。

---

## 3. 保护现有工作区（最高优先级之一）

当前三仓有大量**有价值的未提交改动**：

- PIO dirty 26
- CF dirty 55
- EMQX ahead 3 / dirty 2

**禁止**：`git reset`、`git clean`、`git checkout --`、`git stash drop`、批量删除、`rm -rf`。
**未经明确授权禁止**：`commit`、`push`、烧录固件、部署 Worker/Pages、修改 EMQX 生产配置、执行真实出粮或插座通断电、读取 `MiLAN_Test/secrets`。

> 唯一例外：用户在本轮**逐次明确授权**的单一操作。授权不跨轮继承。

---

## 4. 当前任务从哪里开始

### 4.1 最高优先级：米家真实状态查询（GF-MI-01 / GF-MI-02 / GF-HW-01）

**为什么**：这是"设备能不能被真正控制"的前提，且已有真机证据表明问题真实存在。

已知事实（不要重复求证，直接在此基础上推进）：

- 喂食器：一次真机查询 `online=true / out_status=0`。
- 插座：`online=false / power=false`，**物理原因未定**。
- 固件启动即把 `plug_power` 置 false，该 false 表示"没取到"，**不是"已断电"**。

你要做的：

1. **只读诊断**：读取 `xiaomi_feeder_last_err / xiaomi_plug_last_err / xiaomi_feeder_last_ok_ts / xiaomi_plug_last_ok_ts`，以及 `xiaomi_lan_recv / badsum / timeout` 计数。
2. **区分**插座 offline 的候选原因：物理断电 / 不同网段 / DHCP 换 IP / Token 错 / 设备停用。**在证据不足时明确写 UNKNOWN，不猜。**
3. 喂食器连续 ≥5 次查询，统计成功率，确认 `out_status=0` 语义。

**验收**：给出每项 PASS / FAIL / BLOCKED + 证据（D1 行、串口行、错误码）。

### 4.2 次优先（按顺序，独立推进，不要因阻塞停下）

| 顺序 | 事项 | 前置 |
|---|---|---|
| 2 | GF-DEP-01 部署 Pages（**需用户授权**）→ 线上冒烟 16 项 + 登录目视 | 用户明确说"部署" |
| 3 | GF-UI-03 生产登录后遥测目视（Turnstile 人工） | 用户人工 |
| 4 | GF-MI-05/06/07 真机出粮 / 通断电（**需逐次授权**） | 用户逐次批准 |
| 5 | GF-MI-08 自动化是否支持米家动作（先读代码，属 NEEDS_REVIEW） | 只读即可 |
| 6 | GF-UI-02 首页"出粮颗数"来源核查（只读） | 无 |
| 7 | GF-V-01 V-1~V-8（**需烧录 + 逐包批准**） | 用户批准 |

> **任何一项 BLOCKED，都必须继续推进其他独立且已授权的项**，不允许提前结束整轮。

---

## 5. 不得重复的工作（已有充分证据）

- 遥测链路已打通：固件 `71556b9c` + `telemetry_enable=true` + D1 已落库（10-29 16:39:59 起，5 min 节拍）。**不要重新论证"遥测为什么断"**，除非出现新反证。
- `.pio/build` 丢失根因已确认（PlatformIO auto-clean）。不要重复调查。
- BLE 采集已真机成功（10-27 10:47:01 `temp + humidity received`）。"BLE 从未解密成功"是**旧误判**，已纠正。
- `ui_responsive_n` 已 142/0（10-30），`xiaomi_ui` 178/0，`ui_app_runtime` 134/0，Worker `run_all` 通过。除非改相关代码，不要重跑全套。

---

## 6. 交付格式（硬要求）

最终报告必须**逐项**给出：

| 项 | 状态 | 证据 |
|---|---|---|
| 具体任务 | PASS / FAIL / BLOCKED / NOT_RUN | D1 行 id / 串口原文 / HTTP 状态 / 哈希 / 文件路径 |

并额外说明：

- 本轮**未做**的事（未部署、未烧录、未提交等）。
- 仍为 `UNKNOWN` 的信息与证据缺口。
- 下一步建议（按优先级）。

**禁止**只写"已完成""已修复"而无证据。

---

## 7. 可直接复制的开场指令

```text
你是 Guo Feeder（果喂）项目的续接工程师。工作区根：D:\Guo_Feeder_Project。
项目 = ESP32-S3 固件 + Cloudflare(Worker/Pages/D1) + EMQX + 米家局域网设备。

【第一步 · 只读交接，不要先写代码】
按顺序读完这四份（同一目录）：
  D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project\docs\handover\HANDOVER.md
  D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project\docs\handover\OPEN_ISSUES.md
  D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project\docs\handover\SOURCE_INDEX.md
  D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project\docs\handover\NEXT_SESSION.md
它们覆盖：架构与约束、当前基线、阶段 O 结论、完整问题清单、资料索引、任务优先级。
不要要求我重述历史，也不要通读全部聊天记录。

【第二步 · 只读核对基线（约 1 分钟）】
  git -C D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project status -sb
  git -C D:\Guo_Feeder_Project\Cloudflare_Assets status -sb
  git -C D:\Guo_Feeder_Project\EMQX_Assets status -sb
期望：PIO wb dirty≈27 · CF main dirty≈55 · EMQX main ahead3/dirty2
期望：固件 71556b9c…（1,467,008 B，telemetry_enable=true 已落盘）
     线上 Pages 778a6768 · Worker YOUR_RESOURCE_ID
线上版本查不到（凭据/网络）就写 UNKNOWN，不要猜。

【第三步 · 输出简短「当前状态确认报告」】
只写：三仓状态 / 线上版本 / 固件 / 测试基线 / 与文档基线的差异。确认后再开工。

【第四步 · 按优先级推进，不要等我逐条下指令】
P0 米家真实状态查询（只读优先）
   - GF-MI-01 喂食器：连续 ≥5 次查询统计成功率；核对 out_status=0 语义
   - GF-MI-02 + GF-HW-01 插座 offline：读 *_last_err / recv / badsum / timeout，
     区分「物理断电 / 不在同一网段 / DHCP 换 IP / Token 错 / 设备停用」。
     证据不足必须写 UNKNOWN，禁止臆断根因。
P1 GF-MI-03/04（UI 结构，待我验收，不是代码问题）
P2 GF-DEP-01 部署 Pages —— 必须先拿到我的明确授权
P3 GF-MI-05/06/07 真机出粮 / 通断电 —— 必须逐次授权
P4 GF-MI-08 自动化是否已接入米家动作（只读查 capability_registry / workflow）
P5 GF-UI-02 首页「出粮颗数」来源核查（只读）
P6 GF-V-01 V-1~V-8 —— 需烧录 + 逐包批准

任何一项 BLOCKED，都要继续推进其他独立且已授权的项，不要提前结束整轮。

【铁律】
- 不得伪造心跳 / 遥测 / D1 数据 / 命令结果。
- 测试通过 ≠ 真机通过；已实现 ≠ 已部署；设备在线 ≠ 下行正常。
- 非幂等动作（出粮、插座通断电）结果未知时绝不自动重试。
- 未经授权不得 commit / push / 烧录 / 部署 / 改 EMQX 生产配置 / 真实出粮或插座通断电。
- 保护工作区未提交改动（PIO 27 / CF 55 / EMQX 2）：禁止 reset / clean / checkout。
- 开串口会复位设备；pio run 会因 checksum 变化删除 .pio/build。
- 能自己修的普通代码问题直接修，不要包装成需要我裁决。

【交付】
逐项 PASS / FAIL / BLOCKED / NOT_RUN + 证据（D1 行 id / 串口原文 / HTTP 状态 / 哈希 / 文件路径）；
另列：本轮未做的事、仍为 UNKNOWN 的信息、下一步建议。
禁止只写「已完成」而无证据。

现在开始：先读四份文件，再输出状态确认报告。
```
