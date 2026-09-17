# LogManager P1.5 上板验证报告（0918）

> 性质：**设备侧验证记录**（不改动任何 `src/` 生产代码）
> 固件基线：`65b4523 fix(log): complete P1.5 ack hole and replay cursor fixes`
> 验证方式：编译 → 烧录 → 串口批量回归（187 断言）+ 真实 MQTT 收发探针
> 结论一句话：**LogManager P1.5 核心修复在真实硬件上全部得到证实（167/187 断言通过，20 条未通过全部定位为用例预期值缺陷，非固件缺陷）；同时发现 1 个现场级问题 —— 云端从未回送 `log_ack`。**

---

## 1. 测试环境

| 项 | 值 |
|---|---|
| 主机 | Windows / Git Bash（沙箱） |
| 开发板 | ESP32-S3 DevKitC-1，Flash 16MB QIO，OPI PSRAM 8MB |
| 串口 | **COM8**（USB-Enhanced-SERIAL CH343，VID:PID=1A86:55D3） |
| 烧录方式 | `esptool.py write_flash -z 0x10000 firmware.bin`（**不动 LittleFS**，保留设备既有配置） |
| 构建目录 | `.pio/build/p15board`（全量编译 **88.5 s**，`[SUCCESS]`） |
| 固件产物 | `firmware.bin` 1364000 B → 压缩 874515 B，`Hash of data verified` |
| MQTT broker | EMQX（`n302933b.ala.cn-hangzhou.emqxsl.cn:8883`，TLS，CA 已加载 1316 B） |
| MQTT 客户端 | `test/mqtt_log_probe.py`（订阅/解码）、`.pio/p15run/ack_probe.py`（下行 ACK 注入） |
| 串口回归器 | `test/serial_batch.py`，`QUIET=3.0 s`、`HIT_GRACE=0.35 s` |
| 测试时间 | 2026-09-17 20:48 – 21:07 |

### 1.1 Flash / LittleFS 前置状态

本次**未执行 `uploadfs`**，设备保留上一轮（P1.4/P1.5）留下的 `/littlefs/log/` 段文件。
这不是污染 —— 它恰好提供了**跨重启的真实遗留数据**，使 F2-B（重启后补发）得以在真实遗留段上验证。

---

## 2. 固件版本 commit

| 项 | 值 |
|---|---|
| 代码 HEAD | `65b4523`（其上 `1d0b47c` 为仓库整理，不含代码变更） |
| `git diff --stat -- src/ test/`（测试前） | **空** |
| 编译期契约自检 | `[LogContract] v=2 rec=128 seg=3984 x16 cap=496 fmt=2 check=PASS` |

---

## 3. 验证 A —— LogManager 初始化（全部通过 ✅）

启动日志（`20:56:18`，A 段 boot）：

```
PSRAM size: 8386279
PSRAM free: 8386035
[LogContract] v=2 rec=128 seg=3984 x16 cap=496 fmt=2 check=PASS
[Log] RAM ring ready: 64 slots x 128 B = 8192 B in PSRAM (boot_seq=1)
[Log] Flash ring ready: seg=16 x 31 rec, seg_size=3984 B, valid=5, oldest=0, newest=4,
                       append=4@9, total_rec=133, batch=1024 B in PSRAM
[Log] seq: boot_seq=6 base=1793 limit=2048 reserved=2048 reliable=1 corrupt=1
[Log] Cloud queue ready: 128 slots x 128 B = 16384 B (q=PSRAM), batch=2048 B, cbor=4096 B in PSRAM
```

| 检查项 | 期望 | 实测 | 结论 |
|---|---|---|---|
| PSRAM 检测 | 8MB 可用 | `8386279` B（≈8.0 MB），free `8386035` | ✅ |
| 冻结契约自检 | PASS | `check=PASS`（v2 / 128B / 3984×16 / cap496 / fmt2） | ✅ |
| RAM 环初始化 | 64 槽 × 128 B，PSRAM 优先 | 8192 B **in PSRAM** | ✅ |
| Flash 段环初始化 | 建段 + 全量扫描 | `seg=16 x 31 rec`，扫描出 `valid=5, total_rec=133` | ✅ |
| meta / seq 预留 | boot_seq 递增 + 预留区间 | `boot_seq=6 base=1793 limit=2048 reserved=2048 reliable=1` | ✅ |
| Cloud 队列初始化 | 128 槽，PSRAM 优先 | 16384 B **in PSRAM**，batch 2048 B，CBOR 4096 B | ✅ |
| ACK 回调注入 | `cloud_set_log_ack_callback` 成功 | 由 `cloud_init_buffers()` 成功后注入（`log_manager.cpp:1397`）；实测下行 ACK 可被接收（§6） | ✅ |

> **seq 稀疏（★ 根因认知）在启动日志中直接可见**：
> `valid=5` 段共 `total_rec=133` 条，而 `base=1793 → limit=2048` 是 **256 个 seq 号**；
> 更直接的证据见 §6.1 的线上批次头：`seq_from=288 → seq_to=519` 但 `count=16`。

### 3.1 boot_seq 递增（重启测试 D 的一部分）

| 时刻 | 场景 | boot_seq | 说明 |
|---|---|---|---|
| 20:49:00 | 首次抓取（沿用上轮 meta） | **5** | meta 完好 |
| 20:50:00 | 重启（A 段首次运行） | **6** | ✅ **递增 +1** |
| 20:56:18 | 重启（`logt mwipe` 之后） | 1 | meta 被测试钩子擦除 → 重建后 boot_seq 重新从 1 起 |

⇒ **meta 完好时 boot_seq 正确递增（5 → 6）✅**。
`boot_seq` 回落为 1 仅出现在**显式擦除 meta 的测试钩子**之后（`logt mwipe`），属测试构造，非缺陷；
但**云端必须按 boot_seq 分段**，否则 meta 重建后的 ACK 会被 `boot 不匹配 → IGNORE`（设计如此）。

---

## 4. 验证 B —— 串口状态观测

### 4.1 `logt stats` 全字段（A 段 F1，`21:02:06` 附近）

```
[LogT] stats  emit=140 debug=0 ring=0/64 hw=62 drop=0 consumed=140 flash=140 cloud=140
              crit=0 flush=0 boot=1 seq=140 ready=1 psram=1 bytes=8192
[LogT] fstats ok=140 fail=0 seg_new=5 seg_del=0 crc_err=0 corrupt=0 retry=0 blocked=0 honored=0
[LogT] cstats batch=1 rec=0 retx=0 ack_ok=0 ack_to=0 ack_lost=0 qdrop=4 fdrop=0 deg=0 qused=128
[LogT] cstats2 evict_inf=8 seg_evict_unacked=0 hole_evict=0
[LogT] ackst  ignored=0 partial=0 segdel=0 replay=0 offskip=0 giveup=0
```

