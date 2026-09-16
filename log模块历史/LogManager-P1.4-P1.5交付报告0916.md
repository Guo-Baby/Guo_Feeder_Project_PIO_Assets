# LogManager P1.4 / P1.5 交付报告（2026-09-16）

> 需求：`log模块历史/log模块开发需求规划0915.md` —— §15–§18 = P1.4，§19–§27 = P1.5
> 契约：`src/log_events.h`（P1.1 冻结）+ `log模块历史/LogManager-P1契约冻结0915.md`
> 进度台账：`log模块历史/P1.4-P1.5开发进度.md`

---

## 1. 交付摘要

| 阶段 | Commit | 内容 | 状态 |
|---|---|---|---|
| P1.4 | `42f564f` | Log Topic 上行 + CBOR 批次 + 云待发队列 | ✅ 已提交 |
| P1.5 | `4fdc1b2` | ACK 驱动推进 + 重试退避 + 离线补发 | ✅ 已提交 |
| 报告 | 本文件 | 交付与测试说明 | — |

```
Contract Test  ALL PASS（4 步，其中 2 步为 wasm32 真实执行）
Build          PASS 105.50s   RAM 39.7%   Flash 64.9%
Device / MQTT  ⛔ BLOCKED —— 开发板未连接（枚举不到 USB-Serial）
```

---

## 2. P1.4 —— Cloud Log Topic 上行（`42f564f`）

### 2.1 CloudManager：4 处纯新增，现有函数签名 0 改动

```cpp
enum CloudRoute { CLOUD_ROUTE_UP = 0, CLOUD_ROUTE_LOG = 1 };

bool cloud_send_log(const uint8_t *data, size_t length);        // ① 二进制 → log_topic
typedef void (*CloudLogAckCallback)(uint32_t, uint32_t, uint32_t);
void cloud_set_log_ack_callback(CloudLogAckCallback cb);        // ② 回调注入
// ③ config_get_mqtt_log_topic()  + mqtt.json "log_topic"
// ④ cloud_process_rx_message() 内 log_ack 旁路分支（照 change_msg_limit）
```

- 依赖方向 **LogManager → CloudManager**（单向），CloudManager 不反向依赖。
- `cloud_send_log()` 离线返回 false：**不为 Log 建任何 TX 队列**，退避重试归 LogManager。
- `log_ack` 落在 CommandManager **之前**，不回 ACK（at-least-once 由云端幂等消化）。
- 顺手删除 `cloud_send_up_cbor` / `cloud_send_set_cbor` / `cloud_send_up_binary`
  —— 三个**有声明无实现**的死接口（0914 审计标记的陷阱）。

### 2.2 LogManager：PSRAM 云待发队列

- 128 槽 × 128 B = **16 KB，PSRAM 优先 / DRAM 回退**。
- 入队带去重守卫；队列满 FIFO 淘汰最旧并计 `drop_overflow`。
- 组批 ≤16 条且**必须同一 boot_seq**（批次头只有 1 个 boot_seq 字段）。
- 批次头携带 4 个**增量**丢弃计数（`drop_ring` / `drop_overflow` /
  `drop_unacked` / `self_degraded`）作侧信道 —— 因为"丢弃"本身无法写进已满的环。
- 节流 ≤1 批 / 500 ms。

### 2.3 `src/log_cbor.h`：freestanding CBOR 编码器

只依赖 `<stdint.h>` / `<stddef.h>` / `<string.h>` + `log_events.h`，**不依赖 Arduino**。
定序数组 12 项（`LOG_BKEY_*` 0..11），`records` 为 `bstr(128)` **原样保留冻结布局**。
容量 / 上界 / 空指针全部返回 0，绝不越界。

**为什么单独成头文件**：可在主机上编到 wasm32 并用 node **真实执行**，
对 wire 格式做字节级验证 —— 这是本项目在**没有开发板**时唯一能"真跑"的路子。

---

## 3. P1.5 —— ACK / Retry / Offline（`4fdc1b2`）

### 3.1 `src/log_ack.h`：判定与执行分离（本阶段最重要的结构决定）

| 函数 | 作用 |
|---|---|
| `log_ack_range_valid` | `from <= to` 且均非 0 |
| `log_ack_classify` | → IGNORE / DUPLICATE / ACCEPT / PARTIAL |
| `log_ack_covered_count` | 只取被覆盖的**前缀** |
| `log_ack_segment_deletable` | 末条 `seq <= acked` 才可删段（"部分覆盖不得删段"的唯一判定点） |
| `log_ack_backoff_ms` / `_ex` | 2/4/8/16/32 s，上限 60 s |
| `log_ack_should_give_up` | `retry > LOG_ACK_MAX_RETRY(5)` |
| `log_ack_should_replay` | `GIVE_UP_NOT_ADVANCE` 的水位判定 |

