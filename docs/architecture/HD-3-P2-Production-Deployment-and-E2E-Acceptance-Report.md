# HD-3 · `P-2` 生产部署与真机 MQTT 端到端验收报告

- 执行日期：**2026-10-19**（UTC `12:55` – `13:00`）
- 设备：`288485896ce4`（`dev_288485896ce4`，EMQX 在线）
- 目标：将已本地验收的 `P-2`（`cloud_api` 下行主题）修复部署到生产 Worker，并通过**生产 `cloud_api` 路径**驱动真实设备完成端到端验收
- 结论：**通过**（四个层次全部闭合；详见 §8）

> 阅读纪律：本报告严格区分「部署成功」「消息发布成功」「设备实际收到」「完整响应成功」四层，
> 不以 `HTTP 200` 冒充 MQTT 成功，不以「Worker 记录」冒充「设备已执行」。

---

## 1. 部署记录

### 1.1 部署前基线（只读查询）

| 项 | 值 |
|---|---|
| Worker 名称 | `guo-feeder-api`（`wrangler.toml` `name="guo-feeder-api"`） |
| 入口 | `main="src/index.js"` |
| 路由 | `https://guo-feeder-api.guobaby.workers.dev`（`workers_dev`） |
| 绑定 | `env.guofeeder_DB` (D1 `guofeeder`) · `env.LOG_BOOT_WATERMARK="1"` |
| Cron | `0 3 * * *` |
| **部署前活动 deployment** | `c28f2eba-2f5e-49c0-8205-206b15d9858e`（`2026-10-18T20:18:44Z`） |
| **部署前活动 version** | `f9647a00-e079-4de9-bcf1-8c9bc5344185`（number **46**，含旧 `P-2` 缺陷） |
| 回退目标（部署前上一版） | deployment `9f4fee53-d382-4ce5-905c-ca734c7ca2ab` / version `508461f3-0dcf-443c-98f9-d185f412174e`（number 45） |

> 术语澄清：`f9647a00` / `508461f3` 是 **script version id**，不是 deployment id。
> 二者的 deployment id 分别是 `c28f2eba` / `9f4fee53`。

### 1.2 部署前本地验证（本轮重新执行，非引用历史）

| 验证 | 结果 |
|---|---|
| `test/current/cloud_api_downlink_topic.test.mjs` | **36 / 36 PASS**（FAIL 0） |
| `test/run_all.mjs`（全量 21 套件） | **通过**；`所有套件通过（含 legacy EXPECTED_FAILURE，属预期）` |
| `wrangler deploy --dry-run` | 通过；上传 150.78 KiB / gzip 37.88 KiB |

### 1.3 待部署差异审查

`git status` 显示 10 改 + 1 新增（+250 / −78），**全部落在测试与文档 + `P-2` 修复范围内**：

| 文件 | 性质 |
|---|---|
| `src/index.js` | `P-2` 修复主体（删除 `publishToEMQX()`、`cloud_api` 使用设备专属 topic、失败返回 502） |
| `src/emqx-admin.js` | **仅注释更正**（旧注释引用已删除的函数） |
| `migrations/0006_*.sql` | **仅注释**（删除过期的「草案 / NOT APPLIED」描述，SQL 语义零改动） |
| `migrations/README.md`、`test/real_d1/README.md` | 文档 |
| `test/current/{hard_retirement,redaction}.test.mjs`、`test/run_all.mjs` | 既有测试适配 + 新套件注册 |
| `test/real_d1/{watermark_rebase,log_conflict}_real_d1.sh` | 测试脚本修复（R-0d 断言 / 反引号命令替换） |
| `test/current/cloud_api_downlink_topic.test.mjs` | 新增（36 项） |

结论：**无未验收的行为变更**。唯一涉及运行时行为的改动就是 `src/index.js` 的 `P-2` 修复。
（`wrangler` 只打包 `src/`；`migrations/*.sql` 的注释变更不会触发任何 DDL。）

### 1.4 部署执行

```
cd Cloudflare_Assets/guo-feeder-api
wrangler deploy
```