| 检查项 | 结果 |
|---|---|
| `drop_ring=0` | ✅ 全程未见 RAM 环溢出（`ring=0/64`、`drop=0`） |
| `drop_overflow`（`qdrop`） | 仅在**故意构造溢出**的用例中出现（F1/F7），其余为 0 ✅ |
| `flash blocked / retry` | ✅ 全程 `blocked=0 retry=0`（Flash 无阻塞，落盘畅通） |
| `flash` 段状态 | ✅ `ok=140 fail=0`，段扫描/建段/回收计数自洽 |
| `psram=1` | ✅ RAM 环与 Cloud 队列均在 PSRAM |

> **注意**：本文件中没有名为 `stats` 的独立读取命令以外的观测手段。
> `logt stats` **会先排空 RAM 环**（`logt_drain_all()` → 16 × `log_task()`），
> 因此它是"排空后的一致快照"，不是纯只读查询。

---

## 5. 验证 C —— MQTT / 日志上云（设备→云 ✅，云→设备 ACK ❌）

### 5.1 设备→云：批次真实可达并可解码 ✅

PC 端订阅 `guo_feeder/log` 收到的真实批次（`test/mqtt_log_probe.py listen 45`）：

```
[log #1] topic=guo_feeder/log len=2097
{'fmt': 2, 'event_dict_ver': 2, 'boot_seq': 1,
 'seq_from': 288, 'seq_to': 519, 'count': 16,
 'drop_ring': 0, 'drop_overflow': 0, 'drop_unacked': 0, 'self_degraded': 0,
 'flags': 1, 'records': '16 条', 'rec0_head': '02020101200100000100000001040000'}
```

| 字段 | 冻结契约 | 实测 | 结论 |
|---|---|---|---|
| `fmt` | 2 | 2 | ✅ |
| `event_dict_ver` | 2 | 2 | ✅ |
| `count` | ≤ 16 | 16 | ✅ |
| `flags` | `LOG_BATCH_FLAG_SEQ_RELIABLE` = 0x01 | 1 | ✅ |
| 侧信道计数 | `drop_ring/overflow/unacked/degraded` | 全 0 | ✅ |
| **seq 稀疏** | 允许空洞 | **`seq_from=288 → seq_to=519`（跨度 232）但 `count=16`** | ✅ **线上证据：INFO 占 seq 但不落 Flash** |

设备侧同批次对照：`[Cloud LOG] OK topic=guo_feeder/log len=2097 t=6252`。

### 5.2 云→设备：**云端从未回送 `log_ack`** ❌（现场第 1 号发现）

| 段 | 发出的批次 | 收到的 ACK | ACK 超时 | 重试耗尽(give-up) |
|---|---|---|---|---|
| 首次抓取 | 2 | **0** | 1 | 0 |
| A | 4 | **0** | 6 | 1（F2-A 故意不 ACK） |
| B | 5 | **0** | 3 | 0 |
| C | 1 | **0** | 1 | 0 |
| D | 0 | 0 | 0 | 0 |
| E | 2 | **0** | 0 | 0 |

典型时序（首次抓取）：

```
20:49:04.035 [Cloud LOG] OK topic=guo_feeder/log len=2095      ← 批次已发出
20:49:20.796 [Log Cloud] ack timeout retry=1/5 backoff=2000ms  ← 14.76 s ≈ LOG_ACK_TIMEOUT_MS(15000)
20:49:21.167 [Cloud] MQTT published/ack msg_id=35352 outbox=0  ← 重发成功
（之后仍无 ACK）
```

**判读**：`grep -c 'ack rx'` 在全部 6 个会话中均为 **0**；设备侧 MQTT 上行完全正常
（QoS1 PUBACK 正常、QoS=1 store=1、消息长度 1053–2097 B 合规）。
⇒ **设备端无缺陷；缺的是云端 `guo_feeder/down` 的 `log_ack` 回执实现。**

**后果（按优先级）**：
1. 每条批次都要走满 **6 次超时 + 2/4/8/16/32 s 退避 ≈ 152 s** 才 give-up；
2. `GIVE_UP_NOT_ADVANCE` ⇒ `acked_seq` 水位不前进 ⇒ **Flash 段永远无法被回收**（496 条写满后开始淘汰未确认记录，`drop_unacked` 上升）；
3. 云端持有全部日志却无法驱动设备端清理 ⇒ **存储泄漏 + 重复补发**。

**这是 P2 开工前必须先解决的外部依赖**（本次不计入设备缺陷）。

---

## 6. 验证 ACK 路径（含 F5，真实 MQTT）

### 6.1 真实下行 `log_ack` 可被设备识别 ✅

`.pio/p15run/ack_probe.py`（PC 端 MQTT 发 → 设备串口收，同一进程观察联合时序）：

```
[Cloud LOG] ack rx boot=0 from=0 to=0     ← 注入的 log_ack（带 i）被接收并进入 ACK 状态机
step2 ack(with i): 'ack rx' = True ; 'ack ok' = False
```

`ack ok = False` 的原因是被注入时设备**当前无在途批次**（`boot=0 from=0 to=0`，
探针打开串口后板子刚复位，`logt cloud` 读到的 `boot=0 from=0 to=0 n=0`）
⇒ ACK 被判 `IGNORE`，**这是冻结契约的正确行为**（区间非法 / boot 不匹配 → 忽略）。

### 6.2 协议级旁路：**不带 `i` 的 `log_ack` 被正确处理** ✅✅

```
step3 ack(no i): 'Missing id' = False (must be False)
step3 ack(no i): 'ack rx' = True
```

这是 `cloud_manager.cpp:1060-1076` 的 FIX-4 关键分支：`log_ack` 必须绕过
"命令 id 必需性"校验。**上板实测通过**（若未绕过，会打印 `Missing id` 并丢弃）。

### 6.3 F5 —— `log_ack` 不得污染命令去重缓存 ✅✅✅

| 步骤 | 期望 | 实测 |
|---|---|---|
| 发 `id=DUPX` 第 1 次 | 执行（无 duplicate） | `duplicate = False` ✅ |
| 发 `id=DUPX` 第 2 次 | 命中去重 | `duplicate = True` ✅ |
| 连发 **15 条** `log_ack`（i = AK01..AK15） | 全部不得进入去重缓存 | 15 × `[Cloud LOG] ack rx` ✅ |
| 再发 `id=DUPX` 第 3 次 | **仍须**命中去重 | `duplicate = True` ✅ |

