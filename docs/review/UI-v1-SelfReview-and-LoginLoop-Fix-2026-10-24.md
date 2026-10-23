# UI v1 全面自查 + 登录死循环故障修复报告（2026-10-24）

> 触发：用户报告「登录页正确输入口令后无限刷新，电脑/手机一样」。
> 范围：① 该故障根因与修复；② 对新写 UI（gf-app.js / gf-ui-core.js / app.html /
> login.html / Pages functions）的全面自查；③ 修复、测试、部署与生产验证。

---

## 1. 登录死循环 —— 根因与修复

### 1.1 故障链（双轨分裂）

生产 `SESSION_STORE=d1` ⇒ 登录签发**形态 A** 会话（`gf_session=<sid>.<secret>`，
D1 可撤销）。而四个调用点用的旧 `readSession()` **只验形态 B**（HMAC 本地令牌）：

| 调用点 | 旧行为（形态 A cookie） | 后果 |
|---|---|---|
| `_middleware.js`（页面门禁） | 验签失败 ⇒ 判“未登录” | `/app` **302 回登录页** |
| `api/auth/login.js`（GET 探测） | 同上 ⇒ `authenticated:false` | 登录页认为没登录 |
| `api/read/[[path]].js`（只读代理） | 同上 ⇒ **401** | SPA 所有只读请求失败 |
| `api/auth/logout.js` | 同上 ⇒ `was_authenticated:false` | 仅日志失真 |

而死循环的另一半在 `login.html`：它调 `/api/auth/session`（**用的是正确的
`readSessionFull`**）⇒ 报告“已登录” ⇒ `location.replace('/app')` ⇒ 中间件又踢回
登录页 ⇒ 再跳转……**两个端点对同一 cookie 给出相反答案**，用户看到的就是
“登录成功后无限刷新回登录页”。电脑/手机一致，因为与服务端判定有关，与终端无关。

### 1.2 为什么之前没发现

- 第四轮验收的 e2e 走的是 **Worker `/api/ui/session`（Bearer）** 机器路径，
  从未走 Pages 口令登录；"Pages 登录 Turnstile 未验"本就是列名的 NOT VERIFIED 项。
- 全部单测构造的都是**本地签的形态 B cookie**（`sess.signSession`），
  旧 `readSession` 在测试里表现完全正确 —— **测试绿 ≠ 生产通**的又一实例。

### 1.3 修复（5 处 + 1 兜底）

1. `_lib/session.js`：`readSession()` 改为 `readSessionFull()` 的薄包装
   （真值语义不变），并注明“新代码一律用 `readSessionFull`”。
2. `_middleware.js`：显式改用 `readSessionFull` + `s.ok === true`。
3. `api/auth/login.js` GET 探测、`api/auth/logout.js`、`api/read/[[path]].js`：
   同样改用 `readSessionFull`。
4. `login.html` 防死循环兜底：5 秒内第二次被弹回登录页即**停止跳转**并明示
   “会话校验异常，请清除 cookie 重试”，不再让用户面对永转页面；
   主动提交登录时清除守卫标记（防“登录→立刻登出→再登录”被误判）。

### 1.4 回归测试（pages_auth 新增第 5.5 组 · 15 项）

形态 A 端到端：签发 ⇒ 中间件放行（**修复前这里是 302**，已用旧路径实测证明
`verifySession(形态A)===null`）⇒ 代理放行 ⇒ 探测认可 ⇒ 撤销后拒绝 ⇒
fail-closed（形态 A + local store 拒绝）⇒ **完整故障场景**（登录⇒进 /app 不循环）。

---

## 2. 全面自查发现的其他 bug（按严重度）

### P0 · query/workflow 命令全部发不出正确的定位键（功能性，生产必现）

`runCommand()` 解构驼峰 `actionId`/`workflowId`，而**全部 query 族调用点传下划线
`action_id`**、workflow 运行传 `workflow_id` ⇒ `payload.action_id = undefined`
被 `JSON.stringify` 丢弃 ⇒ POST 体没有定位键 ⇒ Worker 一律 `400 unknown_query`。
**受害面**：registry 拉取、阀门状态查询、workflow.list、config_query×7、
诊断查询×3、workflow 运行 —— 即除 action/safety 外的**全部命令**。
（冒烟测试抓 POST 体时实测：`{"device_id":…,"family":"query","params":{}}`。）

修复：`runCommand` 双键名归一（`o.actionId ?? o.action_id`）+ query/safety/system
族缺 id **本地硬拦截** + 未知族拒绝；6 个调用点统一驼峰。T-8 组断言锁死。

### P1 · 导航竞态抛未处理 rejection（5 处）

