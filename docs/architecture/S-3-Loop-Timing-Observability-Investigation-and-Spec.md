# S-3 · `loop()` 耗时观测能力调查与实施规格（**只读 · 未实施**）

> 任务：为正式 `G-4` 补齐 `loop()` 耗时 P99 的**观测能力**。
> 本轮范围：**只读调查 + 实施规格**。**未修改代码、未构建、未烧录、未复位、未改配置/库/部署、未触发硬件动作。**
> 执行时间：`2026-10-20 01:58 – 02:2x`（本地）
> 状态影响：`G-4` / `WP-5` / `BT-1` / `STOP-R1` **均未改动**。

## 0. 交付物索引与本轮结论

| 要求 | 对应章节 |
|---|---|
| ① 现状与证据 | §1 |
| ② 实施规格与验收标准 | §2 · §5 · §6 |
| ③ 风险及回滚 | §7 |
| ④ 待人工裁决 | §8 |
| ⑤ 变更声明 | §0.2 |

### 0.1 一句话结论

**`loop()` 耗时观测能力当前完全不存在（`src/` 内 `micros()` / `esp_timer_get_time` 命中数 = 0）**；
因此 `G-4` 的 `loop()` P99 项既没有历史基线、也无法在现有固件上测得 —— 这是**能力缺口**，不是"指标不合格"。
同时**门禁本身存在两套互不替代的口径**（绝对 `P99 < 20 ms` vs 相对 `劣化 < 20%`），必须人工裁决后才能定"达标"（§4.2 / §8）。

### 0.2 变更声明（必须精确）

| 类别 | 本轮是否产生变更 |
|---|---|
| 源代码 | **否**（0 行改动） |
| 构建产物 / 固件 | **否**（未执行 `pio run`） |
| 烧录 / 设备复位 | **否**（未开串口、未发任何命令） |
| 设备状态（`boot_seq` / 计数器 / 配置） | **否**（全程未与设备交互） |
| 数据库（D1）/ 云端部署 | **否**（未执行任何写操作或部署） |
| 生产配置 / EMQX 配置 | **否** |
| `G-4` / `WP-5` / `BT-1` / `STOP-R1` 状态 | **否**（未改动任何状态文档） |
| 文档 | **是**：仅**新增**本文件 `docs/architecture/S-3-Loop-Timing-Observability-Investigation-and-Spec.md` |
| 既有证据 | **否**（`docs/evidence/` 未触碰） |

---

## 1. 现状与证据

### 1.1 主循环结构

`src/main.cpp:668 void loop()` 是**纯协作式超循环（cooperative superloop）**，顺序调用 20 个模块的 `*_task()`：

```
system_command_task → wifi_task → event_dispatch → time_task → cloud_task →
credential_manager_task → command_manager_task → homie_task → config_task →
workflow_task → oled_task → weight_task → valve_task → computer_reset_task →
MiThermometer_task → telemetry_task → test_mqtt_task → log_task →
serial_debug_command_process
```

| 事实 | 证据 |
|---|---|
| **无 `vTaskDelay`、无 `yield()`、无固定周期** | `grep -c 'vTaskDelay\|delay(' src/main.cpp` → 仅 `setup()` 内 `delay(1000)`（`main.cpp:446`）；`grep -rc 'yield()' src/` = **0** |
| **整个 `src/` 内没有任何 `xTaskCreate`** | `grep -rn 'xTaskCreate' src/` → 0 命中 |
| 即：主循环**跑多快就多快**，单轮耗时直接等于"两个模块调用的最大间隔" | 结构推论（无节流器） |

⇒ **测量口径天然清晰**：`loop()` 的"单轮耗时"就是**相邻两次进入 `loop()` 的时间差**，无需区分"执行时间"与"等待时间"（没有等待）。★ 但仍需人工确认（§8 `Q-7`）。

### 1.2 任务调度与并发上下文（影响采样的线程安全）

| 上下文 | 谁会改统计 | 说明 |
|---|---|---|
| `loopTask`（Arduino 主任务） | ✅ 唯一 | 采样器写者只在这里 |
| esp-mqtt 任务 | ❌ 不写采样器 | `cloud_manager.cpp:1095` 在 `mqtt_event_handler()` 内**同步执行** `command_manager_execute()`；`system.stats` / 未来的 `system.loop` **在这个上下文读**统计 |
| 系统任务（Wi-Fi / TCP-IP / esp_timer） | ❌ 不写 | — |

⇒ **单写者 + 单读者 + 跨上下文**：这是典型的"写热、读冷"模式，用 **无锁写 + 短临界区读** 即可（§3）。

### 1.3 耗时敏感路径（长尾来源清单）