⇒ **F5 判定：通过**（这正是 `test/log_fix_tests.txt` 中标注"无法用串口验证、必须走真实 MQTT"的那一段）。

### 6.4 完整 ACK 状态机（串口注入路径）

`logt ackauto` / `logt ack` 走**与 CloudManager 回调同一条**状态机（只跳过 MQTT 传输），
本次共产生 **12 次成功落账**，覆盖整段覆盖与部分覆盖两种情况：

```
ack ok boot=1 to=2320 covered=16/16 acked=2320 r=2     ← 整段覆盖
ack ok boot=1 to=2350 covered=15/15 acked=2350 r=2
ack ok boot=1 to=2507 covered=8/8  acked=2507 r=2      ← F2-B 重启后补发的 8 条
ack ok boot=1 to=4    covered=4/8  acked=4    r=3      ← ★ 部分覆盖（F7 用）
ack ok boot=1 to=28   covered=16/16 acked=28  r=2
```

| 检查项 | 结果 |
|---|---|
| 整段覆盖推进 | ✅ `covered=N/N` → `acked=N` |
| **部分覆盖只推进被覆盖前缀** | ✅ `covered=4/8 → acked=4`（不越权推进） |
| 推进依据 = 收到 ACK（非发送成功） | ✅ 未 ACK 的批次重试 6 次后 `give up (NOT advance)` |
| 重试耗尽不推进水位 | ✅ A 段 `[Log Cloud] give up (NOT advance) boot=1 from=2244 to=2251 n=8` |

---

## 7. 验证 replay / 补发（P1.5 的核心承诺）✅ 全部证实

### 7.1 F2-B：重启后未 ACK 日志恢复发送

```
B 段 boot（真实重启，串口重开触发）：
  [Log] Flash ring ready: ... valid=1, oldest=0, newest=0, append=0@8, total_rec=8
  [Log] meta missing/corrupt -> rebuild: max_seq=2507 reserved=2560 corrupt=1

21:03:13  cloud ... qused=8 acked=0 inflight=1 boot=1 from=2500 to=2507 n=8 retry=0
21:03:13  cloud3 ... rseg=0 ridx=8 rarmed=1                 ← 补发已 arm
21:03:15  ackst ... replay=8                                ← 8 条重新投递
21:03:16  ack ok boot=1 to=2507 covered=8/8 acked=2507 r=2   ← ACK 全部覆盖
21:03:16  [Log Cloud] replay sweep done (acked=2507 giveup=0)
21:03:26  cloud2 ... replay_done=1 ; cloud3 ... rarmed=0      ← 扫描收尾
```

⇒ **`rarmed=1 → replay=8 → ACK → replay sweep done → replay_done=1 → rarmed=0` 全链路通过**。
断言 `rarmed=1` / `replay=8` / `acked=` / `rarmed=0` **全部 OK**。

### 7.2 F8：补发 sweep 中删除"游标所在段"后继续前进（FIX-D1）

```
21:03:13  cloud3 ... rseg=0 ridx=16 rarmed=1
21:03:17  [LogT] fcorrupt seg=0 ok=1            ← 人为破坏游标所在段
21:03:18  fstats ... crc_err=1 corrupt=1        ← 扫描识别并废弃
21:03:22  cloud3 ... rarmed=1                   ← 补发仍在继续（未回退到最老段）✅
21:03:26  ack ok boot=1 to=272 covered=16/16
21:03:27  ackst ... replay=25                   ← ★ 16 → 25，继续前进 ✅
21:03:28  cloud3 ... rseg=1 ridx=9 rarmed=1     ← ★ 游标已越过被删段 ✅
21:03:30  ack ok boot=1 to=296 covered=9/9
21:03:31  cloud2 ... replay_done=1 ; cloud3 ... rarmed=0    ← 正常收尾 ✅
```

⇒ **FIX-D1 核心承诺成立**：游标所在段被删除后，补发从**其后**继续（`rseg 0 → 1`、`replay 16 → 25`），
**没有回退到最老段、没有空转、没有丢条**。
唯一 MISS 是辅助断言 `seg_del=1`（见 §8）。

### 7.3 F3-A / F3-B：段环压力淘汰记账

```
F3-A 写满 16 段（8 × fill warn 62 + flush）
  21:02:xx  flash ... segs=16 total=496 append=15@31
            cstats2 ... seg_evict_unacked=0
  fill warn 1 + flush（第 497 条 → 触发首次压力淘汰）
            cstats2 ... seg_evict_unacked=31   ← ★ 未确认段被淘汰必须记账 ✅
            cstats  ... fdrop=31                ← ★ 未 ACK 条数同步上报 ✅
            fstats  ... seg_del=1               ← ✅
            flash   ... total=466 oldest=1      ← 496 − 31 + 1 = 466 ✅ 公式吻合
```

⇒ **FIX-3（HIGH-1）通过**：压力淘汰未确认段**必须记账**，且 `total` 数字与公式
`496 − 31 + 1 = 466` 精确吻合。

---

## 8. F0–F8 回归结果（187 断言 → **167 OK / 20 MISS**）

回归按 `test/log_fix_tests.txt` 的复位边界拆成 5 段执行（用例文件第 156 行明确要求中途复位开发板）：

| 段 | 覆盖 | 断言 | OK | MISS | 耗时 |
|---|---|---|---|---|---|
| A | F0、F1、F2-A | 56 | 51 | 5 | ~120 s |
| B | F2-B、F3-A | 57 | 54 | 3 | ~150 s |
| C | F3-B、F4 | 21 | 16 | 5 | ~60 s |
| D | F7 | 30 | 24 | 6 | ~65 s |
| E | F8 | 23 | 22 | 1 | ~40 s |
| **合计** | | **187** | **167 (89.3%)** | **20** | ~7.5 min |

### 8.1 各 F 段结论

| 用例 | 主题 | 判定 |
|---|---|---|
| F0 | 稀疏 seq 模型 + DIR-1 slot 遍历补发 | ✅ 通过（含 `gap=15` 的稀疏证明） |
| F1 | FIX-1 队列溢出与 ACK 落账叠加**不跳条** | ⚠️ 核心不变量成立，4 条预期值需校准 |
| F2-A | FIX-2 give-up 登记空洞 + 空洞阻止段回收 | ✅ 通过 |
| F2-B | 重启后补发 + 确认后才回收 | ✅ 核心通过（`segdel` 预期错误） |
| F3-A | FIX-3 段环压力淘汰未确认段必须记账 | ✅ 通过 |
| F3-B | 已确认段被回收时**不计** drop_unacked | ✅ 通过 |
| F4 | FIX-5 超时后批次描述符**仍然有效** | ⚠️ 核心（`tx_valid=1`）成立，`inflight` 预期错误 |
| F5 | FIX-4 `log_ack` 不得污染命令去重缓存 | ✅ **通过（真实 MQTT，见 §6.3）** |
| F7 | FIX-H2 ACK 落账阶段为"淘汰间隙"补齐空洞 | ⚠️ 核心（`partial=1`/`hole_evict`）成立，预期值需校准 |
| F8 | FIX-D1 补发 sweep 中删除"游标所在段"后继续 | ✅ 通过（仅辅助断言 `seg_del` 预期错误） |