**判定顺序是语义的一部分**（已在探针中固化优先级）：

```
① 区间非法 → IGNORE   ② 无在途 → IGNORE   ③ boot_seq 不匹配 → IGNORE
④ ack_to <= acked_seq → DUPLICATE（回退/重复，无变化）
⑤ ack_to < tx_from → IGNORE（确认的是更早批次）
⑥ 完整覆盖 → ACCEPT   ⑦ 其余 → PARTIAL
```

**为什么必须分离**：本设备开发期长期没有可用的 MQTT 对端。ACK 的
ACCEPT / PARTIAL / 退避 / 放弃分支若只写在 `log_manager.cpp` 里，将**完全无法验证**。
分离后判定部分可在主机 wasm32 上穷举（50 项断言，见 §4）。

### 3.2 `cloud_poll()` 重写为七段式

```
(1) ACK 落账       回调只置标志；判定 + Flash I/O 全在 loop 上下文
(2) 在途超时       超过 5 次 → 放弃；否则退避后重发同一批
(3) 退避等待
(4) 离线跳过       不消费任何记录，计 cloud_offline_skip
(5) 节流           最快 1 批 / 500 ms
(6) Flash 补发     RAM 云队列已空时，把离线期间落盘的未确认记录重新入队
(7) 取批发送
```

**推进依据从"发布成功"改为"收到 ACK"** —— P1.4 的乐观推进已移除。

### 3.3 关键语义实现

| 语义 | 实现 |
|---|---|
| 部分覆盖 | 只推进被覆盖前缀，尾部留在队列下轮重组（不丢、不重复记账） |
| 段回收 | 整段被覆盖且非追加目标、且补发游标已越过 → 删段 |
| **GIVE_UP_NOT_ADVANCE** | 只移出 RAM 云队列并计 `drop_unacked`；**ack 水位与 Flash 段都不推进**（记录留待下次开机重放） |
| 重启后重发 | `acked_seq` 归 0 ⇒ 曾发过的记录会重发，属**正常行为**（§25），云端按 `(device_id, boot_seq, seq)` 幂等去重 |
| 去重守卫 | 由"seq 单调"改为 **`(boot_seq, seq)` 相等** —— 单调判定会让 Flash 补发在队列排空后**永远被拒绝**（补发的都是旧 seq） |

### 3.4 测试钩子（仅上板自测，正式固件不调用）

| 命令 | 作用 |
|---|---|
| `logt ack <boot> <from> <to>` | 直接注入 ACK（走同一状态机） |
| `logt ackauto <0..3> [k]` | **按当前在途批次自动构造 ACK** —— seq 由 Boot 区间预留分配，脚本无法预知 |
| `logt cfail <n>` | 令接下来 n 次 `cloud_send_log()` 失败（模拟离线） |
| `logt sonline <0\|1>` | 强制在线 + 跳过真实 publish —— 没有 MQTT 对端时也能产生"在途批次"，否则 ACCEPT/PARTIAL 分支永远走不到 |
| `logt atimeout / abackoff <ms>` | 缩短超时与退避（默认值不变）。否则"重试耗尽"需 **≈152 s** 纯等待，超出单条命令 200 s 预算，该冻结语义**无法验证** |

---

## 4. 测试结果（逐项，真实执行）

| # | 项 | 结果 | 证据 |
|---|---|---|---|
| 1 | Contract `[1/4]` 正向探针 | ✅ PASS | 编译通过（契约值全对） |
| 2 | Contract `[2/4]` 负向探针 | ✅ PASS | 按预期编译失败 ⇒ 断言**可证伪** |
| 3 | Contract `[3/4]` CBOR 字节级 | ✅ PASS | 21 B 头逐字节匹配；`bstr` 前缀 `0x58 0x80`；越界哨兵；上界拒绝（**16 项**） |
| 4 | Contract `[4/4]` **ACK 逻辑** | ✅ PASS | **50 项**（区间 6 / 分类 12 / 覆盖前缀 6 / 段可删性 7 / 退避 14 / 放弃门槛 4 / 可重放水位 7） |
| 5 | Build | ✅ PASS | 105.50s，RAM 39.7%（130048 B），Flash 64.9%（1361161 B） |
| 6 | EMQX `guo_feeder/log` ACL | ✅ PASS | `test/mqtt_log_probe.py probe` → CONNACK=0、SUBACK granted=1、publish rc=0 |
| 7 | **Device Flash（P1.4/P1.5 固件）** | ⛔ **NOT TESTABLE** | 开发板未连接 |
| 8 | **设备侧 ACK 全链路**（用例 A） | ⛔ **NOT TESTABLE** | 同上 |
| 9 | **超时重试 / 重试耗尽**（用例 B） | ⛔ **NOT TESTABLE** | 同上 |
| 10 | **真实 EMQX 端到端**（设备 → 云端 → `log_ack`） | ⛔ **NOT TESTABLE** | 同上 |
| 11 | 掉电事务恢复 / 16×16 满载 | ⛔ 未覆盖 | 属 P1.3 遗留，本次未涉及 |