| 类别 | 代表位置 | 为什么可能长尾 | 量级（估算，未实测） |
|---|---|---|---|
| **串口同步输出** ★ | `Serial.*` 共 **520 处** | Arduino `Serial` 是**同步阻塞**写；115200 8N1 = **86.8 µs/字节** | 一行 100 B ≈ **8.7 ms**；200 B ≈ **17.4 ms** ⇒ **单次打印就能吃掉 20 ms 阈值的 43%~87%** |
| LittleFS 读写/段管理 | `LittleFS.` 共 **143 处**（`log_manager.cpp` 为主） | 擦除一个 4 KB 段可达数十 ms（与擦除块对齐有关） | 段删除/新建时最可能出长尾 |
| NVS 提交 | `nvs_` / `Preferences` 共 **31 处** | `nvs_commit` 会写 flash，**百 ms 级**风险 | 配置 `set`+`save`、凭据 promote 时 |
| TLS/CA 握手 | `cloud_manager` 连接路径 | mbedTLS 握手 + RSA 运算 | 百 ms 级，但只在重连时 |
| CBOR 编解码 | 日志批次 / 遥测 | 纯 CPU，`log` 批次 ≤ 4 KB | 亚 ms ~ 数 ms |
| OLED I2C | `oled_task()` | I2C 阻塞传输 + 帧延时配置 | 每帧数 ms，频率受限 |
| HX711 读取 | `weight_task()` | ★ `weight.cpp:409` 注释记载过 `while(!is_ready()) delay(0)` **无超时无上界**的历史坑（已修，但同族风险存在） | 不可预知 |

> ★ **关键推论**：`loop()` 长尾的**第一嫌疑是串口打印**，不是网络。这直接决定采样器**绝不能逐样本输出**（§3.6）。

### 1.4 现有统计设施（可复用的模式）

| 设施 | 位置 | 结构 | 读出口 |
|---|---|---|---|
| `LogStats`（POD，≈30 个 `uint32/uint8`） | `log_manager.h:167` | `static LogStats s_stats`（`log_manager.cpp:160`）+ `portMUX_TYPE s_mux`（`:163`） | `log_get_stats()` / `log_stats_get()` → `system.stats` |
| `LogCloudInfo` / `LogFlashInfo` | 同上 | 同族 POD | `system.stats` 的 `queue` / `flash` 段 |
| 内存水位 | `system_command.cpp` `syscmd_memory()` | `ESP.getHeapSize/getFreeHeap/getMinFreeHeap/getMaxAllocHeap` | `system.memory` |

**可复用**：`portMUX_TYPE` + 单临界区整体拷贝（`log_stats_get()` 已经是这个范式，`+158 行`那次提交）。
**缺**：`LogStats` 里**没有任何耗时字段**；`src/` 内**没有任何计时调用**。

### 1.5 串口输出机制（单列，因为它同时是"观测手段"和"测量污染源"）

- 全部走 Arduino `Serial`（`Serial.print/printf` **520 处**），**同步阻塞**；
- 115200 8N1：每字符 **86.8 µs**（10 bit / 115200 s）；
- 因此**"用日志观测 loop 耗时"本身会显著改变 loop 耗时** —— 门禁原文也警告过
  "**埋点本身会扰动 496 环计数，优先用 INFO**"（`BT-1-Log-ACK-…Plan.md:895`，`496` = Flash 环 16 段 × 31 条）。

### 1.6 证据清单（可复现的只读命令）

```bash
grep -n "void loop" src/main.cpp                     # 668
grep -rc "vTaskDelay\|delay(" src/main.cpp           # 1（setup 内 delay(1000)）
grep -rn "yield()" src/ | wc -l                      # 0
grep -rn "xTaskCreate" src/ | wc -l                  # 0
grep -rn "micros()\|esp_timer_get_time" src/ | wc -l # 0   ← 观测能力缺口的直接证据
grep -rn "Serial\.print\|Serial\.write\|Serial\.printf" src/ | wc -l   # 520
grep -rn "LittleFS\." src/ | wc -l                   # 143
grep -rn "nvs_\|Preferences" src/ | wc -l            # 31
grep -rn "LOG_DRAIN_MAX_PER_TASK" src/log/log_manager.h  # 8
```

---

## 2. 采样设计

### 2.1 采样点（单点，`loop()` 入口）

```
loop():
  t_now = esp_timer_get_time()      # 64-bit µs
  if (have_prev) sample(t_now - prev_us)
  prev_us = t_now
  ... 原有 20 个 task 调用 ...
```