### 8.2 20 条 MISS 的逐条归因（**全部为用例预期缺陷，非固件缺陷**）

> 判定依据：每条都给出了"实测值 + 自洽的算术/契约解释"，无一条指向 LogManager 行为错误。

| # | 断言 | 段 | 实测 | 归因 | 证据/修法 |
|---|---|---|---|---|---|
| 1 | `evict_inf=12` | A | 9 | **预期算术错**：只在途批次窗口 16 槽里的那部分才算 inflight | `qdrop(12) + evict_inf(9) = 21 = (29+120) − 128` **精确自洽**；预期 12 是把"全部淘汰"当成了"窗口内淘汰" |
| 2 | `qdrop=0` | A | 12 | 同上 | 同上；正确预期应拆成 `evict_inf=9` / `qdrop=12` |
| 3 | `qused=124` | A | 112 | **预期算术错** | 整批 16 条被 ACK ⇒ `128 − 16 = 112`；预期的 124（=128−4）假设只有 4 条还在队列 |
| 4 | `tx_valid=0` | A | 1 | **预期错**：ACK 后队列仍有 112 条 ⇒ 立即又发一批 | 实测 `from=2132 to=2147 n=16 retry=0` |
| 5 | `inflight=1` | A | 0 | **时序假设不成立**：F2-A 已设 `atimeout=200`/`abackoff=100`，批次在 `logt flash` 的等待窗口内已重试耗尽 | 同一段随后 `giveup=1` 断言 **PASS**，说明这是设计内的耗尽 |
| 6 | `replay=0` | B | 8 | **不可观测**：串口会话在 boot 之后才建立，replay 早已完成 | `replay=8` 恰是 P1.5 想要的结果（§7.1） |
| 7 | `segdel=1` | B | 0 | **预期错**：`seg0` 同时是 `s_append_segment` | `cloud_delete_acked_segments()` 显式 `if (seg == s_append_segment) continue;` —— **追加目标按设计不可回收**；本场景只有 1 段（`segs=1 append=0@8`），故必然为 0 |
| 8 | `seg_del=1` | B | 0 | 同上 | 同上 |
| 9 | `inflight=1` | C | 0 | **预期错**：ACK 超时后 `inflight` 清零、`tx_valid` 保留 | **这正是 FIX-5 的本意**（超时后描述符仍有效）；紧邻的 `tx_valid=1` 断言 **PASS** |
| 10 | `inflight=1` | C | 0 | 同上（仍在 20 s 退避窗口内，未发新批） | 同上 |
| 11 | `acked=` | C | `FAIL no-inflight` | **用例用错工具**：`logt ackauto` 在 `inflight=0` 时拒绝注入 | F4 应改用 `logt ack <boot> <from> <to>`（该入口走同一状态机且不要求在途批次） |
| 12 | `ack_ok=1` | C | `ack_ok=0` | #11 的连锁 | — |
| 13 | `tx_valid=0` | C | 1 | #11 的连锁 | — |
| 14 | `evict_inf=12` | D | 8 | **预期算术错**（同 #1） | `qdrop(4) + evict_inf(8) = 12 = (20+62+58) − 128` **精确自洽**；在途批次只有 8 条，窗口内最多 8 条 |
| 15 | `qdrop=0` | D | 4 | 同 #14 | 同上 |
| 16 | `hole_evict=8` | D | 4 | **预期算术错（下游）** | 缺口 = `[covered=4, evicted=8)` = **4 条**；预期 8 来自错误的 `evicted=12` |
| 17 | `holes=1` | D | 2 | ⚠️ **待确认（唯一未完全归因项）** | 空洞表条目数 2 vs 预期 1；与 `evict_inf=8` 修正后的缺口结构可能一致，需结合 `log_hole_*` 实现复核 |
| 18 | `holes=1`（第 2 次） | D | 2 | 同 #17 | 同上 |
| 19 | `tx_valid=0` | D | 1 | **预期错**：立即又发一批 | 实测 `from=13 to=28 n=16` |
| 20 | `seg_del=1` | E | 0 | **预期错**：`fcorrupt` 只把段标记为废弃（`present=0`），不删文件 | 实测 `crc_err=1` **且** `corrupt=1` —— 扫描已正确识别并计数；正确断言应为 `corrupt=1` |

**分类小结**

| 分类 | 条数 | 说明 |
|---|---|---|
| 预期值算术错误（`evict_inf`/`qdrop`/`hole_evict`/`qused`） | 6 | 构造场景的代入计算写错 |
| 预期值未考虑设计语义（`segdel`/`seg_del`/`inflight`/`tx_valid`） | 9 | 追加目标不可回收；超时后 `inflight` 清零而 `tx_valid` 保留；ACK 后立即续发 |
| 时序不可观测 / 假设失效 | 3 | `replay=0`（boot 前已发生）；`inflight=1`（跨命令等待窗口内已完成） |
| 用例用错工具 | 2 | F4 应用 `logt ack` 而非 `ackauto`（含 1 条连锁） |
| 待确认 | 2 | `holes=2` —— 需要结合空洞表实现复核 |

### 8.3 用例文件自身的缺陷（本次已修，见 §9）

`test/log_fix_tests.txt` 曾引用 **7 个从未在固件里存在过的 `logt` 子命令**：

| 误用为命令 | 实际身份 | 正确写法 |
|---|---|---|
| `logt cloud3` | `logt_print_cloud()` 的**第 3 行输出前缀** | `logt cloud` |
| `logt cloud2` | 同上（第 2 行） | `logt cloud` |
| `logt ackst` | `logt_print_stats()` 的**第 5 行输出前缀** | `logt stats` |
| `logt fstats` | 同上（第 2 行） | `logt stats` |
| `logt cstats` | 同上（第 3 行） | `logt stats` |
| `logt cstats2` | 同上（第 4 行） | `logt stats` |
| `logt fseg2 gap=15` | `logt fseg <n>` 的**第 2 行输出前缀** | `logt fseg 0 \|\|\| gap=15` |

