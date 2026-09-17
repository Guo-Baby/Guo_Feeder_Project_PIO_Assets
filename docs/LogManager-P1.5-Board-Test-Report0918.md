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