- 样本 = **相邻两次 `loop()` 入口的间隔**（= 上一轮完整周期，含本轮入口开销）。
- **只取 1 个采样点**：多取（每个模块前后各一次）会把 20 次计时调用塞进热路径，且需要 20 组直方图 ⇒ 收益不抵开销。**分模块耗时留给下一阶段**（§8 `Q-10`）。
- 首个样本丢弃（`have_prev=0` 时只记 `prev`）。

### 2.2 计时源与精度

| 项 | 选择 | 理由 |
|---|---|---|
| 计时源 | **`esp_timer_get_time()`**（`int64_t`，µs） | 硬件计数器，分辨率 1 µs；**64 位不回绕** |
| **禁用** `micros()` | — | Arduino `micros()` 基于 `uint32_t` µs ⇒ **71.58 min 回绕**；2 h 门禁窗口**必然**溢出，差分会产生负值/巨值 |
| 单次调用开销 | ≈ **0.5–1.5 µs**（函数调用 + 64 位读） | 需在 L2 有效性校验里实测（§6） |
| 差分保护 | `if (dt <= 0 || dt > 60s) → anomaly++` 并跳过 | 防异常值污染 |

### 2.3 直方图（固定内存，无分配）

| 参数 | 取值 | 依据 |
|---|---|---|
| 量程 | **0 – 51.2 ms** | 覆盖绝对阈值 `20 ms` 的 2.5 倍余量 |
| 桶数 | **256** | 512 B 便于单临界区拷贝 |
| 桶宽 | **0.2 ms** | = 阈值的 1%（20 ms 时误差 ≤ 0.2 ms） |
| 元素类型 | `uint16_t`（饱和于 65535） | 单桶 65535 次 ≈ 在 1 kHz 下 65 s 满 ⇒ 需 §2.4 的饱和策略 |
| 溢出桶 | `over_cnt`（`uint32`）+ `max_us`（`uint32`，**精确最大值**永不受直方图分辨率限制） | >51.2 ms 的长尾仍可判读 |
| 附加计数 | `n`（`uint32`，饱和保护）、`sum_us`（`uint64`）、`min_us`、`max_us`、`anomaly` | 供均值/极值交叉校验 |

> 桶饱和的处理：单桶 `uint16` 满后**不再自增**并置 `sat=1`（对外暴露），此时该窗口的 P99 只能降级为"≥ 该桶下界"——**必须显式标注降级**，不得静默。

### 2.4 统计窗口与 P99 计算（**差分法，不引入写命令**）

- 计数器**自 boot 起累积**（与 `emit_total` 同语义）；
- 窗口 = **两次快照之间**：`Δhist[i] = after.hist[i] − before.hist[i]`；
- `N = Σ Δhist`（不含 `over_cnt` 的溢出样本时需单列）；
- **P99 = 最小桶 `i`，使 `Σ_{j≤i} Δhist[j] ≥ 0.99 × N`**，取值 `(i+1) × 0.2 ms`（保守取桶上界）；
- 若 `Δover_cnt > 0` ⇒ 同时报 `P99 = "≥51.2 ms"` 与精确 `max_us`，**不允许只报 51.2 ms**。

> 这样**不需要 `stats_reset` 写命令**（避免新增写操作/破坏"只读"性质），代价是调用方要保存 before 快照。
> 若将来要"一键窗口"，再加 `system.loop_reset`（属**写**操作，须独立授权，见 §8 `Q-6`）。

### 2.5 样本量要求

| 目标 | 要求 | 推导 |
|---|---|---|
| 单次 P99 可判读 | **N ≥ 10 000** | P99 尾部样本 = 1% ⇒ ≥100 个尾部样本，标准误量级可接受 |
| 绝对阈值判定（20 ms） | 同上 + `sat=0` | 桶饱和会使 P99 失真 |
| 正式门禁证据 | **连续 2 h 稳态 + 30 min 突发**（门禁原文） | 1 kHz 下 2 h ≈ 7.2×10⁶ 样本，`uint32 n` **不溢出**（4.29×10⁹ ≈ 49.7 天 @1 kHz） |
| 样本量自检 | `n / Δuptime ≈ 1 / P50` | 用于 L2 有效性（§6） |

### 2.6 输出频率

- **仅按需**：`system.loop`（只读）返回聚合快照；**禁止**周期性打印/上报；
- 每次查询产生 **1 条** INFO 日志（沿用 `system.stats` 的做法），不会进入 496 环的高频扰动区；
- 建议查询节奏：场景开始时 1 次（before）+ 结束时 1 次（after），**不是**每 10 s 轮询。

### 2.7 统计开销预算（每轮）

