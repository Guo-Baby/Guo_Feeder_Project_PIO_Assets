# Rev.2 阶段四（修订）交付报告 —— 浏览器 WSS 直连 EMQX 账号池

> 日期：2026-10-25 · 云仓 commit `e161179`（已推送 `origin/main`）· 固件仓零改动
> 上游任务：《Guo Feeder Rev.2 阶段四修订任务》（项目负责人批准「推翻维持 Worker 代理、实施浏览器直连」）
> 本报告 = 阶段 5 交付物。阶段 1（调查）/ 阶段 2（方案）结论已并入前序摘要与 `scripts/p4r_emqx_probe.py` 头注。

---

## 0. 一句话结论

**编码 + 测试全部完成并全绿（1125 + 33 + 23 断言，零 FAIL）；4 个只有真实连接/真实浏览器才抓得到的真 bug 已修并各附回归。生产部署未执行**（迁移 0010、`MQTT_POOL_KEY` Secret、Worker/Pages 上线、EMQX 控制台核对）——按任务书铁律「不自动部署生产」，留给项目负责人按 §5/§6 清单一次性授权执行。回退路径见 §7。

---

## 1. 交付物清单（A–J）

| # | 交付物 | 位置 | 状态 |
|---|---|---|---|
| A | 迁移 0010（两表，纯增量） | `guo-feeder-api/migrations/0010_mqtt_browser_pool.sql` | ✅ 已写未应用 |
| B | 账号池核心模块（~700 行） | `guo-feeder-api/src/mqtt-pool.js` | ✅ |
| C | cron 编排下沉（保 scheduled 薄入口护栏） | `guo-feeder-api/src/cron-jobs.js` + `index.js` 重构 | ✅ |
| D | Worker 端点：凭据/管理 3+1 | `index.js`（`GET /api/ui/mqtt/credentials`、`/api/admin/mqtt/pool/{status,bootstrap,rotate}`） | ✅ |
| E | Pages 凭据出口（会话+限流+转发） | `guo-feeder-pagesdev/functions/api/mqtt/credentials.js` | ✅（import bug 已修，见 §4） |
| F | 浏览器直连通道（~660 行） | `guo-feeder-pagesdev/assets/js/gf-mqtt.js` + `gf-app.js` 通道开关 | ✅ |
| G | vendored mqtt.js 5.16.1（MIT，无 CDN 无构建） | `guo-feeder-pagesdev/assets/vendor/mqtt.min.js` | ✅ |
| H | 测试 6 套件（§3 全绿） | `tests/api|pages|ui|e2e/` | ✅ |
| I | 实测探针脚本（阶段 1 遗产，零残留纪律） | `scripts/p4r_emqx_probe.py` / `p4r_emqx_acl_probe.py` | ✅ |
| J | 本报告 | 固件仓 `docs/review/Rev2-Stage4-Browser-MQTT-Direct-Connect-2026-10-25.md` | ✅ |

**产品决策落实对照**：账号池 5 个 `gf_web_NN`（可配 1..20）✅；Topic 从代码逐项提取（ACL 三条通配规则，`browserAclRules()`）✅；不暴露设备专用账号（GLOB 双边界 + G-1 fetch 审计）✅；不碰设备账号（P-11 + G-1 全程审计）✅；密码轮换 24h 可配 1..168h、改密主动踢连接（实测踢除 ~4s 内可观测）✅；轮换中账号不分配（rotating/failed 态 CAS 排斥）✅；Worker Secret + AES-GCM 密文（随机 nonce，错密钥/篡改 fail-closed）✅；不提交密码到 Git（`.gitignore` + 无明文落盘）✅；新增迁移纯增量 ✅。

---

## 2. 架构与数据流（最终形态）