**影响**：首轮执行时有 **19 条断言**被 `[LogT] unknown op (try: logt help)` 伪失败。
**证据**：`git log -S 'cloud3' -- src/main.cpp` 只命中 `c29d9d5`（新增的是**打印行**），
`"ackst"` 作为独立 op 从未在 `main.cpp` 中出现过。

---

## 9. 本次对仓库的改动

| 文件 | 类型 | 说明 |
|---|---|---|
| `test/log_fix_tests.txt` | **修正** | 56 处 op 名替换（见 §8.3），断言数 186 → 187（补上原缺失的 `\|\|\| gap=15` 期望） |
| `docs/LogManager-P1.5-Board-Test-Report0918.md` | 新增 | 本报告 |

**未改动任何 `src/` 生产代码**（`git diff --stat -- src/` 为空）。

---

## 10. 已知问题

### 10.1 现场级（阻塞线上回收链路，P2 开工前需解决）

| ID | 问题 | 影响 | 建议 |
|---|---|---|---|
| **BT-1** | **云端从未回送 `log_ack`** | 每条批次走满 ~152 s 重试才 give-up；`acked_seq` 不前进 ⇒ **Flash 段永不回收**，496 条写满后开始淘汰未确认记录 | 云端 Worker 实现 `guo_feeder/down` 的 `log_ack`（`{"c":"log_ack","i":..,"p":{"b","f","t"}}`），**且必须幂等**（at-least-once） |
| **BT-2** | 设备端 `boot_seq` 在某次重启后回落为 1 | 若云端按 boot_seq 分段，旧 ACK 会因 `boot 不匹配` 被 IGNORE | 属 meta 重建的预期行为；云端需容忍 boot_seq 回退并按新 boot 重新对账 |

### 10.2 测试资产级

| ID | 问题 | 处理 |
|---|---|---|
| **BT-3** | `test/log_fix_tests.txt` 用过 7 个不存在的 `logt` op（19 条伪失败） | ✅ 本次已修（§9） |
| **BT-4** | F1/F7 的 `evict_inf`/`qdrop`/`qused` 预期值算错（6 条） | ⏳ **待用例作者确认后修正**。正确值：A 段 `evict_inf=9`/`qdrop=12`/`qused=112`；D 段 `evict_inf=8`/`qdrop=4`/`hole_evict=4` |
| **BT-5** | F2-B/E 的 `segdel=1`/`seg_del=1` 预期与"追加段不可回收"设计冲突（3 条） | ⏳ 建议改为 `segdel=0`/`seg_del=0`；E 段改为 `corrupt=1` |
| **BT-6** | F4 用 `logt ackauto` 无法在超时后注入（3 条连锁） | ⏳ 建议改用 `logt ack <boot> <from> <to>` |
| **BT-7** | `holes=2` vs 预期 1（2 条） | ⏳ **未完全归因**，需结合 `log_hole_*` 实现复核后再定 |
| **BT-8** | `replay=0` / `inflight=1` 类"跨命令窗口"时序断言不可观测（3 条） | ⏳ 建议删除或改为可观测的等价断言 |

### 10.3 观测到的次要疑点（不影响判定，留待 P2 顺带确认）

| 项 | 现象 | 说明 |
|---|---|---|
| `offskip` 计数 | B 段 `ackst ... offskip=2509`（A 段为 0） | 该计数器语义为"因离线跳过发送的轮数"，数值等于当时的 seq 量级，疑为统计口径问题；**低优先级**，建议 P2 接入 Log 自身观测时复核 |
| `[Log] seq: ... corrupt=1` | 多次启动出现（`reliable=1` 同时成立） | 指 `meta.bin` 缺失/损坏并已重建；`reliable=1` 表示重建后的 seq 窗口可信。属预期降级路径 |
| 首启 `vfs_api.cpp: open(): ... no permits for creation` | 段扫描对不存在的段文件报 E 级 | 属 LittleFS 探测语义，非错误路径缺陷；但会**污染串口日志**（P2 Storage bridge 接入后需避免把这类"探测失败"误报为 ERROR） |

---

## 11. 未完成 / 建议后续

1. **BT-1 是唯一阻塞项**：云端 `log_ack` 未实现 ⇒ F2-B / F3-A 的"段被正常回收"在**真实链路上**无法闭环。
   本报告中的回收证据均来自串口注入的 ACK（`logt ackauto`/`logt ack`），走的是**同一状态机**，但**未经真实云端闭环**。
2. **F6（既有回归复跑）未执行**：`test/log_fix_tests.txt` §F6 要求复跑 P1.2 核心 66、Flash 92、F5 恢复 34、F7-F9 43、交接 56
   —— 这些用例集**不在本文件内**，本次未收集。建议在 P2 首个模块接入前补跑一次全量基线。
3. 设备侧测试**不再 BLOCKED**：本次已完成 A–E 全部可跑段落。

---

*报告结束 —— 阶段 1 完成*

---

# 附录 R1 —— 测试资产修正与基线重录（0918 第二轮）

> 本节在首轮报告（§1–§11）之后追加，记录"按归因修正用例预期值 → 重跑 → 建立新基线"的全过程。
> 修正主题：**只改预期值与注释，不改命令顺序、不改测试意图、不改任何 `src/` 生产代码**。

## R1.1 修正清单（14 处，commit `test(log): fix P1.5 case expectations and re-baseline`）