| 动作 | 估算 |
|---|---|
| `esp_timer_get_time()` × 1 | 0.5–1.5 µs |
| 64 位减法 + 分支 + 索引计算 | ≈ 20–40 周期 @240 MHz ≈ 0.08–0.17 µs |
| 直方图 `++` + `n/sum` | ≈ 10–20 周期 ≈ 0.04–0.08 µs |
| **合计** | **≈ 1.0–1.8 µs / 轮** |

相对影响：若 `P50` 为 1 ms ⇒ **0.10–0.18%**；若为 10 ms ⇒ 0.01–0.018%。均远小于 20% 劣化阈值。
★ 但**必须实测**（§6 L2），不得以估算代替测量。

### 2.8 参考伪代码（仅示意，非实现）

```cpp
// 静态存储，编译期定容；internal RAM（不用 PSRAM，避免 cache miss 抖动）
static uint16_t s_hist[256];
static uint32_t s_n, s_over, s_anom; static uint64_t s_sum;
static uint32_t s_min_us, s_max_us; static int64_t s_prev;
static portMUX_TYPE s_loopmux = portMUX_INITIALIZER_UNLOCKED;

void loop_stats_tick(void) {              // loop() 第一行调用
    const int64_t now = esp_timer_get_time();
    if (s_prev != 0) {
        const int64_t dt = now - s_prev;
        if (dt <= 0 || dt > 60000000) { s_anom++; }
        else {
            const uint32_t us = (uint32_t)dt;
            if (us < 51200) { auto &b = s_hist[us / 200]; if (b < 0xFFFF) b++; else s_sat = 1; }
            else s_over++;
            s_n++; s_sum += us;
            if (us < s_min_us) s_min_us = us;
            if (us > s_max_us) s_max_us = us;
        }
    }
    s_prev = now;
}
// 读：单个临界区内整体拷贝 512 B + 标量（≈2–8 µs，见 §3.3）
```

---

## 3. 无分配 / 非阻塞 / 低锁竞争 / 实时性

### 3.1 完全静态分配

- 直方图 512 B + 标量 ≈ **544 B**，**编译期定容**；`static` 对象，**无 `new`/`malloc`/`std::vector`**。
- ★ **故意不用 PSRAM**：PSRAM 访问经 cache，可能因 cache miss 产生**数十 µs 抖动**，会污染被测对象本身。内部 RAM 更稳。
- 与现有铁律一致：>1 KB 才考虑 PSRAM，本结构 **< 1 KB**。

### 3.2 热路径无锁

- **单写者**（`loopTask`）⇒ 直方图自增 **无需任何锁**；不调用 `portENTER_CRITICAL`；
- 不做读-改-写跨任务的复合操作 ⇒ 不会与 `s_mux`（LogManager）产生**锁顺序问题**（两者完全独立）。

### 3.3 读路径（短临界区，有上界）

- 读者在 **esp-mqtt 任务**；写者在 `loopTask` ⇒ 需保证"拷贝期间不自增"。
- 方案：**单临界区一次性拷贝 544 B**。成本估算：`memcpy` 512 B @240 MHz 约 128–256 周期（≈0.5–1.1 µs）+ 临界区进出 ≈ 1–2 µs ⇒ **总计 2–4 µs**（关中断时长同等量级）。
- 预算上限：**≤ 10 µs**。若实测超 10 µs，改为**分块拷贝**（8 × 64 B，每块一个临界区）或**双缓冲 + 序号校验**（seqlock）。★ 这是实施期的强制校验点（§5 `S0`）。
- 对比：`system.stats` 现有 `log_stats_get()` 已用同样范式（单临界区整体拷贝），**无新增风险模式**。

### 3.4 不阻塞输出

- 采样器**自身零输出**：不调 `Serial`、不调 `log_emit`、不调 MQTT；
- 输出只发生在**命令响应**里（esp-mqtt 任务、本来就要发一帧），与 `loop()` 解耦；
- 因此**不会**出现"观测手段把被测对象拖长"的自指污染（对比 §3.6）。

### 3.5 对 ESP32-S3 实时性的影响分析

| 面 | 影响 | 结论 |
|---|---|---|
| 每轮固定开销 | 1.0–1.8 µs（§2.7） | 相对 20 ms 阈值 **≤0.009%**，可忽略 |
| 中断延迟 | 热路径**不关中断**；仅读路径关 2–4 µs | 对 Wi-Fi/TCP 影响可忽略（ESP-IDF 建议单次 <20 µs） |
| 内存 | +544 B 内部 RAM（静态） | 相对 251 KB heap 占 **0.22%**；不影响 `minimum_free` 判读口径（但会**永久**抬高一次基线，见 §8 `Q-9`） |
| Flash 磨损 | **0**（不落盘） | — |
| 调度 | 不加任务、不加信号量、不改优先级 | 无 |
| 可观测性副作用 | 直方图会**改变** `loop()` 自身耗时分布（+1.2 µs） | 报告 P99 时须声明"含采样开销"；如需纯净值，用 `GF_LOOP_STATS_ENABLED=0` 的同源构建对照（§5 `S0`） |