```
浏览器 ──WSS(8084, /mqtt, mqtt.js v5)──▶ EMQX ◀──MQTT── 设备
   │                                        ▲
   │ GET /api/mqtt/credentials               │ 管理 API（建号/ACL/改密/踢连接）
   ▼                                        │
Pages Function（会话+限流+Bearer+x-ui-sid）──▶│
                                           Worker ──▶ D1 guofeeder-v2（池状态+AES-GCM 密文）
                                           Worker ──▶ D1（审计/历史，**直连路径不写** command_audit）
命令回退路径：gf.cmd_path=http（默认）⇒ POST /api/command ⇒ Pages ⇒ Worker ⇒ EMQX（留审计）
```

关键不变量（均有机器可检断言）：
- **帧对等**：浏览器 `buildDeviceFrame`/`stateFromFrame` 与 Worker 逐端口对等（55 断言，六命令族 × 压缩/未压缩/重启族/未知码）。
- **状态词表一致**：`done/accepted/running/failed/rejected/timeout` 与 HTTP 通道 `toUiState` 同源。
- **路径可分**：`status.source="mqtt"` 标识未经 Worker 审计；UI 通道选择器明示取舍（上屏文案）。
- **fail-closed 链**：池密钥缺失 ⇒ 凭据 503；会话失效 ⇒ 401；上游挂 ⇒ 502（且语义「未分配任何账号」）；会话存储不可用 ⇒ 503。

---

## 3. 测试结果（阶段 4 验收）

### 3.1 套件总览（本次全绿）

| 套件 | 断言 | 覆盖 | 结果 |
|---|---|---|---|
| `tests/api/mqtt_pool.test.mjs` | 68 | 池全生命周期（真实 SQLite + mock EMQX：命名边界/钳位/AES-GCM/ACL 形状/bootstrap/CAS 租约/轮换七步/失败路径/踢不落/设备账号零触碰/status 形状） | ✅ 68/0 |
| `tests/ui/mqtt_frame_parity.test.mjs` | 55 | Worker↔浏览器端口对等（帧构造 28 + 状态机 22 + 目录 registry_index + 无 DOM 可加载） | ✅ 55/0 |
| `tests/ui/mqtt_path_confirm.test.mjs` | 29 | 路径确认（零 `/api/command`；载荷/topic/qos；本地硬拦截；401；PUBACK 135 重试；SUBACK 0x80；重启 accepted 即返回） | ✅ 29/0 |
| `tests/pages/mqtt_credentials.test.mjs` | 30 | 凭据 Function（401 fail-closed/转发头/no-store/405/500/限流 429/502/503 透传/日志纪律/存储不可用 503） | ✅ 30/0 |
| `tests/e2e/mqtt_browser_e2e.mjs` | 33 | **真实 EMQX**（见 §3.2） | ✅ 33/0 |
| `tests/e2e/mqtt_browser_playwright.mjs` | 23 | **真实 chromium + 真实 UI**（见 §3.3） | ✅ 23/0 |
| 既有回归（16 中央套件 + api 仓 run_all） | 1125 / 25 套件 | 全仓回归，含 `retention_policy` 薄入口护栏 | ✅ / 唯一 FAIL 为既有已知 `s3_ingest_validate`（mock 502，git stash 实证与本次无关，文档记载「建议不修」） |

### 3.2 真实 EMQX e2e（`tests/e2e/mqtt_browser_e2e.mjs`，33/33）

对生产 EMQX（`n302933b.ala.cn-hangzhou.emqssl.cn`）执行，**零残留**（结束即删光所建池账号 + 踢光池连接 + 复核 0 条）：

- **C 组（连接）**：CONNACK rc=0（mqtt.js 5.16.1，与浏览器同库同版本）；SUBACK 订阅 `/up` `/log` 被授予；安全发布到**不存在的设备** `deadbeef0001`（真实设备零接收）PUBACK 正常；越权 subscribe 被拒（mqtt.js error 形态）；越权 publish ⇒ PUBACK rc=135；错密码 ⇒ CONNACK rc=134；相同 clientId 二次连接 ⇒ 旧连接被踢（close 可观测）。
- **D 组（轮换）**：`rotateAccount` 七步全过（真实改密 + ACL 重放 + 按 username 踢 + 验证断开 + 加密落库）；旧连接断开（close 旗标 + EMQX 零会话双证）；旧密码 rc=134 被拒；D1 密文解出的新密码 CONNACK rc=0；流水 `rotate_start→…→rotate_done` 七事件齐。
- **G 组（纪律）**：全程 fetch URL 审计只触碰 `gf_web_NN`（设备账号/保护名单零命中）；poolStatus 无密码/密文/nonce 字段；结束时 EMQX 侧 `gf_web_*` 账号与 `gfweb_*` 连接双双为 0。