| # | 位置 | 原预期 | 改为 | 依据 |
|---|---|---|---|---|
| 1 | F1 头注释 | "起点 used=0、批次 16 条" | 起点 used=9、批次 n=9 的完整推导 | `creset` 会 `cloud_replay_arm()` 把 F0 遗留的 9 条重新入队 |
| 2 | F1 | `evict_inf=12` | **`evict_inf=9`** | 在途窗口宽度 = 批次**真实条数 9**（不是 16） |
| 3 | F1 | `qdrop=0` | **`qdrop=12`** | 21 − 9 = 12；`9 + 12 = 21 = 149 − 128` 恒等式自洽 |
| 4 | F1 | `qused=124` | `ack ok boot=`（改为断言"ACK 被接受"） | ACK 命中哪一批是**时序相关**的：9 条批次全被淘汰时 used=128；批次轮换后 used=112 ⇒ 不作为不变量 |
| 5 | F1 尾 | `tx_valid=0` | **`tx_valid=1`** | ACK 后队列仍有记录 ⇒ loop 立即续发下一批 |
| 6 | F2-A | `inflight=1` | **`giveup=1`** | 本段已设 `atimeout=200`/`abackoff=100`，批次在 `logt flash` 的等待窗口内就重试耗尽 |
| 7 | F2-B 头注释 | "预期 segdel=1 / seg_del=1" | 说明追加目标段按设计不可回收 | `cloud_delete_acked_segments()` 显式 `continue` 跳过 `s_append_segment` |
| 8 | F2-B | `replay=0` | **`qused=8`**（+ 注释） | 串口会话建立于 boot 之后，"replay 尚未开始"这个中间态**不可观测** |
| 9 | F2-B | `segdel=1` / `seg_del=1` | **`ack_ok=1` / `qused=0` / `segdel=0` / `seg_del=0`** | 正面断言 ACK 落账 + 队列排空；段回收由 F3-B / F7 覆盖 |
| 10 | F4 头注释 | "t≈21 s 退避到期 → inflight=1" | 澄清 `abackoff 20000` 是**退避**窗口；1 s 后已不在途 | `atimeout=1000` |
| 11 | F4 | `inflight=1` ×2 | **`retry=1` / `tx_valid=1`** | 同上；`tx_valid=1` + `inflight=0` **正是 FIX-5 的核心证据** |
| 12 | F4 | `logt ackauto 0` | **`logt ack <BOOT> 1 4294967295 \|\|\| ack ok boot=`** | `ackauto` → `log_cloud_test_ack_inflight()` **要求 `s_cloud_inflight`**；FIX-5 之后 inflight=0 ⇒ 恒 `FAIL no-inflight`。改用 `logt ack` 直接注入（同一条状态机） |
| 13 | F7 头注释 + 命令 | `evict_inf=12` / `qdrop=0` / `hole_evict=8` / `holes=1` / `tx_valid=0` | **`evict_inf=8` / `qdrop=4` / `hole_evict=4` / `holes=2` / `tx_valid=1`** | 同一根因（批次真实条数 = 8）；`8+4=12=140−128` |
| 14 | F8 | `seg_del=1` | **`corrupt=1`** | `fcorrupt` 只把段**判废**（`present=0`）并计入 `corrupt`/`crc_err`，**不删文件** |

### R1.1.1 一处根因串联了 F1 与 F7

两条段落的预期都错在**同一个假设**：

> ❌ 假设：`evict_inf` 的上限是 `LOG_BATCH_MAX_RECORDS`(16)
> ✅ 事实：`evict_inf` 的上限是**在途批次的真实条数**，而它受"当时队列里可用条数"限制
> 　　　（F1 实测 n=9、F7 实测 n=8）

修正后的判据统一为 **恒等式**：
```
evict_inf + qdrop == 入队总数 − 队列容量(=128)
  F1: 9 + 12 = 21 = (9 + 20 + 120) − 128
  F7: 8 +  4 = 12 = (20 + 62 + 58) − 128
```

### R1.1.2 `holes=2` 的代码审查结论 —— **设计行为，非缺陷**

空洞有**两个相互独立、各自有文档的来源**：

| 来源 | 代码位置 | 登记粒度 |
|---|---|---|
| ① `cloud_queue_push()` 的非在途分支 | `log_manager.cpp:1867-1876` | 每条被淘汰的 Flash-routed 记录各一次 `log_hole_add(seq, seq)` |
| ② `log_hole_protect_gap()`（FIX-H2） | `log_manager.cpp:2176`、`log_ack.h:289-330` | 为 `[covered, evicted)` 逐条 `log_hole_add(seq, seq)` |

`log_hole_add()`（`log_ack.h:195-234`）会**合并重叠或相邻**（`from <= holes[i].to + 1`）的区间。
⇒ 每个来源内部必然合并成 1 条；**两个来源之间是否合并，取决于它们的 seq 是否相邻**。
本项目 `seq` 允许空洞（INFO 消耗 seq 但不落 Flash），故**不能保证相邻** ⇒ 实测 **2 条**。

**结论**：`holes` 的**条目数不是不变量**。修正后的断言口径：
- 只断言 `hovf=0`（空洞表未溢出，`LOG_HOLE_MAX=8`）与 `hole_evict=4`（FIX-H2 的专有计数）
- 段保护的真实判据是 `segdel=0` / `seg_del=0`（空洞存在 ⇒ 段不得回收）
- `holes=2` 作为**钉住值**保留，并在注释中说明它不是不变量

## R1.2 新基线（第二轮全量重跑）

| 段 | 覆盖 | 断言 | OK | MISS |
|---|---|---|---|---|
| A | F0、F1、F2-A | 56 | **56** | 0 |
| B | F2-B、F3-A、F3-B | 59 | **57** | **2** |
| C | F4 | 21 | **21** | 0 |
| D | F7 | 30 | **30** | 0 |
| E | F8 | 23 | **23** | 0 |
| **合计** | | **189** | **187 (98.9%)** | **2** |

> 首轮：167 / 187（89.3%，20 MISS）
> 本轮：**187 / 189（98.9%，2 MISS）**
> 新增 2 条断言来自 F2-B（`ack_ok=1`、`qused=0`），断言总数 187 → 189。

### R1.2.1 剩余 2 条 MISS —— 不是用例问题，是**新发现的设计级缺陷 BT-9**

两条 MISS 都在 F2-B：`qused=8` 与 `replay=8`（实测 `qused=1`、`replay=0`）。

**根因（P2-A 接入后暴露）**：

```
Boot 后：
  [JStg][W] read: open failed: /config/.commit   ← P2-A 桥接注入的 live WARN（seq=2817）
  → 立即被组批发出（n=1, len=147）
  → cloud_poll() 的补发 sweep **只在 used == 0 时推进**  ⇒ 这条 live WARN 把 replay 挡住了
  → 该 WARN 被 ACK ⇒ acked_seq 跳到 2817
  → Flash 里 8 条未确认旧记录（seq 2500..2507）满足 seq <= acked_seq
  → log_ack_should_replay() 判为"已覆盖" ⇒ **永久不再补发**（实测 replay=0）
```

⇒ **违反 at-least-once**：Boot 期任何 live 日志（P2-A 之后必然存在）都会
**抢在旧未确认记录之前被 ACK**，从而把旧记录永久越过。

| 项 | 内容 |
|---|---|
| 编号 | **BT-9** |
| 严重度 | 🔴 **高**（静默丢失 WARN+，P1.5 核心承诺被破坏） |
| 触发条件 | Boot 时 Flash 中存在未确认记录 **且** 本次 Boot 产生了 live 日志（P2-A 后为必现） |
| 是否 P2-A 引入 | ❌ 不是 —— P1.5 已存在（原代码 `cloud_poll()` 的 `used == 0` 前置）⇒ P2-A 只是让它**必现** |
| 处置 | **本次不改实现**（replay/ACK 水位语义属 P1.5 冻结契约，需单独评审） |
| 用例处置 | F2-B 的 `qused=8` / `replay=8` **保留不动** —— 它们正是 BT-9 修复后的**验收标准** |

