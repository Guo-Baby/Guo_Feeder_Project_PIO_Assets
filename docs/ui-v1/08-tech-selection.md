# 技术选型（前端）

> 本轮**只选型**：不引入依赖、不改构建配置、不开始编码。

---

## 1. 现状约束（决定了选型的边界）

| 约束 | 事实 |
|---|---|
| 构建步骤 | **没有**。`web:dev`/`web:deploy` 直接指向静态目录 |
| 依赖 | **零依赖**（无 `package.json`、无 CDN） |
| 部署 | `wrangler pages deploy guo-feeder-pagesdev` |
| 认证 | 服务端 HMAC cookie + Turnstile（`login.html` 内联脚本） |
| 同源代理 | `/api/read/*`、`/api/command`，浏览器只见 cookie |
| 目标 | 手机优先；未来 Android App（WebView 或原生）复用 |
| 团队规模 | 单人 + AI 协作，长期维护成本比"上手速度"更重要 |

---

## 2. 候选方案对比

评分：★ 越多越好（★=1，☆=0.5）

| 维度 | A. 原生 ES Modules（自研轻量组件） | B. Preact + htm（ESM/CDN，无构建） | C. Vite + React/Svelte（有构建） |
|---|---|---|---|
| 首屏体积 | **★★★** ~15–25 KB（无框架） | ★★☆ 框架 ~5 KB + 应用 ~20 KB | ★★ 通常 60–120 KB |
| 构建/部署改动 | **★★★** 零改动 | ★★★ 零改动（但依赖 CDN 运行时） | ★ 要加 build 步骤 + 产物目录 |
| 供应链风险 | **★★★** 无外部依赖 | ★☆ CDN 可用性/版本漂移 | ★★ 锁文件可控但依赖树大 |
| 离线/PWA | **★★★** 全本地缓存 | ★★ 需缓存第三方 ESM | ★★★ 天然打包 |
| 拖动排序 | ★☆ 需自研（~120 行 Pointer Events） | ★★ 同样需自研（框架不提供） | ★★☆ 生态有库，但要再引依赖 |
| 参数表单（动态 schema） | ★★ 自研 ~150 行（模式简单：8 种控件） | ★★★ 组件化更顺手 | ★★★ 最顺手 |
| 复杂状态机（9 态命令） | ★★ 需自研 store（~80 行） | ★★★ 组件 + hooks 自然 | ★★★ 生态成熟 |
| 可测试性 | ★★☆ 纯函数易单测；DOM 用 `node --test` + 轻量 stub | ★★ 需 DOM 环境 | ★★★ 生态成熟 |
| 长期可维护（单人） | **★★★** 无版本升级负担 | ★★ 框架版本升级 | ★☆ 构建链会腐化 |
| Android WebView 复用 | **★★★** 静态资源直接塞 assets | ★★★ 同 | ★★ 需打包产物 + 路由适配 |
| 与现有登录/Turnstile 兼容 | ★★★ | ★★★ | ★★★ |

---

## 3. 推荐：**A. 原生 ES Modules + 自研极简组件层**

### 3.1 理由（按权重）

1. **复杂度真的不高**。整个 UI 是「4 个列表页 + 1 个编辑器 + 1 个详情」，
   没有虚拟列表、没有复杂动画、没有多路由嵌套。
   编辑器本质是"数组增删改 + 按 schema 渲染 8 种控件"——**不需要框架**。
2. **零构建 = 零腐化**。这个项目已经把"可复现"当成纪律（`.bin`+sha256 归档、
   冻结契约、三源交叉）。引入 Vite 会带来 `node_modules`、锁文件、构建产物版本漂移，
   与现有工程习惯冲突。
3. **供应链**：CDN 方案（B）在"离线可用 + 长期可维护"上是最差的组合
   —— 既没有构建的确定性，又引入了外部运行期依赖。
4. **体积直接决定手机体验**：目标场景是"掏手机看一眼、点一下关阀"，
   冷启动 200 ms 与 800 ms 的差别，比开发体验重要。
5. **WebView 复用最省事**：`app/` 静态目录整个塞进 Android assets 即可，
   连 origin 都能对齐（便于 cookie 和同源策略）。

### 3.2 目录组织（保持在无构建前提下可维护）