输出要点：

```
Total Upload: 150.78 KiB / gzip: 37.88 KiB
Worker Startup Time: 1 ms
Uploaded guo-feeder-api (3.87 sec)
Deployed guo-feeder-api triggers (2.54 sec)
  https://guo-feeder-api.guobaby.workers.dev
Current Version ID: 826332ed-3a73-42c4-a090-a5bf737dc210
```

### 1.5 部署后核实

| 项 | 值 |
|---|---|
| **部署后活动 deployment** | `c666aeeb-150e-4b10-a155-59a13bdaab0b` |
| **部署后活动 version** | `826332ed-3a73-42c4-a090-a5bf737dc210`（number **47**） |
| 部署时间 | `2026-10-19T12:56:06.861548Z` |
| 提交人 | `wyl94@foxmail.com` |
| 新回退目标 | deployment `c28f2eba` / version `f9647a00`（number 46） |

---

## 2. 变更确认（生产实际生效代码是否含修复）

不依赖「打包成功」推断，用**行为探针**验证线上运行的是修复版。

| 探针 | 请求 | 期望（修复版） | 实测 | 结论 |
|---|---|---|---|---|
| N-1 缺 `device_id` | `POST /api/command` `{"source":"cloud_api","c":"system",...}` | **400** `invalid_device_id` | **400** + `invalid_device_id` | 修复版生效 |
| N-2 大写 `device_id` | `{"device_id":"288485896CE4",...}` | **400**（`isValidDeviceId` 要求 12 位小写 hex） | **400** | 修复版生效 |
| N-3 缺 `device_id`（经 Pages） | `POST https://guo-feeder-pagesdev.pages.dev/api/command` | **400** | **400** | Pages → Worker 全链路生效 |
| N-4 副作用 | 上述三次负向请求是否写入 `mqtt_messages` | 0 行 | `WHERE msg_id LIKE 'p2neg%'` = **0** | 拒绝发生在发布之前 |

> 反证：旧版对 N-1 会返回 **200 `OK`** 并把消息发到 `guo_feeder/down`（无订阅者）。
> 线上现在返回 400，说明 **`f9647a00` 已被 `826332ed` 取代**。

---

## 3. 真机测试证据

### 3.1 测试方法

- 入口：`POST https://guo-feeder-pagesdev.pages.dev/api/command`
  → Pages `functions/api/command.js` 追加 `source="cloud_api"` → 转发 `https://guo-feeder-api.guobaby.workers.dev/`
  → Worker `cloud_api` 分支 → `publishToDevice()` → EMQX `/api/v5/publish`
- **未使用**直接向 MQTT topic 发送消息的方式（那无法验证 Worker 发布路径）
- 命令体（只读 `config_query`，`valve` 模块，无任何硬件副作用）：

```json
{"device_id":"288485896ce4","c":"system","i":"<唯一ID>","p":{"o":"config_query","module":"valve"}}
```

- 每次测试使用**全新命令 ID**（规避 30 s 去重静默丢弃）：`p21792414675`、`p2b1792414735`

### 3.2 设备与会话基线（发布前，12:57:32 UTC）

| 项 | 值 |
|---|---|
| `connected` | `true` |
| IP | `110.180.51.142` |
| `connected_at` | `2026-10-18T20:21:42.975Z`（当时已连续在线约 16.6 h，测试期间未重连） |
| `send_cnt` / `recv_cnt` | `1135` / `1136` |
| `mqueue_len` / `mqueue_dropped` | `0` / `0` |
| 订阅（EMQX 实证） | `guo_feeder/288485896ce4/down` · QoS **1** · `nl=0` |
| `mqtt_messages` 基线 | `MAX(id)=4501`，`COUNT(*)=4483` |

### 3.3 命令 1 —— `p21792414675`

API：`HTTP 200` body `OK`，耗时 **2.97 s**（含 Pages 冷启动）
请求发起：`12:57:56.287 UTC`