### 3.3 真实浏览器 e2e（Playwright + chromium，23/23）

静态服务器 serve **产品文件**（app.html / gf-app.js / gf-mqtt.js / gf-ui-core.js / vendored mqtt.min.js），chromium 真实渲染；`/api/*` 仅路由夹具（auth/目录/凭据/命令代理），命令代理**记账**：

- **P-2**：启动期（默认 http）registry 确实经 `POST /api/command` —— 证明默认路径未变。
- **P-6 ★**：UI 切到 MQTT 后，**零新命令 POST `/api/command`**（GET 轮询属切前已提交命令，允许继续）—— 网络层路径确认，比 Node 桩测试更接近真实。
- **P-5/P-7**：连接参数（protocolVersion 5 / clean / 唯一 `gfweb_` clientId / URL 带 `/mqtt`）；发布 topic=`guo_feeder/<device>/down`、qos=1、载荷=`{c:"system",i,p:{o:"memory"}}`；订阅 `/up`；config_query 7 模块批量全走 MQTT。
- **P-8/P-10**：回帧驱动 UI（`#d-out` 显示结果）；通道 chip「已连接（WSS）」；全程零未捕获异常。

### 3.4 按任务书 §6.1 A–E 的逐项结论

| 项 | 结论 | 证据 |
|---|---|---|
| A 登录/会话 | **PASS**（Function 级 107 既有 + 浏览器层 P-1/P-2 启动链） | `pages_auth` 107/0；Playwright P-2 |
| B 凭据接口 | **PASS** | `mqtt_credentials` 30/0 + 真实 EMQX allocate（e2e E-2）+ 浏览器 P-4 |
| C 真实 WSS 连接 | **PASS** | e2e C-1..C-5（CONNACK/SUBACK/发布/越权拒绝/唯一 clientId） |
| D 轮换 | **PASS**（失败恢复 = mock 级 P-9/P-10 + 真实 D-2..D-6） | e2e D-2..D-6 + `mqtt_pool` P-8..P-10 |
| E 路径确认 | **PASS**（Node 网络桩 + 浏览器网络层双证） | `mqtt_path_confirm` 29/0 + Playwright P-6 |

---

## 4. 实测抓到的 4 个真 bug（本阶段最大价值）

| # | 现象 | 根因 | 修复 | 回归 |
|---|---|---|---|---|
| 1 | 浏览器直连必失败（ENOTFOUND） | `MQTT_WSS_URL`/DEFAULT 主机名拼错 `emqssl.cn`（l）≠ `emqxsl.cn`（x） | 两处同改 + 注释铁律 | e2e C-1 |
| 2 | WSS 握手 404 | URL 缺 `/mqtt` 路径（EMQX Serverless 只在该路径协商 MQTT 子协议；阶段 1 探针用的就是 `/mqtt`，知识在但没传到配置） | 加路径 + 两处注释互指 | e2e C-1 + Playwright P-5f |
| 3 | 每条直连命令白等 75s | `publishAndCollect` 的 `finished` 只被超时 resolve，终态回帧不提前返回 | 终态（done/failed/rejected）或重启族 accepted 即 resolve | `mqtt_path_confirm` E-3/E-9 |
| 4 | Pages 部署即失败（模块找不到） | `credentials.js` import `../_lib/` 少一级（应为 `../../_lib/`） | 已修 | `mqtt_credentials` 全 30（模块根本加载不了就会 FATAL） |

另修 1 个**测试侧**问题：`getCatalog()` 无条件写 `registry_index` 键（值为 undefined 时键仍存在）⇒ 改条件写键，使内存形状与线上 JSON 逐键一致（F-3c 由假红变真断言）。