```
guo-feeder-pagesdev/
  app.html                 ← 唯一 shell（head/nav/挂载点）
  assets/
    css/tokens.css         ← 设计变量（现有 :root 变量升级）
    css/app.css            ← 布局与组件样式
    js/app.js              ← 入口：路由 + 挂载
    js/store.js            ← 极简订阅式状态（命令生命周期、设备上下文）
    js/api.js              ← 只读代理 + command 封装（含 401/429 处理）
    js/router.js           ← hash 路由
    js/ui.js               ← 组件：card/kpi/chip/sheet/dialog/toast
    js/screens/home.js  control.js  workflows.js  editor.js  records.js  device.js
    js/catalog.js          ← 元数据目录（action/trigger schema）
    js/dnd.js              ← Pointer Events 拖动排序（可选，若启用拖拽）
  sw.js                    ← PWA（M4）
  manifest.webmanifest
```

约束：
- 所有模块用 **ESM `import`**，浏览器原生加载（HTTP/2 多路复用，无需打包）
- **禁止**运行时 CDN
- CSS 用**原生变量 + 少量工具类**，不引 Tailwind（避免构建）

### 3.3 自研的最小件（工作量与边界）

| 件 | 行数量级 | 说明 |
|---|---|---|
| `store.js` | ~80 | `{get, set, subscribe}` + 命令生命周期 Map（按 `cmd_id`） |
| `ui.js` | ~150 | `el(tag, props, children)` + 4 个组件 |
| `catalog.js` | 数据为主 | 参数 schema（见 `04` §2.4） |
| `dnd.js` | ~120 | **可选**：Pointer Events 实现；不启用时用"上移/下移"按钮（已设计） |
| 表单渲染 | ~150 | 8 种控件 + 校验 |

**测试**：`store.js` / `catalog.js` 的校验函数 / 解码结果的渲染映射
都可以用 `node --test` 直接单测（纯函数），与现有 `tests/pages/*.mjs` 风格一致。

---

## 4. 备选方案与放弃理由

| 方案 | 何时才该选它 |
|---|---|
| **B. Preact + htm（CDN）** | 如果 UI 需要 3 个以上复杂可复用组件（例如富文本、图表交互），且能接受离线缓存第三方 ESM。**当前不需要** |
| **C. Vite + React/Svelte** | 如果团队 > 2 人、或要长期迭代到"多设备 + 多用户 + 实时推送"。**可在 App 阶段重估** |
| **D. 保持单文件 HTML** | 只适用于"只读看板"这种规模。编辑器一旦进来，单文件会超过 2000 行，**明确不采用** |

★ 特别说明（回应需求原文）：**"不要因为 `app.html` 很小就继续堆单文件"** ——
这一点已采纳：应用代码全部拆到 `assets/js/`，`app.html` 只保留 shell。

---

## 5. 图表方案

三条曲线（温度/湿度/重量）+ 可选累计量。

| 选项 | 评价 |
|---|---|
| **自研 SVG 折线（推荐）** | ~120 行；完全可控（断点、空档、降采样）；零依赖；体积 0 |
| Chart.js（CDN/本地） | 40 KB+，功能过剩；断点/空档表现需额外配置 |
| Canvas 自绘 | 适合大数据量；本项目 24h 只有 ~340 点，**不需要** |

**降采样策略**：`telemetry` 端点支持 `from/to`，前端按时间窗聚合到 **≤ 300 点**
（取每桶均值 + 标记 max/min 极值点），避免"点多到看不清"。

---

## 6. PWA 与未来 App

| 项 | 设计 |
|---|---|
| `manifest.webmanifest` | `display: standalone`、`theme_color`、图标 192/512 |
| `sw.js` | **只缓存静态资源**（`assets/**`、shell）；**绝不缓存 `/api/**`**（数据必须新鲜） |
| 离线页 | 显示"离线，最后同步 xx 前"，控制按钮禁用 |
| Android 复用 | 两种路径：① WebView 包静态资源（最快）；② 原生 + 调同一套 Worker 接口（体验最好，但要重写 UI）。**M4 之后再决定** |

⚠️ Service Worker 与现有会话 cookie 无冲突（cookie 由浏览器管理，SW 只代理 fetch）。

---

## 7. 选型结论（一句话）

**采用 A：原生 ES Modules + 自研极简组件 + 自研 SVG 图表；保持零构建、零运行时依赖。**
当且仅当出现"多视图实时推送"或"团队扩到多人"时，再重估是否引入构建链。
