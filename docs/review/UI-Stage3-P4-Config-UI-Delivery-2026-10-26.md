# Guo Feeder · 阶段三（P4 · 配置 UI 化）交付报告 · 2026-10-26

> 阶段：阶段三 = 原计划 P4（配置规格 + 读侧 UI + 写侧 + 失败演练指引）
> 状态：**代码完成、测试全绿、未部署**（任务书明确：阶段三生产部署不含在阶段二授权内）
> 唯一进度表：`docs/review/UI-V11-Gap-Audit-and-Implementation-Plan-2026-10-25.md` §8
> 配置规格表（阶段三产物）：`docs/review/Config-Spec-Table-2026-10-25.md`

---

## 一、阶段二部署与生产验证（本轮先完成的部分）

### 1.1 部署前实际状态

| 仓 | 分支 | HEAD | 工作区 |
|---|---|---|---|
| 云 `D:\Guo_Feeder_Project\Cloudflare_Assets` | `main` | `3128860` | 干净 |
| 文档 `D:\Guo_Feeder_Project\PIO_Assets\Guo_Feeder_Project` | `wb` | `91ba225` | 有历史未提交文件（**非本轮产生**，未纳入提交） |

部署差异核查：`git diff --stat 24f87f2 3128860` ⇒ **仅阶段二 4 个文件**
（Worker 1 + Pages 3），**零 migration、零 wrangler 绑定变更、零 EMQX 变更**。

### 1.2 实际部署版本

| 组件 | 版本 | 回滚目标 | 备注 |
|---|---|---|---|
| Worker | `a5a030a7` | `6bf731d4` | 双 cron 保留 |
| Pages | `004a2538` → **重部署 `0d960fb4`** | `497f4672` | 第二次含风险列修正 |

> 重部署原因：生产验收发现**真实缺陷 X8**（审计风险列用了虚构枚举）⇒ 修正后重新部署 Pages。

### 1.3 生产验证结果

| 项 | 结果 | 工具 |
|---|---|---|
| 阶段一生产回归 | **20/20 PASS** | `scripts/verify_stage1_prod.mjs` |
| 阶段二生产验收 | **35/35 PASS** | `scripts/verify_stage2_prod.mjs` |
| 产物指纹 | 生产 `gf-app.js` / `gf-ui-core.js` 与本地 `git hash-object` **完全一致** | 验收脚本内比对 |
| 鉴权未削弱 | 无令牌 401 / Pages 代理 401 / 未授权 admin 拒 / 带新参数仍 401 | 验收脚本 |
| R1 会话级页面 | ✅ **用户已人工验收**（登录 →「记录 → 审计」表格正常、动作翻译符合预期） | 人工 |

> ★ 阶段一验收脚本自身有两处**方法错误**，已修并记录：
> ① `git hash-object --stdin` + `encoding:"utf8"` 会经字符串中转 ⇒ 与 `git hash-object <file>` 哈希不同，
> 造成"部署成功却报不一致"的**假红**；
> ② 首版把 risk 真实值集合写成 `low/medium/high`（同样是**虚构枚举**）⇒ 真实数据里的 `read` 把它顶红。

---

## 二、阶段三实施内容（P4）

### 2.1 P4-0 · 配置规格表 ✅

**权威数据源**：`guo-feeder-api/src/config-spec.js`（66 键，10 模块）
**对齐测试**：`tests/api/config_spec_parity.test.mjs`（30 断言，**机械解析固件源码**）

| 分类 | 数量 | 说明 |
|---|---|---|
| 键总数 | 66 | 10 模块 |
| 可改 | **59** | D-6 一次性全开，不分批 |
| 死参数 | **7** | 写了对设备**没有任何效果** ⇒ 服务端 400 + UI 禁用并给原因 |
| 敏感 | 3 | `wifi.password` / `mqtt.password` / `mi_thermo.BLE_key` |
| 固件真钳位 | **9** | 只有这 9 个能在 `min/max` 写范围 |
| 固件不校验 | 57 | 只能给 UI 建议区间（`suggest_min/max`） |

死参数（写了对设备没有任何效果，UI 必须拒绝）：

| 键 | 原因 |
|---|---|
| `mqtt.retry_interval` | `cloud_manager.cpp` 的 `static retry_interval` **从未被赋值** |
| `mqtt.retry_max` | `:2437` 用 `(retry_max > 0)` 判断，但变量从未赋值 ⇒ 恒用 `MQTT_RETRY_MAX` |
| `mqtt.sleep_retry_interval` | 变量从未赋值（调用点 0） |
| `mqtt.mqtt_keep_alive` | `keep_alive` 从未赋值 ⇒ `cfg.keepalive` 恒 0 |
| `valve.open_duration_ms` | 全仓无消费者（预留字段） |
| `weight.sample_interval` | `weight.cpp` 不读（采样周期由 loop 节拍决定） |
| `weight.filter_samples` | 滤波窗口用编译期 `WEIGHT_FILTER_WINDOW` |