**NOT TESTABLE 的原因（唯一）**：`list_ports` 只能枚举到 3 个蓝牙虚拟串口，
没有 USB-Serial 设备 ⇒ **不是代码问题**，是外部条件缺失。

**恢复后要执行的命令（已备好，可直接照跑）**：

```bash
# 1) 烧录（只写 app 分区，保留 LittleFS）
esptool.py --chip esp32s3 --port COM8 --baud 921600 \
           write_flash -z 0x10000 <BD>/esp32-s3-devkitc-1/firmware.bin

# 2) 用例 A：ACK 语义（IGNORE / PARTIAL / DUPLICATE / ACCEPT / 段回收 / 离线）
python test/serial_batch.py COM8 .pio/logp15a.log test/log_p15_ack_tests.txt 5.0 0.35

# 3) 用例 B：超时重试 / 重试耗尽
python test/serial_batch.py COM8 .pio/logp15b.log test/log_p15_giveup_tests.txt 3.0 0.35

# 4) 真实 MQTT 端到端
python test/mqtt_log_probe.py listen 60                    # 终端 1：订阅日志
python test/mqtt_log_probe.py ack <boot> <from> <to>       # 终端 2：发 log_ack
```

> 用例 A/B 是**校准草稿**：断言只取单调计数器与结构性字段
> （loop 在两条命令之间持续收发，瞬时标志不稳定）。首次上板请对照日志校准数值 ——
> 本项目历史上多次因**断言值写错**而误判 MISS。

---

## 5. 未执行 / 阻塞项（诚实清单）

| 项 | 状态 | 缺什么 |
|---|---|---|
| 设备 Flash / 串口测试 | ⛔ 未执行 | 开发板（USB-Serial 未枚举到） |
| 真实 EMQX 上行 + `log_ack` 往返 | ⛔ 未执行 | 同上 |
| 用例 A/B 断言值校准 | ⛔ 未执行 | 同上 |
| `FORCE_ADVANCE` | 未实现（按冻结要求） | 刻意；v1 默认 `GIVE_UP_NOT_ADVANCE` |
| 掉电事务恢复 / 16×16 满载 | 未覆盖 | 属 P1.3 遗留范围 |

**没有把任何未执行的测试写成 PASS。**

---

## 6. 与 P1.4 的差异（需要知悉）

| 项 | P1.4 | P1.5 |
|---|---|---|
| 队列推进依据 | 发布成功即推进（乐观） | **收到 ACK 才推进** |
| 去重守卫 | `seq` 单调 | **`(boot_seq, seq)` 相等** |
| 段回收 | 无 | 整段被覆盖才删；跳过追加目标与补发游标未越过的段 |
| 离线 | 发送返回 false 即停 | 不消费记录 + 计数；恢复后从 Flash 补发 |

---

## 7. 风险与遗留

1. **段回收对"当前追加目标"保守跳过**：首段需等追加目标移走才回收
   （宁可晚回收，不可删掉可能还要补发的数据）。属刻意设计，非缺陷。
2. **`drop_overflow` 与 `drop_unacked` 是两个计数**，批次头分别携带，勿混用。
3. **`sonline` / `atimeout` / `abackoff` 是测试旁路**：`log_init()` 与
   `log_cloud_test_reset()` 都会复位；正式固件不得调用。
4. **未做 MQTT 端到端 ⇒ `log_ack` 的字段解析只在 PC 端验证过**
   （`mqtt_log_probe.py ack` 用冻结键 `c/i/p/b/f/t` 构造），设备侧解析逻辑
   （`cloud_process_rx_message` 的 log_ack 分支）**尚未在真实报文下跑过**。
5. 上一个阶段的 P0 前置修复（`json_storage`/`file_storage` 回调未注册、
   `event_names[]` 错位、MQTT 明文密码、BLE hex 隔离等）**仍未做**。

---

## 8. 下一步建议

1. **接上开发板** → 跑用例 A/B + 真实 EMQX 端到端（命令见 §4）。
2. 若 `log_ack` 报文解析有问题，优先检查 `cloud_manager.cpp` 的 log_ack 分支
   与 `LOG_ACK_P_*` 键名是否与云端一致。
3. 然后才是 P0 前置修复（与 Log 解耦，可独立提交）。