### 3.6 反例：为什么**不能**逐样本 `log_emit`

| 反例 | 代价 |
|---|---|
| 每轮打一条 INFO | 每轮 +1 条日志 ⇒ 1 kHz 下 1000 条/s，远超 `LOG_CLOUD_QUEUE_SLOTS=128` 与 496 环容量；`drop_overflow` 会瞬间起飞，**测出来的丢弃是测量造成的** |
| 每轮 `Serial.printf` | 一行 100 B ≈ **8.7 ms** ⇒ 直接把 `P99` 从"µs 级"推到"ms 级"，**测量结果全是测量本身** |
| 每 N 轮打印一次 | 仍有 8.7 ms 尖峰，且恰好构成**人造 P99.9 长尾** |

⇒ **只允许"聚合后按需输出"**，这是本设计的硬约束。

---

## 4. 现行门禁定义核实（`loop()` P99）

### 4.1 权威出处（两处，**互不替代**）

**出处 A**：`docs/review/BT-1-Log-ACK-Read-Only-Investigation-and-Fix-Plan.md` **§6.2「CPU」行（`:895`）**，原文：

> `loop()` 单轮耗时 P50/P99；`cloud_poll()` 单次耗时 ｜ 测量方法：`micros()` 埋点 + `log` 侧信道（**注意：埋点本身会扰动 496 环计数**，优先用 INFO） ｜ 建议时长：连续 **2 h** 稳态 + 30 min 突发 ｜ 阈值：**`loop()` P99 < 20 ms**（现无基线，需先测**修复前**同一指标做对照）

**出处 B**：同文件 **§8.1 表（`:936`）**，原文：

> `loop()` P99 ｜ 修复前：**待测**（当前无基线） ｜ 修复后：**劣化 < 20%** ｜ 阻塞门禁：**是**

**关联条目**：§6.2「竞争」行（`:900`）—— "任何**非日志模块**的 P99 延迟**不得**比修复前劣化 **> 20%**"。

### 4.2 口径冲突（★ 必须裁决，本规格不自行选择）

| # | 冲突点 | 说明 |
|---|---|---|
| 1 | **绝对阈值 vs 相对劣化** | A 说 `P99 < 20 ms`（绝对值）；B 说"劣化 < 20%"（相对值）。二者**不可同时用一句"通过"覆盖**：可能 12 ms 但比基线劣化 30%（A 过 B 不过），也可能 25 ms 但与基线持平（A 不过 B 过） |
| 2 | **相对口径的分母不存在** | B 的分母是"**修复前**"（`BT-1` 修复前固件）的 P99；但**当时没有任何埋点**（`src/` 计时调用 = 0），**该基线永久不可获取**，也无法回退到修复前固件复测（生产已升级，回退需重新烧录且已超出本任务授权） |
| 3 | **时长口径与现有验收不一致** | 门禁要 **2 h 稳态 + 30 min 突发**；而 `G-4` 上一轮压力验收只跑了 ~21 min ⇒ 即使补齐埋点，**证据时长仍不满足 A** |
| 4 | **"单轮耗时"是否含采样开销** | A 未定义；本设计含 +1.2 µs（§3.5） |
| 5 | **`cloud_poll()` 单次耗时** | A 把它与 `loop()` P99 并列，但 B 的阻塞项只列 `loop()` P99；`Q-9.a` 也只写"`loop()` P99" |

### 4.3 证据要求（综合 A+B，未裁决部分标注）

| 要素 | 要求 | 状态 |
|---|---|---|
| 指标 | `loop()` 单轮 P50 **与** P99 | 明确 |
| 时长 | 2 h 稳态 + 30 min 突发 | 明确（与既有 21 min 验收不符） |
| 对照 | "修复前同一指标" | **不可获得**（§4.2-2） |
| 阈值 | `< 20 ms` 或 `劣化 < 20%` | **冲突**（§8 `Q-1`） |
| 采样方法 | 原文写 `micros()`；本规格主张 `esp_timer_get_time()`（`micros()` 会 71.6 min 回绕） | **需裁决**（§8 `Q-2`） |
| 输出面 | 未指定（`log` 侧信道 vs 命令查询） | **需裁决**（§8 `Q-5`） |

---

## 5. 测试矩阵