---

## 5. EMQX 控制台手动核对清单（生产 bootstrap 后逐项打勾）

> 前置：先按 §6 部署 Worker（含 Secret）。bootstrap 也可用管理端点触发（见 §6.3）。
> 探针脚本 `scripts/p4r_emqx_probe.py` 可独立复核 ①②③（零残留纪律内置）。

1. **账号清单**：`Authentication → Users` 应见且仅见 `gf_web_00 .. gf_web_0N`（N = `MQTT_POOL_SIZE-1`，默认 5 个）；**无**其它新建账号；设备账号 `GuoFeederDevice` 原样未动。
2. **ACL**：任选一 `gf_web_NN` ⇒ Authorization 应恰有 3 条用户级规则：
   `allow publish guo_feeder/+/down`｜`allow subscribe guo_feeder/+/up`｜`allow subscribe guo_feeder/+/log`（无其它）。
3. **连接抽查**：浏览器切 MQTT 通道发一条查询 ⇒ `Clients` 应见 `gfweb_<12hex>`（username = 池账号，conn_state=connected）；命令执行后 60s 隐藏或切通道即断。
4. **轮换抽查**：24h 后（或管理端点手动触发）该账号密码应已变 ⇒ 旧浏览器连接被踢、旧密码拒连（rc=134）；`mqtt_browser_rotation` 流水七事件齐。
5. **保护名单**：全流程不得出现 `test001` / `shouji` / `workbuddy` / `dev_288485896ce4` 的 ACL 或密码变动。

---

## 6. Cloudflare 配置清单（生产上线步骤，待授权）

### 6.1 Secret（`wrangler secret put`，**不进仓库**）

```bash
cd guo-feeder-api
# 生成：node -e "console.log(require('crypto').randomBytes(32).toString('base64'))"
wrangler secret put MQTT_POOL_KEY        # AES-256-GCM：base64 32B 或 64hex；缺失 ⇒ 凭据 503 fail-closed
# EMQX_APP_ID / EMQX_APP_SECRET 已存在（设备 provision 同套，无需新增）
```

### 6.2 迁移（纯增量，可反复执行）

```bash
wrangler d1 migrations apply guofeeder-v2 --remote   # 应用 0010（IF NOT EXISTS，不碰 0001–0009）
```
（⚠️ `MQTT_POOL_KEY` 一旦写入生产**不可轻改**——换密钥 = 全部旧密文解不开，唯一恢复是 `bootstrapPool(force)` 重铺并踢全部池连接。轮换密码不换密钥，两者独立。）

### 6.3 部署顺序（建议一个窗口内完成，避免「URL 已下发但池未建」窗口）

```bash
# 1. 先建池（部署前或后均可，端点幂等）——部署 Worker 后：
curl -X POST https://guo-feeder-api.guobaby.workers.dev/api/admin/mqtt/pool/bootstrap \
  -H "Authorization: Bearer $ADMIN_API_TOKEN"
#   ⇒ {ok:true, accounts:[gf_web_00..04 created]}
# 2. Worker：wrangler deploy --config guo-feeder-api/wrangler.toml
#    （crons 已含 "5 * * * *" 池维护；vars 已含 MQTT_POOL_* / MQTT_WSS_URL）
# 3. Pages（铁律：必须在 guo-feeder-pagesdev/ 内以 . 为参数）：
cd guo-feeder-pagesdev && wrangler pages deploy . --project-name guo-feeder-pagesdev
#    判据：输出含 "Compiled Worker successfully" + "Uploading Functions bundle"
# 4. 对账：GET /api/admin/mqtt/pool/status（管理令牌）
```

### 6.4 运行参数（wrangler.toml `[vars]`，非密钥）

`MQTT_POOL_SIZE=5`（钳 1..20）· `MQTT_POOL_ROTATE_HOURS=24`（钳 1..168）· `MQTT_POOL_LEASE_MIN=30`（钳 5..240）· `MQTT_POOL_ROTATE=1`（逃生开关，=0 停 cron 池维护）· `MQTT_WSS_URL=wss://n302933b.ala.cn-hangzhou.emqssl.cn:8084/mqtt`（**已含实测修正**）。