### 2.2 P4-1 · 读侧 UI ✅

- **设备页**：10 个模块卡片 → 模块详情页 `#config/<module>`；
- **Config 分类页** `#config`：按用途分组（网络/执行器/外设/系统）；
- **一次全量读**：取代旧的 **7 条串行** `config_query`（每条一次 3 s 往返），
  现在 **1 条**（`module` 为空 ⇒ 固件 `exec_query_all` 回 `{data, versions}`）；
- **元数据只来自云端目录**（前端不复制第二份键表，复制必然漂移）；
- **三态标注**：设备当前值 / 本地草稿 / 已写入内存（未落盘）/ 已持久化；
- **敏感值掩码**：显示首尾各 2 位，**目录里不下发任何出厂凭据**（X9）。

### 2.3 P4-2 · 写侧（RAM → 落盘 → 重启）✅（代码链路）

**白名单只有 3 条**（`config_delete`/`config_reset`/`config_backup`/`config_module`
**故意不在其中** —— 能一次毁掉整模块配置）：

| 命令 | 作用 | 落盘 | 重启 |
|---|---|---|---|
| `system.config_set` | 写单键（仅 RAM）+ `expect_version` 乐观锁 | 否 | 否 |
| `system.config_save` | dirty 模块写 Flash | 是 | 启动 5 分钟自动重启计时 |
| `system.config_restart` | 先落盘再请求重启（`cancel:true` 只取消计时） | 是 | 是 |

**服务端校验链**（`validateConfigParams`）：未知模块 / 未知键 / 死参数 / 类型 / 范围 / 枚举 /
模板占位符 / 版本号范围 —— 任一不过 ⇒ **400，根本不下发到设备**。
（固件侧虽有钳位，但它是**静默**的 ⇒ 必须服务端先拦。）

**乐观锁**：固件 `config_set` 支持 `expect_version`（`config_manager.cpp:2394`），
版本不符 ⇒ 设备拒绝并回 `current_version`（`:2400`）。
UI 写入时带上**读取时的模块版本**，冲突时提示"模块已被修改，请重新读取后再改"并失效缓存。

**三段式交互**：编辑（仅 RAM）→ 落盘（回显 `saved`）→ 立即重启 / 取消自动重启。
**倒计时**：固件 `config_restart_pending()` **无云端命令入口** ⇒ 读不到真实剩余时间；
按 D-7 只显示**前端按本地落盘时刻的估算**，并明示"这不是设备端真实剩余时间，倒计时结束不代表已重启"。

### 2.4 P4-3 · 失败演练与回滚指引 🟡（部分，需真机）

| 场景 | 状态 | 处置 |
|---|---|---|
| 落盘失败（Flash IO 失败） | ✅ 代码路径已判（`s≠0` ⇒ 提示"配置可能仍只在内存，请勿断电"） | **演练未做** |
| 重启失败 / 设备不在线 | ✅ 已有重启六段流程（阶段二 P3-6），`boot_seq` 才算完成 | **演练未做** |
| 非计划重启回滚 | ✅ **回滚提示已实现**：读日志 `0x0102 LOG_SYS_BOOT_INCOMPLETE_PREV`（CRITICAL）⇒ 显示"配置可能被回滚，请复核" | **演练未做** |
| `commit` 卡 PENDING | ✅ 固件启动 recover 路径已存在（`config_manager.cpp:941-958`） | UI 无主动查询入口（无云端命令） |
| 5 分钟窗口内连续修改 | ✅ 固件从最后一次起算（`config_manager.h:123-127`）；UI 用本地落盘时刻估算 | **演练未做** |

> ★ **演练未做的原因**：四种场景都需要**向真机写配置并制造故障**，
> 本轮任务书禁止向生产设备发命令 / 做危险硬件操作 ⇒ 演练未做。
> 这正是**开放写配置的硬前置**（见 §三 门禁）。

### 2.5 R7 · 模块「启用该设备」✅

设备页顶部给「模块启用开关」区：**只有规格表里真实存在 `enable` 键的 5 个模块**
（valve / weight / rtc / stepper / vibration）有开关；
oled / wifi / mqtt / time / mi_thermo **无 enable 键 ⇒ 绝不给开关**（D-6：不得伪装）。
写的就是该模块的 `enable` 键（`config_set`），关闭后其动作**从能力表消失**（需重启生效）。

---

## 三、门禁：写配置当前**默认关闭**

```js
// capability-catalog.js
export const CONFIG_WRITE_ENABLED_BY_DEFAULT = false;
```