| 阶段 | 记录 | topic | 时间（UTC） | 相对下发 |
|---|---|---|---|---|
| 下行 | id 4502 | `guo_feeder/288485896ce4/down` | 12:57:58.194 | +0.000 s |
| ACK 帧 | id 4503 | `.../up` | 12:58:02.671 | **+4.477 s** |
| 受理帧 `s=1` | id 4505 | `.../up` | 12:58:03.444 | **+5.250 s** |
| 终帧 `s=0` | id 4506 | `.../up` | 12:58:04.210 | **+6.016 s** |

（另有 id 4504 为 EMQX 规则侧对同一 `down` 的重复采集，topic 带 `$tenants/n302933b/` 前缀，见 §6）

下行 payload（D1 原文）：
```json
{"device_id":"288485896ce4","c":"system","i":"p21792414675","p":{"o":"config_query","module":"valve"},"source":"cloud_api"}
```
ACK：
```json
{"c":"ack","i":"p21792414675","o":"config_query","p":{"result":"received"},"t":1792414680}
```
受理帧：
```json
{"s":1,"c":"result","i":"p21792414675","t":1792414680,"m":"config query queued","command":"system"}
```
终帧：
```json
{"s":0,"c":"result","i":"p21792414675","o":"config_query","t":1792414680,"m":"query module ok","e":0,"v":1,
 "type":"config_result","command":"system",
 "data":{"gpio_pin":12,"active_level":1,"open_duration_ms":0,"safety_timeout_sec":300,"enable":true}}
```

### 3.4 命令 2 —— `p2b1792414735`（可重复性）

API：`HTTP 200` body `OK`，耗时 **0.76 s**（Pages 已热）
请求发起：`12:58:56.270 UTC`

| 阶段 | 记录 | topic | 时间（UTC） | 相对下发 |
|---|---|---|---|---|
| 下行 | id 4508 | `guo_feeder/288485896ce4/down` | 12:58:57.159 | +0.000 s |
| ACK 帧 | id 4510 | `.../up` | 12:58:58.798 | **+1.639 s** |
| 受理帧 `s=1` | id 4511 | `.../up` | 12:58:59.567 | **+2.408 s** |
| 终帧 `s=0` | id 4512 | `.../up` | 12:59:00.333 | **+3.174 s** |

终帧数据与命令 1 **完全一致**（`gpio_pin=12 / active_level=1 / open_duration_ms=0 / safety_timeout_sec=300 / enable=true`）。
`e=0`，`data` 位于**顶层**（非 `p.data`）。

### 3.5 EMQX 侧计数（独立佐证）

| 时刻 | `send_cnt` | `recv_cnt` | `mqueue_len` / `dropped` |
|---|---|---|---|
| 12:57:32（基线） | 1135 | 1136 | 0 / 0 |
| 12:58:12（命令 1 后） | **1141** | **1141** | 0 / 0 |

`send_cnt` 增长 = broker 向该客户端投递了 PUBLISH（含下行命令）；`recv_cnt` 增长 = 收到设备上行帧。
`mqueue_dropped=0` ⇒ 无投递丢弃。

### 3.6 端到端耗时判断

| | 命令 1 | 命令 2 | 历史基准（10-19 直接发布） |
|---|---|---|---|
| ACK | 4.48 s | 1.64 s | 1.19 s |
| 终帧 | 6.02 s | 3.17 s | 2.67 s |

两轮均落在同一数量级。命令 1 偏慢（4.5 s）出现在 Pages 冷启动 + 设备同时在上传日志的时段
（同期有 `log_ack ack:21:5168` 记录），**判定为合理波动**，非链路异常。

---

## 4. 结果分类