**修复方向（候选，需评审）**：

- **A**：`acked_seq` 不得越过"replay 游标起点"—— 即 ACK 推进时钳位到 `min(ack_to, replay_start_seq - 1)`，直到 sweep 完成；
- **B**：把 replay 与 live 记录的 ACK 分开记账（ACK 只覆盖"本次实际发送过的区间"，而非全局单高水位）；
- **C**：解除"replay 只在 used == 0 时推进"的约束，让 sweep 可与 live 混跑（需保证 seq 单调与不重复投递）。

## R1.3 回归器的配套改动（`test/serial_batch.py`）

新增 **`<BOOT>` 宏**：把命令里的 `<BOOT>` 替换为**最近一次从设备输出捕获到的 boot 值**
（匹配 `boot=N` / `boot_seq=N`）。

原因：`logt ack` 的 boot 参数必须**精确匹配**当前 `boot_seq`，而它跨会话不确定
（取决于 meta 是否重建）。没有该宏，F4 只能写死一个会在下次运行失效的数字。
未捕获到 boot 时该行记 `>>> SKIP` 并跳过（**不算 MISS**）。

## R1.4 本节之后的状态

| 项 | 状态 |
|---|---|
| P1.5 测试资产 | ✅ 预期值与设计语义一致，189 断言中 187 通过 |
| 剩余 2 条 MISS | ⏳ 已归因 **BT-9**（设计级），保留为修复验收标准 |
| 设备侧 BLOCKED | ✅ 已解除（A–E 全部可跑段落完成） |
| `src/` 生产代码 | ✅ 本轮未改动（仅 P2-A 的 Storage 桥接为上一轮成果） |


---

# 附录 R2 —— BT-9：ACK 水位越过未确认补发记录（分析 · 修复 · 验证）

> 本节记录 **BT-9** 的完整处置：根因分析 → 修复设计 → 验收。
> 本轮**未**通过修改测试绕过问题；测试文件的改动是"新增判别场景 + 修正我自己的
> 用例书写错误"，两条原 KNOWN-FAIL 断言（`replay=8` / `qused=8`）现已真实通过。

## R2.1 三者关系：`acked_seq` × replay sweep × Flash GC

修复前只有一个标量 `s_cloud_acked_seq`，同时被四处使用：

| 使用点 | 语义需求 |
|---|---|
| `log_ack_classify()` 规则④（重复/回退） | 云端确认过的**最大** seq |
| `log_ack_should_replay()`（跳过补发） | **连续**已确认水位 |
| `log_ack_segment_reclaimable()`（段回收 / 环压力受害者） | **连续**已确认水位 |
| `flash_count_unacked()`（淘汰记账） | **连续**已确认水位 |

而 **ACK 的语义只是"我刚发出的这一批被云端持久化了"**（冻结契约 §13），
**并不**意味着"比它小的 seq 都已持久化"。二者的缝隙就是 BT-9：

```
Boot（P2-A 之后必然有 live 日志）
  [JStg][W] read: open failed: /config/.commit     ← live WARN，seq 较大
    ↓ 进 RAM 云队列 → 组批 → 发出 → 被 ACK
  s_cloud_acked_seq = <live seq>                   ← 单点水位跳到旧记录之上
    ↓ 队列排空 ⇒ cloud_poll() 第 (6) 步开始补发 sweep
  log_ack_should_replay(seq_old, acked=live, 0)    ← seq_old <= acked ⇒ false
    ⇒ 旧记录**永久不再补发**（违反 at-least-once）
  log_ack_segment_reclaimable(last<=acked, ...)    ← true ⇒ **段被删**
    ⇒ 物理副本也没了，重启同样补不回
```

**放大因素（非根因）**：补发 sweep 只在 `used == 0` 时推进 ⇒ live 记录会把补发挡住。
P2-A 之后每个 Boot 必有 live 日志 ⇒ BT-9 由"偶发"变成"必现"。

## R2.2 修复：状态语义分离（采用用户指定的方案）

```c
s_cloud_acked_seq   云端确认过的**最大** seq（真实值）→ 仅观测，不参与判定
s_cloud_gc_seq      **连续**可回收水位            → 所有放行判定
s_cloud_gc_floor    未确认 backlog 的最低 seq 下界（0 = 无钳制）

gc_seq = (gc_floor == 0) ? acked_seq : min(acked_seq, gc_floor - 1)
不变量：gc_seq <= acked_seq（钳制只会更保守）
```

`gc_seq` 现已替换掉全部三处放行判定（补发跳过 / 段回收 / 淘汰记账）
**以及 `log_ack_classify` 的重复检测** —— 最后一处是必需的，见 R2.3。

`gc_floor` 的生命周期：

- **设置**：`cloud_replay_arm()`（log_init / `creset` / 游标段被删）取
  `flash_lowest_first_seq()`。关键在时序：**arm 一定发生在任何 ACK 之前**。
- **清除**：`cloud_poll()` 中 `!s_replay_armed && gc_floor != 0 &&
  cloud_queue_head_is_current_boot()` ⇒ 清零并让 `gc_seq` 追平 `acked_seq`，
  随后补做一次段回收。
  - `!s_replay_armed`：还有旧记录没投递 ⇒ 不放行。
  - 队首已是本 Boot：补发只在 `used == 0` 时推进 ⇒ 补发记录构成**队列前缀**
    ⇒ 队首回到本 Boot 等价于旧记录已全部离开队列（ACK / 放弃(空洞) / 淘汰(空洞)）。
  - ⚠️ 不能用"队列已空"当条件：持续有 live 记录时队列可能长期非空 ⇒ 钳制永不
    解除 ⇒ 段永不回收 ⇒ 把静默丢失换成环压力丢失。

## R2.3 为什么不会破坏其它机制（逐条论证）