| 项 | 值 |
|---|---|
| 目录暴露的开关 | `config_write.enabled = false` |
| 门禁原因文案 | "P4-3 失败演练（落盘失败/重启失败/非计划重启/commit 卡 PENDING）未在真机执行" |
| UI 表现 | 配置页显式提示"⚠ 写配置未开放：<原因>"，只读展示，**不做成"点了没反应"** |

> ★ 代码链路**已实现且有测试**，是否对用户开放由**这一处开关**决定（不散落）。
> 演练通过后把它改成 `true` 即可。

---

## 四、本轮抓到并修的缺陷（10 个）

| 编号 | 缺陷 | 发现方式 | 处置 |
|---|---|---|---|
| X9 | 目录把敏感项的**出厂值**（WiFi/MQTT 密码、BLE key）下发给浏览器 | 阶段三回归 | `factory/fallback` 置 null，只带 `masked` |
| X10 | `runCommand` 只对 query/wfadmin/system 取回帧 ⇒ **config 族拿不到设备回帧**，无法区分"已写入"与"设备拒绝" | 浏览器测试 S3-8a | 补 config 族取帧 + 严格判定（**无回帧 / accepted 都不算成功**） |
| X11 | 规格表把「UI 建议范围」当成「固件钳位」 | 对齐测试 | 拆成 `min/max`（固件事实）与 `suggest_min/max`（UI 提示） |
| X12 | 规格表模块顺序 ≠ 固件 `kModuleNames` 顺序（`config_query` 全量返回依赖后者） | 对齐测试 P4-0-7a2 | 数组顺序对齐固件；UI 顺序另用 `uiOrder` |
| X13 | 配置回帧解包读 `frame.data`（真实在 `frame.o`）⇒ 永远拿不到配置 | 浏览器测试 S3-2c | 先解 `o` 再取 `data/versions` |
| X14 | `runCommand` 漏 `config` 分支 ⇒ 请求体没有 `action_id`，命令根本没发出 | 浏览器测试 S3-7a | 补分支 + 客户端白名单 |
| — | 规格表枚举键被误删 `enum` 字段 | 自检 | 恢复并加断言 |
| — | `validateConfigValue` 枚举判断在范围判断之后 ⇒ 错误信息不具体 | 自检 | 枚举优先 |
| — | 替换脚本把 `frames` 块误插进 3 个写配置函数（覆盖判定逻辑 ⇒ `family is not defined`） | 桩测试 T-6 | 重写三函数 + 恢复 `let frames` |
| — | 测试桩缺 `/api/command/:cmd_id` ⇒ 轮询空转到 75 s | 浏览器测试超时 | 补桩（真实 Worker 有此端点） |

> ★ X10 是**最关键**的一个：旧写法 `if (st !== null && st !== 0)` 在**没有回帧**时 `st=null`
> ⇒ **误判成功**。已改为「无回帧 ⇒ 结果未知」「accepted ⇒ 只入队不算成功」。

---

## 五、测试结果（分层，不得合并）

### 5.1 单元 / 契约测试

| 套件 | 结果 | 新增 |
|---|---|---|
| `config_spec_parity` | **30 PASS / 0 FAIL** | ✅（机械解析固件源码校对规格表） |
| `config_write_command` | **49 PASS / 0 FAIL** | ✅（白名单/死参数/范围/乐观锁/风险/审计） |
| `ui_stage3_config` | **46 PASS / 0 FAIL** | ✅（掩码/三态/门禁/倒计时/回滚/输入校验） |
| `ui_security` | **275 PASS / 0 FAIL** | 强化 D8-1（禁值 / 禁默认值 / 禁控制面凭据词） |
| `ui_app` | **196 PASS / 0 FAIL** | 更新 + 新增 S-10h2/S-10h3 |
| `ui_app_runtime` | **131 PASS / 0 FAIL** | 更新（配置入口/弹层/启用开关） |

### 5.2 API / 集成测试

| 套件 | 结果 |
|---|---|
| **中央 26 套件合计** | **930 PASS / 0 FAIL** |
| api 仓 `test/run_all.mjs` | 25/26 —— 唯一失败是**既有** `S3-10-1b`（EMQX mock 缺 CONFIG，与 config 写通道无关；未删断言、未放宽） |

### 5.3 真实 Chromium 浏览器测试

| 套件 | 结果 |
|---|---|
| `stage3_config_playwright`（新） | **30 PASS / 0 FAIL** |
| `stage1_ui_playwright` | 47 PASS / 0 FAIL |
| `stage2_ui_playwright` | 38 PASS / 0 FAIL |
| `stage5_ui_playwright` | 22 PASS / 0 FAIL |
| `mqtt_browser_playwright` | 27 PASS / 0 FAIL |
| **合计** | **164 PASS / 0 FAIL** |