| # | 检查项 | 结果 | 证据 |
|---|---|---|---|
| 1 | 生产活动版本 = 修复版 | **PASS** | deployment `c666aeeb` / version `826332ed`（v47）；负向探针 400 |
| 2 | `cloud_api` 使用设备专属下行主题 | **PASS** | D1 id 4502 / 4508 topic = `guo_feeder/288485896ce4/down` |
| 3 | 不再发布到 `guo_feeder/down` | **PASS** | 近 16 小时内该 topic 新增 **0** 行（`created_at > 1792400000000` = 0） |
| 4 | 真机收到只读命令 | **PASS** | 设备 ACK 帧（id 4503 / 4510），ACK 契约 `p.result="received"` |
| 5 | ACK / 受理帧 / 终帧完整闭合（同 ID） | **PASS** | 每轮各 3 条上行帧，`i` 与下发完全一致 |
| 6 | 终帧查询成功且数据合理 | **PASS** | `s=0 · e=0 · data` 五项字段齐备，两轮逐字一致 |
| 7 | 缺失/非法 `device_id` 明确拒绝 | **PASS** | 400 `invalid_device_id`（N-1/N-2/N-3），零消息落库（N-4） |
| 8 | 不静默回落通用主题 | **PASS** | 代码路径无 fallback；负向请求未产生任何 MQTT 消息 |
| 9 | API 未把发布失败伪装成成功 | **PASS（正向）** | 成功路径 200；失败路径代码返回 502 且带 `status`/`code` —— 但见 §5-R1（未做真实失败注入） |
| 10 | 链路可凭时间戳重建 | **PASS** | §3.3 / §3.4 四帧毫秒级时间戳齐全 |
| 11 | 设备在线状态未受测试影响 | **PASS** | 全程 `connected=true`，`connected_at` 未变（无重连） |
| 12 | Worker 实时日志中的 `[cloud_api] published ...` 行 | **NOT VERIFIED** | `wrangler tail` 经代理 502，直连被沙箱拦截；以 D1 + 设备响应替代 |
| 13 | 真实发布失败分支（502）注入 | **N/A（未执行）** | 生产环境不得人为制造发布失败；由单测 36 项覆盖 |
| 14 | Pages / Android 调用方是否携带 `device_id` | **NOT VERIFIED** | 见 §7-R2 |

---

## 5. 问题及修复记录

本轮**未发现新缺陷**，无需补救性修复或二次部署。下表为已知但未阻塞验收的观察项：

| 编号 | 观察 | 级别 | 说明 |
|---|---|---|---|
| R1 | `wrangler tail` 无法取证 | MINOR | 代理隧道 502 / 直连被沙箱拦；属工具链限制，非产品缺陷 |
| R2 | 下行 payload 携带 `device_id`、`source` 到设备 | NIT | 修复后 `payload = body`，故设备会收到 `device_id` 与 `source` 字段；固件按 `c` 字段判定格式并忽略未知键 ⇒ 实测 `e=0` 正常。若后续要严格裁剪，需另开任务 |
| R3 | 同一条下行在 `mqtt_messages` 中有两行 | MINOR（既有） | Worker 自身写 1 行（`guo_feeder/.../down`）+ EMQX 规则采集 1 行（`$tenants/n302933b/...`）。**修复前即存在**，本轮未改变 |

---

## 6. 副作用说明

本次在生产环境留下了以下**真实写入**（不声称"生产零写入"）：

| 类别 | 数量 | 明细 |
|---|---|---|
| `mqtt_messages` 新增行 | **12 行**（id 4502–4513） | 命令 1：4502–4506（5）；命令 2：4508–4512（5）；另 4507、4513 为设备例行 `log_ack`（`ack:21:5168/5169`，与本测试无关） |
| 其中 `cloud_api` 相关 | 10 行 | 每轮 = 1× Worker 下行 + 1× EMQX 规则下行副本 + 1× ACK + 1× `s=1` + 1× `s=0` |
| Worker 版本 | +1 | v47 `826332ed`，新 deployment `c666aeeb` |
| D1 schema / migration | **0** | 未 apply 任何 migration，`d1_migrations` 无变化 |
| 设备状态 / 配置 | **0** | 命令为只读 `config_query`（`o=config_query`），未执行 `config_set` / `config_save` / `memory` |
| EMQX 配置 / ACL | **0** | 仅调用 `/publish` 与只读 `GET /clients*` |
| 设备复位 / 烧录 | **0** | 未触碰串口、未断电 |
| Git | **0** | 未 commit / push / reset / clean；工作区与部署前一致 |