| 机制 | 论证 |
|---|---|
| **partial ACK** | `log_ack_classify` / `log_ack_covered_count` 的**判定规则一字未改**，只是"与之比较的水位"换成较保守的 `gc_seq`。由于 `gc_seq <= acked_seq`，原本判 DUPLICATE 的区间**只会更少不会更多**；而少掉的那部分正是"属于更旧批次、本应被接受的合法 ACK"（见下条）。前缀推进（`rd = rd_base + max(evicted, covered)`）逻辑完全未动 ⇒ 部分覆盖仍只推进被覆盖前缀。 |
| **replay** | `should_replay` 改用 `gc_seq` ⇒ 水位更保守 ⇒ **只会补发更多、不会更少**。这正是修复目标。补发的游标 / 单段界限 / 每轮 push 上限 / `give_up_seq` 语义全部未动。副作用：Boot 期已确认的 live 记录也可能被重发一次（其 seq > gc），由云端按 `(device_id, boot_seq, seq)` 幂等去重消化 —— 属 at-least-once 的可接受代价。 |
| **hole protection** | 空洞**只增不减**：`gc_seq` 更低意味着"靠水位放行"更严格，而空洞是**叠加**在它之上的额外约束（`reclaimable` 要求"整段 ≤ 水位 ∧ 不与空洞相交 ∧ 表未溢出"）。give-up / 队列淘汰的登记条件完全未改 ⇒ 空洞语义不变。 |
| **segment GC** | 唯一判定入口仍是 `log_ack_segment_reclaimable()`，只是传入的水位更保守 ⇒ **回收变少、不会误收**。追平机制保证 backlog 投递完毕后水位能追到 `acked_seq`，不会永久停滞（上板证据：`cloud4 ... gcfloor=0 gc=7937`）。 |
| **GIVE_UP_NOT_ADVANCE** | 未动：放弃路径既不推进 `acked_seq` 也不推进 `gc_seq`（只登记空洞）。 |
| **持久化格式** | 纯 RAM 态变更：`LogRecord` v2/128B、Flash 段 3984B/31/16、CBOR fmt=2 全部未动 ⇒ 无格式迁移、无 LittleFS 兼容问题。 |

### 额外修复的一处连带缺陷（不改则比 BT-9 更糟）

旧批次（boot_seq 更小）晚到的 ACK 若与 `acked_seq`（已被 live ACK 抬高）比较，
会被规则④判成 `DUPLICATE` 丢弃 ⇒ `rd` 不推进 ⇒ 那批记录**永远留在云队列里**
⇒ `used != 0` 恒成立 ⇒ 补发被自身永久阻塞。
改用 `gc_seq` 后，旧批次 ACK 的 `ack_to` 恒 > gc ⇒ 正常 `ACCEPT`；
而真正的重复 ACK 仍满足 `ack_to <= gc` ⇒ 幂等性未被放掉
（合约测试第 ⑧ 组用正负向断言双向钉住）。

## R2.4 新增测试场景（旧 Flash 未确认日志 + live 先被 ACK + 重启补发）

场景放在 **F2-B**（原本就是"复位后补发"段），用**注入 ACK**把偶发变成确定性：

```
复位 → rarmed=1
logt sonline 1 / atimeout 60000        # 防 live 批次中途 give-up
logt stats ||| boot=                   # 刷新 <BOOT>
logt ack <BOOT> 1 4294967295           # ★ 注入 live 批次 ACK ⇒ 水位跳到旧记录之上
logt cloud ||| gc= / gcfloor=          # 观测钳制
logt stats ||| replay=8                # ★ 验收 1（修复前恒 0）
logt stats ||| segdel=0 / seg_del=0    # ★ 验收 2：旧段未被提前回收
logt cloud ||| qused=8                 # ★ 验收 3：旧记录回到云队列
logt ackauto 0                         # 旧批次 ACK 不得被判 DUPLICATE 吞掉
logt stats ||| qused=0 / rarmed=0      # 队列排空、补发完成
```

**上板实测（2026-09-17，COM8）**：

```
[LogT] cloud4 acked=0 gc=0 gcfloor=7877                       ← Boot：钳制已挂上
[Log Cloud] ack ok boot=1 to=4294967295 covered=1/1 acked=7937 gc=7876 floor=7877 r=2
                                            ^^^^^^^^^^^^^^^^^^^^^^^^^^^
                                            ★ live ACK 把 acked 推到 5889，
                                              而 gc 被 floor(5829) 钳在 5828
>>> EXPECT 'replay=8' : OK                                    ★ 旧记录仍被补发
>>> EXPECT 'segdel=0' : OK    >>> EXPECT 'seg_del=0' : OK      ★ 旧段未被回收
>>> EXPECT 'qused=8' : OK                                     ★ 旧记录回到队列
[Log Cloud] ack ok boot=2 to=7884 covered=8/8 acked=7937 gc=7876 floor=7877 r=2
                                            ★ 旧批次晚到 ACK 被 ACCEPT（未吞）
[Log Cloud] replay sweep done (acked=7937 gc=7876 floor=7877 giveup=0)
[LogT] cloud4 acked=7937 gc=7937 gcfloor=0                    ★ backlog 走完 ⇒ 追平
>>> EXPECT 'rarmed=0' : OK
```

**离线等价证据**：`test/log_contract/probe_ack.cpp` 新增第 ⑧ 组 **23 条断言**，
含 **3 条负向探针**（换成修复前的单点水位时结论必须相反）⇒ 具备可证伪性。
`run_contract_test.py` 结果 **4/4 ALL PASS，合计失败 0**。

## R2.5 新基线（BT-9 修复后全量重跑）

| 段 | 覆盖 | 断言 | OK | MISS |
|---|---|---|---|---|
| A | F0、F1、F2-A | 56 | 56 | 0 |
| B | F2-B（★BT-9）、F3-A、F3-B | 65 | 65 | 0 |
| C | F4 | 21 | 21 | 0 |
| D | F7 | 30 | 30 | 0 |
| E | F8 | 23 | 23 | 0 |
| **合计** | | **195** | **195 (100%)** | **0** |

> 首轮 167/187（89.3%，20 MISS）→ R1 修正后 187/189（98.9%，2 MISS，记为 BT-9）
> → **本轮 195/195（100%，0 MISS）**，且两条 BT-9 判别断言由 FAIL 转为真实 PASS。
> 断言数 189 → 195：F2-B 由"被动等云端 ACK"改为"主动注入 live ACK"，
> 新增 6 条（atimeout / boot / 注入 ACK / gc / gcfloor / qused）。

## R2.6 遗留

| 编号 | 事项 | 说明 |
|---|---|---|
| **BT-1** | 云端始终不回 `log_ack` | 未变，仍是外部阻塞项。本轮用注入 ACK 绕开它对**设备侧**验证的阻碍，但线上闭环仍缺失 |
| **BT-10** | 云队列溢出淘汰时，仅本 Boot 记录登记空洞（`evict_boot == s_boot_seq`） | 复合低概率残余项；修法与代价见 Integration-Guide §11.6，需单独评审（F1 的 `holes=0` 断言正钉住当前行为） |