查询轮询要数秒，期间切走页面 ⇒ `el("valve-state")` / `el("d-out")` /
`el("log-table")` / `el("log-meta")` / `el("log-level")` 返回 null 后继续点属性
⇒ TypeError ⇒ async onclick 里**没人接的 rejection**（控制台报错，用户无感但污染）。
修复：全部加 null 护栏。

### P1 · 记录页 tab 往返行数翻倍（功能性，用户可见）

`loadLogs()` 每次都往 `S.logRows` 叠加 ⇒ 来回切“告警/日志”tab，日志表格**重复累积**。
修复：进 tab/点“加载”即重置 + **代际守卫**（`logGen`/`alertGen`/`deviceGen`），
旧在途响应到达即丢弃。告警页、设备页同样加代际守卫。

### P2 · 日志“更多”到底后重复拉全表

`next_before_id` 为 null 时 cursor 归零，再点“更多”会**从头重复**并叠加。
修复：`has_more=false` 时禁用按钮（与告警页一致）。

### P3 · 若干（已修）

- 分钟格式吞掉亚秒超出量（60006 ms → “1 分 0 秒”与阈值无差别）⇒ `formatExcessMs` 精示（上一轮已修，本轮复核确认）。
- 设备页诊断 KPI 在 metrics 缺失时显示**空白**而非“—”。
- 设备页 7 个 config_query 顺序查完才整体渲染 ⇒ 白屏十几秒；改**逐行渐进渲染**。
- `renderControl` 死变量 `valveState`。
- 登录页守卫误判（登录→5 秒内登出再登录会被当成循环）⇒ 提交时清除标记。

### 已知限制（非 bug，留档）

- **单设备**：`ensureDeviceId` 从最近日志取**第一个** device_id，无设备切换器。
  多设备场景会静默操作错设备 —— 设备页/devicebar 有明示，切换器留待后续版本。
- 控制页“60 秒安全超时”为**硬编码文案**（与设备实测 `safety_timeout_sec=60` 一致），
  未从 config_query 动态读取；设备改配置后 UI 文案会过期。
- CSRF 在降级会话（形态 B，Worker 挂了的读 degraded 模式）下**写操作一律 403**
  （既有设计：形态 B 不签发 csrf）—— 故障期间 UI 自动变只读，符合 fail-closed。

---

## 3. 测试与验证

### 3.1 自动化（全量 7 套件 · 611 PASS / 0 FAIL）

| 套件 | 结果 | 本轮变化 |
|---|---|---|
| ui_security | 273 | — |
| pages_auth | **104** | **+15**（形态 A 端到端，§1.4） |
| pages_webhook | 22 | — |
| log_decode | 29 | — |
| history_frames | 33 | — |
| ui_app | **117** | **+9**（runCommand 双键名/硬拦截/代际守卫/login 守卫断言） |
| **ui_app_runtime（新）** | **33** | DOM 桩驱动真实 SPA：五路由/命令交互/告警+日志翻页/流程运行/设备页/**导航竞态无 rejection**/401 回登录/P0 键名锁/tab 防重复 |

### 3.2 生产只读实测（12 PASS / 0 FAIL · `scripts/verify_login_loop_fix_prod.py`）

用**真实 D1 可撤销会话**（与浏览器登录同形态同 store）走全链路：
带会话 `/app.html` **200 SPA**（修复前 302 循环）· 只读代理 **200 JSON**（修复前 401）·
两个探测端点都认已登录 · **撤销后**回落登录页 + 代理 401 · 无 cookie 仍受保护。

---

## 4. 提交与部署基线

| 项 | 值 |
|---|---|
| Pages（本版，含全部修复） | **`3e616337`**（Compiled Worker + Functions bundle 均在） |
| Worker（本轮无改动） | `bae88be7-46c7-4644-abc2-bb50939be8a8` |
| 固件仓 | **无代码改动**（两轮均为云端/UI 侧修复） |
| 提交 | 云仓 3 个 commit（fix 中间件/代理/探测 + P0 键名 + 竞态/翻页 + 测试） |

**回滚**：Pages 回滚到上一 deployment 即可；无 D1/固件变动。

---

## 5. 给用户的行动项

1. **现在可以重新登录**：修复已部署到 `guo-feeder-pagesdev.pages.dev`。
   之前“无限刷新”的根因是服务端把已登录会话判成未登录，**你的口令一直是对的**。
2. 若浏览器里还留着故障期间反复登录产生的多个旧会话，**无需处理**（都有效，
   12 小时后各自过期）；也可以在登录页直接登出清零。
3. 真机走查清单（Turnstile 需真人，仍无法自动化）：开阀（含二次确认）/ 关阀 /
   急停 / 解除锁存 / 记录页告警翻页 / 流程页运行 / 设备页配置只读。
4. 仍为 BLOCKED：HX711、DRV8825、GPIO 电气、Turnstile 真验、D1 故障路径、
   config_query 二帧超时；workflow 首跑与 registry 漂移自恢复未真机触发。