### 5.4 生产只读验证

| 项 | 状态 |
|---|---|
| 阶段一生产回归 | ✅ 20/20（阶段二部署后复跑） |
| 阶段二生产验收 | ✅ 35/35 |
| **阶段三生产验证** | ❌ **未执行**（代码未部署） |

### 5.5 真机验证

| 项 | 状态 | 原因 |
|---|---|---|
| 配置写入（RAM/落盘/重启） | ❌ **未验证** | 本轮禁止向生产设备写配置 |
| P4-3 失败演练（4 场景） | ❌ **未验证** | 需真机制造故障 |
| 步进 / 振动 | ❌ 未验证 | DRV8825 硬件损坏 |
| HX711 称重 | ❌ 未验证 | 传感器未接 |

---

## 六、变更文件清单

### 云仓 `Cloudflare_Assets`

| 文件 | 变更 |
|---|---|
| `guo-feeder-api/src/config-spec.js` | **新增** · 66 键规格表（权威数据源）+ `validateConfigValue` + `SPEC_DRIFT` |
| `guo-feeder-api/src/capability-catalog.js` | 目录版本 `1.1.0→1.2.0`；新增 `config_modules`/`config_keys`/`config_drift`/`config_commands`/`config_write`；新增 `CONFIG_WRITE_COMMANDS`（3 条）+ `buildConfigFrame` + `validateConfigParams`；敏感项不下发默认值 |
| `guo-feeder-api/src/command-api.js` | 新增 `config` 族（独立白名单 + 服务端校验 + 风险逐键判定） |
| `guo-feeder-pagesdev/app.html` | 配置页容器 ×2 + `#cfgbar`（待落盘条）+ `#cfgrollback`（回滚提示条）+ 样式 |
| `guo-feeder-pagesdev/assets/js/gf-app.js` | 配置页面（总览/模块详情/单键弹层）、`loadConfigAll` 一次全量、`writeConfigKey`/`saveConfigToFlash`/`restartAfterConfig`/`cancelAutoRestart`、乐观锁、启用开关、回滚提示、路由 `#config` `#config/<module>` |
| `guo-feeder-pagesdev/assets/js/gf-ui-core.js` | 配置纯逻辑层：值格式化 / 三态 / 范围文本 / 写门禁 / 倒计时 / 回滚检测 / 模块卡片 / 前端输入校验 |
| `guo-feeder-pagesdev/assets/js/gf-mqtt.js` | MQTT 直连通道的 config 帧构造（与 Worker 同形）+ 客户端白名单 + `expect_version` |
| `tests/api/config_spec_parity.test.mjs` | **新增** · 30 断言 |
| `tests/api/config_write_command.test.mjs` | **新增** · 49 断言 |
| `tests/ui/ui_stage3_config.test.mjs` | **新增** · 46 断言 |
| `tests/e2e/stage3_config_playwright.mjs` | **新增** · 30 断言（真实 Chromium） |
| `tests/api/ui_security.test.mjs` | 强化 D8-1 三条 |
| `tests/pages/ui_app.test.mjs` / `ui_app_runtime.test.mjs` | 更新到新契约 + 回归锁 |
| `tests/e2e/mqtt_browser_playwright.mjs` | P-9 更新为「一次全量」新口径 |

### 文档仓 `PIO_Assets/Guo_Feeder_Project`

| 文件 | 变更 |
|---|---|
| `docs/review/Config-Spec-Table-2026-10-25.md` | **新增** · 配置规格表（P4-0 产物） |
| `docs/review/UI-Stage3-P4-Config-UI-Delivery-2026-10-26.md` | **新增** · 本报告 |
| `docs/review/UI-V11-Gap-Audit-and-Implementation-Plan-2026-10-25.md` | §8 进度表更新到阶段三 + X8–X14 + 部署记录 |

---

## 七、明确声明

1. **阶段三代码未部署**（任务书：阶段三生产部署不含在阶段二授权内）。
   ⇒ R7 / R11 / R12 / R13 / R14 / R15 / R30 的生产状态一律"**未部署**"。
2. **写配置在生产是只读**：即便部署，`config_write.enabled = false`（P4-3 演练未做）。
3. **阶段四（多设备）尚未开始**：本阶段只做了异步隔离，未做设备列表/切换/跨端别名。
4. **阶段五（P6 EMQX 生命周期）**只剩文档收口，未动策略（维持固定池，D-8）。
5. 本轮**未执行**：D1 migration / 修改生产 EMQX / 向生产设备发命令 / 硬件危险操作。
6. **真机验证全部为空**（配置写入、失败演练、步进、振动、称重）。