> 分两段：**S 段 = 能力/有效性冒烟**（本规格的验收）；**G 段 = 正式门禁证据**（2 h + 30 min，需单独授权执行）。
> 所有场景**只读命令**，不触发硬件动作；`minimum_free` **仅在同一 boot 内比较**。

| ID | 场景 | 时长/负载 | 采集指标 | 通过条件 |
|---|---|---|---|---|
| **S0** | 构建对照（不烧录也可先本地测） | 两个构建：`GF_LOOP_STATS_ENABLED=0/1` | `sizeof(结构)`、静态断言、RAM/Flash 增量、临界区拷贝耗时（本地/逻辑测） | 增量 RAM ≤ 640 B、Flash ≤ +2 KB；拷贝 ≤ 10 µs；`n` 单调、`Σhist+over = n` |
| **S1** | **空闲** | 10 min，零命令 | `P50/P99/min/max/over/sat/n`、`free`/`min_free`/`largest_free_block`、`queue`/`drop_*`、`boot_seq` | `n ≥ 10 000`；`over=0`；`sat=0`；`drop_*` 全 0；`boot_seq` 不变 |
| **S2** | **常规命令负载** | 10 min，1 条只读/30 s（≈20 条） | 同 S1 + 命令往返延迟、`ack_ok/retx` | 往返 100%；`P99` 相对 S1 **增幅 ≤ 20%**；零丢弃 |
| **S3** | **日志突发** | ~5 min：35 条只读突发（复用 `.pio/g4_burst.py`，与上轮同参） | 同 S2 + `drop_unacked`/`hovf`/`seg_deleted`/`cloud_retx` | 零丢弃；`ack_lost=0`；`P99` 相对 S1 **≤ +20%** 或长尾可在 `over/max` 中解释 |
| **S4** | **静置恢复** | 10 min，零命令 | 同 S1 | `P99` 回落到 S1 的 ±10% 内；`free` 与 S1 基线差 ≤ 512 B；`min_free` 不再下探 |
| **G1** | **门禁稳态**（需单独授权） | **连续 2 h**，常规负载 | 同 S2 + 稳定性项 | 按**裁决后**的阈值判定（`Q-1`） |
| **G2** | **门禁突发**（需单独授权） | **30 min** 突发 | 同 S3 | 同上 |
| **G3** | （可选）**多 boot 复现** | 2 个 boot 周期各跑 G1 的 1 h 子集 | 同 S2 | 仅作稳定性参考，**不得**跨 boot 比较 `minimum_free` |

**每场景必须同时采集的四类证据**（沿用 `G4-R5` 纪律）：
① 串口原文 ② 设备侧 `system.loop` / `system.stats` / `system.memory` 快照 ③ `log_history` 行数与 `seq` ④ EMQX 会话状态。**`mqtt_messages` 只作旁证**。

---

## 6. 三个结论的区分（观测能力 / 采集有效 / P99 达标）

| 层级 | 含义 | 判定方式（全部需满足） | 不满足时怎么表述 |
|---|---|---|---|
| **L1 观测能力已实现** | 代码能测、结构正确、可读 | ① `system.loop` 可返回 `available:true` ② `n` 随 uptime 增长 ③ `Σhist + over_cnt = n` 恒等 ④ `min ≤ P50 ≤ P99 ≤ max` ⑤ 单元/静态断言通过 ⑥ RAM/Flash 增量在预算内 | "**未实现**"——不得说"P99 未达标"（根本没测） |
| **L2 数据采集有效** | 测出来的数**可信** | ① `n / Δuptime ≈ 1/P50`（采样率自洽，误差 ≤5%）② 直方图与实际极值不矛盾（`max` 落在最后一个非零桶或 `over`）③ **独立交叉**：用串口 `t=` 时间戳推算的相邻命令间隔与 P99 量级一致 ④ **注入对照**：人为触发一次已知长阻塞（如一次段删除/一条大 `Serial.printf`）⇒ `max/over` 必须响应 ⑤ `sat=0` | "**采集无效**"——不得用其数值得出任何达标结论 |
| **L3 P99 达标** | 门禁该项**通过** | 按**裁决后**口径：<br>· 绝对口径：2 h 稳态 + 30 min 突发均 `P99 < 20 ms`<br>· 相对口径：相对**裁决指定的替代基线**劣化 < 20% | `PASS` / `FAIL` / **`NOT VERIFIED`**（口径未裁决时**只能是这个**） |

> ★ 现有状态下三项全为 **否**：L1 否（无埋点）⇒ L2 无对象 ⇒ L3 = `NOT VERIFIED`。
> ★ **L1/L2 达成后，`G-4` 仍不会自动变绿**：`DRAM` 项仍是独立阻塞（`G4-R4`），且本项还需 L3 + 时长证据。