---

## 7. 回退方案

| 层 | 回退目标 | 操作 | 影响 |
|---|---|---|---|
| Worker | `c6720d7f`（优化阶段 3 上线版） | Cloudflare 面板 Rollback 或 `wrangler rollback` | 凭据/管理端点消失；设备链路不受影响（本阶段零设备侧改动） |
| Pages | `897f151e`（阶段 2 上线版） | 面板 Rollback | UI 回到无通道选择器形态；`gf.cmd_path` 残留 localStorage 无害（gf-mqtt.js 不在旧包内，选择器不渲染） |
| D1 | 无需回退 | 0010 是纯增量（两新表），旧代码不读 | 池表留库无害；`mqtt_browser_rotation` 流水留证 |
| EMQX | 删池账号 | 控制台删 `gf_web_*` + 踢 `gfweb_*` 连接（或 `bootstrap` 逆操作脚本） | 直连不可用，HTTP 代理不受影响 |
| 密钥 | `MQTT_POOL_KEY` 删除 | `wrangler secret delete` | 凭据端点 503（fail-closed），UI 明示「凭据不可用」，可切回 HTTP 通道 |

**逃生开关（无需回退）**：`MQTT_POOL_ROTATE=0` 停轮换；UI 通道选择器随时切回 HTTP（默认即是 HTTP）。

---

## 8. 已知限制与风险（验收通过 ≠ 无代价）

1. **直连路径无云端审计**：命令不经 Worker ⇒ `command_audit` 无记录（`status.source="mqtt"` 明示）。这是产品负责人已批准的取舍；需要审计的场景应留在 HTTP 通道。
2. **信任面前移**：帧构造/状态机在浏览器包内（已用 55 条端口对等断言 + 真实浏览器测试缓解，但理论上包可被篡改——HTTPS + 同源部署是前提）。
3. **多标签页软上限**：per-tab 心跳注册表在池大小时拒绝新建连接（不踢已有）；registry 存 localStorage（清缓存即重拉）。
4. **隐藏 60s 断连**：省 broker 连接，代价是后台标签页的命令窗口可能被断（回前台惰性重连）。
5. **轮换窗口的体验代价**：改密踢连接是设计动作；被踢标签页下一条命令自动作废凭据重取重连（E-6 已验证重试不回退 HTTP）。
6. **e2e 网络环境依赖**：真实 EMQX e2e 需要沙箱/本机可达 `*.emqssl.cn`（本次实测可用；DNS 放行由沙箱策略动态决定，失败时脚本会明确报错而非假绿）。
7. **生产 D1 尚未有池行**：上线后首次 `bootstrap` 才建账号；此前凭据端点 503（fail-closed，UI 有明示文案）。

---

## 9. 待项目负责人决策/授权项（阻塞生产上线，不阻塞代码）

1. 授权执行 §6.1–6.3（Secret / 迁移 / 部署）——或明确「仅合码不上线」。
2. 确认轮换周期 24h 是否符合运营预期（可配 1..168h）。
3. 确认账号池大小 5 是否符合并发浏览规模（可配 1..20；每 GET 凭据占租约 30min）。
4. 生产 Turnstile（登录侧）是否同窗口启用（与本任务独立）。

---

## 10. 复现命令（验收用）

```bash
cd Cloudflare_Assets
node tests/api/mqtt_pool.test.mjs          # 68
node tests/ui/mqtt_frame_parity.test.mjs   # 55
node tests/ui/mqtt_path_confirm.test.mjs   # 29
node tests/pages/mqtt_credentials.test.mjs # 30
node tests/e2e/mqtt_browser_e2e.mjs        # 33（需 EMQX 可达；零残留）
node tests/e2e/mqtt_browser_playwright.mjs # 23（chromium 已装）
node guo-feeder-api/test/run_all.mjs       # 25/26（唯一 FAIL = 既有已知 s3_ingest_validate）
```