Legacy 主题 `guo_feeder/down` 的历史累计 404 行**均为旧数据**，本轮 0 新增。

---

## 7. 剩余风险

| 编号 | 风险 | 影响 | 建议 |
|---|---|---|---|
| R-2-1 | **Pages 前端 / Android 调用方是否携带 `device_id`** 未核实 | 若调用方仍按旧格式（无 `device_id`）下发，现在会得到 **400**（此前是静默 200 + 消息丢失）。表现为「命令发不出去」 | 核查 Android 端请求体；必要时在 Pages 侧补 `device_id`。**这是行为变更的兼容性风险，优先处理** |
| R-2-2 | 真实发布失败（502 分支）未在生产验证 | 该分支逻辑仅由单测覆盖 | 维持单测门禁；不做生产故障注入 |
| R-2-3 | `wrangler tail` 取证通道不可用 | 线上实时日志无法直接读取，排障依赖 D1 + EMQX | 需要时改用 Cloudflare 控制台 Logs |
| R-2-4 | 生产 rebase 仍未执行 | `max_boot_seq=117`、`epoch=1`、`last_rebase_id=NULL`，`rewind_count` 继续累积，`REWIND_CANDIDATE` 误报持续 | 与 `P-1` 同批授权后执行（本次未授权） |
| R-2-5 | `log_ack` 仍为 QoS 1，`cloud_api` 为 QoS 0 | 与修复前一致（旧 `publishToEMQX()` 也是 `qos:0`）。**本轮未改变契约** | 如需统一 QoS，另行评估 |

---

## 8. 最终验收结论

### 8.1 四个层次分别判定

| 层次 | 判定 | 依据 |
|---|---|---|
| **① 部署成功** | **PASS** | `wrangler deploy` 成功；活动 deployment `c666aeeb` / version `826332ed`（v47）；部署时间与输出完整留存 |
| **② 消息发布成功** | **PASS** | Worker 记录的 down topic 精确为 `guo_feeder/288485896ce4/down`；EMQX 侧 `send_cnt` 增长；`mqueue_dropped=0` |
| **③ 设备实际收到** | **PASS** | 设备产生同 `i` 的 ACK 帧（1.6 / 4.5 s），`p.result="received"`；`recv_cnt` 同步增长 |
| **④ 完整响应成功** | **PASS** | 同 ID 的 `ack` + `s=1` + `s=0(e=0)` 三帧齐全；终帧顶层 `data` 五项字段合理；两轮结果逐字一致 |

### 8.2 验收规则逐条核对

| 条件 | 结果 |
|---|---|
| 生产活动版本确认是修复后的版本 | ✅ `826332ed`（负向探针 400 佐证） |
| 生产 `cloud_api` 使用正确设备专属下行主题 | ✅ D1 双轮实证 |
| 真实设备成功收到只读查询命令 | ✅ ACK 帧 ×2 |
| 同 ID 的 ACK / 受理 / 成功终帧完整闭合 | ✅ 两轮各 3 帧 |
| 终帧数据正确、无未解决阻塞问题 | ✅ `e=0` + `data` 完整 |
| 部署与测试记录完整、未越操作边界 | ✅ 见 §6 |

### 8.3 门禁状态（本轮**不改变**）

| 门禁 | 状态 | 依据 |
|---|---|---|
| `BT-1` | **OPEN** | 本轮未按 BT-1 验收流程验证 |
| `G-4` | **BLOCKED** | 无关闭证据 |
| `WP-5` | **未解锁** | 无解锁证据 |
| `STOP-R1` | **未解除** | 无书面解除 |

> 本轮通过仅代表 **`P-2` 生产部署及本次 MQTT 端到端测试通过**。
> **不**关闭 `BT-1` / `G-4` / `WP-5` / `STOP-R1`，**不**代表生产 rebase 已完成。

### 8.4 定级

```
P-2 生产部署与真机端到端验收 = PASS
（附带 R-2-1「调用方需携带 device_id」为待办兼容性核查项）
```