---

## 7. 变更范围 / 构建归档 / 风险 / 回滚

### 7.1 代码变更范围（**估计**，实施前需再评估）

| 文件 | 改动 | 行数量级 |
|---|---|---|
| `src/app/loop_stats.h`（新） | 结构定义 + 4 个 API（`tick/get/reset?/available`） | +45 |
| `src/app/loop_stats.cpp`（新） | 采样、直方图、差分/P99 计算、临界区拷贝 | +110 |
| `src/main.cpp` | `loop()` 首行插入 `loop_stats_tick()`（`#if` 包裹）；`setup()` 末尾一次 `loop_stats_init()` | +6 |
| `src/services/system_command.{h,cpp}` | 新增 `syscmd_loop()`：输出 `available/n/p50/p99/min/max/over/sat/hist_sum` | +70 |
| `src/services/command_manager.cpp` | `system_router` 新增 `object == "loop"` 分支 + 前置声明 | +3 |
| `platformio.ini` | `-DGF_LOOP_STATS_ENABLED=1`（可关） | +2 |
| `test/loop_stats_contract/`（新，本地 host 编译） | 直方图/P99/差分/饱和 的**纯逻辑**单测（不依赖硬件） | +120 |
| **合计** | 7 文件 | **≈ +356 行**（比 `system.stats` 的 +158 大一倍，因含 P99 计算与测试） |

**明确不做**：分模块耗时埋点、`cloud_poll()` 单次耗时、写命令 `loop_reset`、周期上报、PSRAM 存储。

### 7.2 构建与归档（按 `S-2` 纪律，**强制**）

1. **独立构建目录**：`PLATFORMIO_BUILD_DIR=.pio/build/s3loop`（避免增量编译的 safe-delete 坑）。
2. 必须记录并入库：**源码 diff**（`git diff` 存档）、**`.bin`**、**SHA-256**、**构建日志**、**`platformio.ini` 快照**、**RAM/Flash 数字**（`pio run` 的 `RAM:/Flash:` 两行）。
3. 与当前基线对照：现在 `RAM 132 744 B (40.5%)` / `Flash 1 402 305 B (66.9%)` / `.bin` `1 402 672 B`（`sha256=a2c8a038…`）⇒ 新构建须给出三项增量。
4. 归档位置：`docs/evidence/S-3-<date>/`（含 `build.log`；`*.log` 受 `.gitignore` 限制，需在 README 注明"本地保留+可重建"）。

### 7.3 风险清单

| # | 风险 | 影响 | 缓解 |
|---|---|---|---|
| R1 | 采样开销扰动被测对象 | P99 系统性偏高 ~1.2 µs | 提供 `ENABLED=0` 对照构建；报告声明"含开销" |
| R2 | 读路径临界区过长 | 关中断 >10 µs，影响 Wi-Fi/TCP 时序 | 实测 ≤10 µs；超则分块/双缓冲 |
| R3 | 直方图桶饱和（`uint16`） | P99 失真 | `sat` 标志 + 必须显式降级标注；必要时桶类型升 `uint32`（+512 B） |
| R4 | `n` 溢出 | 长跑失真 | `uint32` @1 kHz ≈ 49 天；加饱和保护与告警 |
| R5 | 首样本/异常值 | P99 被单点污染 | 丢弃首个样本；`dt<=0` 或 `>60 s` 计入 `anomaly` 并排除 |
| R6 | 新增 `system.loop` 改变响应体 | 若误并入 `system.stats`，可能影响既有解析方 | **新增独立 object**，不动 `system.stats` |
| R7 | 静态 RAM +544 B | `minimum_free` 永久抬高 ~0.2%，**跨 boot 不可比** | 报告须声明"加入采样器后基线已变"；不得与 boot 22 的 `57 356` 直接比较 |
| R8 | `micros()` 若被误用 | 71.6 min 回绕 ⇒ 假长尾 | 代码评审硬性禁止；单测覆盖回绕边界 |
| R9 | 串口打印回归 | 一行 8.7 ms，直接破坏 P99 | 采样器**零输出**；评审检查 `loop_stats.cpp` 内无 `Serial`/`log_emit` |

### 7.4 回滚方案

| 层级 | 动作 | 验证 |
|---|---|---|
| 代码级 | `GF_LOOP_STATS_ENABLED=0` 重建 ⇒ 采样器编译剔除（零开销） | 对比 `.bin` 与上一版差值应仅为宏分支残留（预期 +0~200 B） |
| 固件级 | 回退到**已归档**的上一版 `.bin`（`sha256=a2c8a038…`）重烧 | 烧录后核对 `boot_seq` +1、`system.stats` 正常、零丢弃；**烧录本身含 1 次重启，需授权** |
| 数据级 | 无（采样器不落盘、不改配置、不改 D1） | — |
| 状态级 | 若 P99 未达标，`G-4` 保持 `BLOCKED`，**不产生任何门禁状态变更** | — |

---

## 8. 待人工裁决（**本规格不自行决定**）

| ID | 问题 | 选项 | 影响 |
|---|---|---|---|
| **`S3-Q1`** | `loop()` P99 用**绝对** `20 ms` 还是**相对** `劣化<20%`？两者冲突（§4.2-1） | ① 仅绝对 ② 仅相对 ③ 双条件同时满足 | 直接决定 L3 判定式 |
| **`S3-Q2`** | 若采用相对口径，**"修复前基线"永久不可得**，用什么替代？ | ① 放弃相对口径（转绝对）② 用"本版本空闲态 P99"作基线 ③ 用上一版固件（含/不含 `system.stats`）复测作基线 ④ 由负责人给定一个常数基线 | 决定是否需要**回溯复测**（额外烧录+授权） |
| **`S3-Q3`** | 采样方法：原文写 `micros()`，本规格主张 `esp_timer_get_time()`（`micros()` 71.6 min 回绕） | ① 批准改用 `esp_timer_get_time()` ② 坚持 `micros()` + 回绕补偿 | 影响实现与可信度 |
| **`S3-Q4`** | `cloud_poll()` 单次耗时**是否也是门禁必需项**（A 列、B 未列、`Q-9.a` 未列） | ① 本阶段只做 `loop()` ② 一并做 | 影响变更范围（+~40 行） |
| **`S3-Q5`** | P99 数据的**输出面**：新 `system.loop` 命令 / 并入 `system.stats` / 仅串口打印 | ① 新 object（本规格推荐）② 并入 `system.stats` | 影响向后兼容与"只读"边界 |
| **`S3-Q6`** | 是否需要 `system.loop_reset`（**写**操作，清计数）？ | ① 不需要（差分法）② 需要（须授权写命令） | 影响"只读"承诺 |
| **`S3-Q7`** | "单轮耗时"= 相邻进入间隔（本规格口径，因无节流器两侧等价）；确认？ | ① 确认 ② 另定义 | 影响 P99 语义 |
| **`S3-Q8`** | 门禁证据**时长**：`2 h 稳态 + 30 min 突发` 是否坚持（对照上轮只跑 21 min）？ | ① 坚持 ② 降级为新阈值（须新增裁决记录） | 决定 L3 所需工作量 |
| **`S3-Q9`** | 采样器自身是否计入被测循环（含 +1.2 µs）？以及**新基线声明**（RAM +544 B ⇒ `minimum_free` 不可与 boot 22 直比） | ① 计入并声明 ② 要求 `ENABLED=0` 纯净对照 | 影响报告措辞与基线口径 |
| **`S3-Q10`** | 是否需要**分模块**耗时埋点（喂食/饮水/OLED/传感器）以支撑 §6.2「竞争」行？ | ① 暂不做（本阶段）② 一并做（范围 ×3） | 影响工作量与实时性风险 |

> ★ 在 `S3-Q1` / `S3-Q2` 裁决前，**L3 只能是 `NOT VERIFIED`**；实施埋点也不会让该项变绿。

---

## 附录 A · 与既有文档的关系

| 文档 | 关系 |
|---|---|
| `docs/review/BT-1-Log-ACK-Read-Only-Investigation-and-Fix-Plan.md` §6.2 / §8.1 | **门禁定义权威出处**（本轮只引用，未修改） |
| `docs/review/BT-1-Human-Decision-Register.md` §5.4 / `Q-9` | `GATE-WP5` 归属与 `Q-9.a` 子项拆分 |
| `docs/architecture/G-4-Gate-Revision-Record-2026-10-20.md` | 仅涉及 DRAM 口径；**本文档不触碰** |
| `docs/architecture/G-4-Personal-Trial-Final-Verification-Report.md` §6.3 | 已列 `loop()` 埋点为下一阶段 `P0` |
| `docs/issues/未修复的问题.md` | 若后续实施，须在此登记进度与最终状态 |

## 附录 B · 本轮明确未做的事

- 未实现任何代码、未新增源文件、未改 `platformio.ini`（§7.1 只是规格）。
- 未构建、未烧录、未复位、未开串口、未发任何 MQTT 命令。
- 未修改 `G-4` / `WP-5` / `BT-1` / `STOP-R1` 的任何状态文件。
- 未修改 `docs/evidence/` 下任何既有证据。
- 未做**任何**跨 boot 的 `minimum_free` 比较。
