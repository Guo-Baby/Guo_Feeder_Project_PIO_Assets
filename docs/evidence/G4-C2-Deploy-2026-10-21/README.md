# 证据目录 · G4 C-2（Worker 部署与生产行为验证）

- 执行窗口：`2026-10-21 01:34–01:42 +0800`
- 目标：Worker `guo-feeder-api`（账号 `9e72d201…`）· D1 `guofeeder` = `0d8385ba-a1a6-4fcc-8e43-ae61c014c43d`
- 源码：`Cloudflare_Assets` HEAD `e1e8b79`，工作区干净（`worktree_dirty=0`）
- 正式报告：`docs/architecture/G4-C2-Deployment-and-Acceptance-Report-2026-10-21.md`

## 版本对照

| 项 | 值 |
|---|---|
| 部署前 version | `826332ed-3a73-42c4-a090-a5bf737dc210`（v47） |
| 部署前 deployment | `c666aeeb-150e-4b10-a155-59a13bdaab0b` |
| **部署后 version** | **`d7fcd4f3-c9f3-4a7c-ba2f-c7e3bce74016`**（100%） |
| **部署后 deployment** | `Created: 2026-10-20T17:39:04.490Z`（`deployments status`） |
| 回滚目标 version（v46） | `f9647a00-e079-4de9-bcf1-8c9bc5344185` |
| 回滚目标 deployment | `c28f2eba-2f5e-49c0-8205-206b15d9858e` |

## 文件顺序

| 文件 | 阶段 | 内容 |
|---|---|---|
| `c2_pre.sh` | 基线 | 可复现脚本（只读） |
| `c2-pre-baseline.txt` | 基线 | 源码 commit + 4 文件 sha256、表规模、方向分布、归属分布、device、log_history |
| `c2-pre-traffic.txt` | 基线 | **近 30 min 流量 = 0**、最新行距今 3275 s |
| `c2-pre-endpoint.txt` | 基线 | **`workers.dev` DNS 被污染不可达**；Pages 直连 200、`/api/webhook` = `MQTT Router OK` |
| `c2-pre-keycheck.txt` | 基线 | 密钥一致性探测（因网络不可达返回 000，未得出结论） |
| `c2-deploy.txt` | 部署 | 命令、起止时间、完整输出、`deploy_exit=0`、新 version |
| `c2-post-version.txt` | 部署 | `deployments status` 100% + 新 version 的 D1 绑定 |
| `c2_probe.py` | 探针 | 可复现探针脚本（含 UA 说明，不含密钥） |
| `c2-probe-run.txt` | 探针 | **首次尝试：403 error 1010（Cloudflare 客户端签名拦截，零写入）** |
| `c2-probe-run-2.txt` | 探针 | 加浏览器 UA 后：8 次请求全部 200 `OK` |
| `c2-probe-verify.txt` | 验证 | 探针 6 行逐行 + 按标记汇总 |
| `c2-dedup-repress.txt` | 验证 | P-4 再发 2 次同 payload：仍 200，行数不变 |
| `c2-invariants.txt` | 验证 | P-4 行数、`I-1`、`I-2`、`V-1` 复检、规模与 id 缺口、device 表、log_history |
| `c2-post-traffic.txt` | 验证 | 方向分布（`log` 首次出现）、`$tenants` 归属、部署后新增行、**真实流量 = 0** |
| `c2-observability.txt` | 观测 | `wrangler tail` 8 秒无输出 ⇒ `INCONCLUSIVE`（本报告不依赖它） |

## 三条必须随结论引用的限制

1. **网络**：本沙箱 `workers.dev` DNS 解析到 `202.160.128.40`（污染地址），直连超时、代理 502。
   探针只能经 Pages 中转 `/api/webhook` → Worker **根路径**；该路径与 `/api/ingest`
   共用同一写入逻辑，差别仅 `/api/ingest` 额外拒绝 `source=cloud_api`。
2. **样本**：真实设备自 `01:20` 起静默（部署后近 10 min 真实写入 **0 行**）。
   `P-1'/P-1''/P-3'` 的**真实流量**验证因此 `NOT RUN`；已用带 `G4PROBE-` 标记的**合成**样本
   （`P-1'-SYN`/`P-3'-SYN`）直接验证同一代码路径，二者**不得互相替换**。
3. **安全观察（既存，非本次引入）**：Pages `functions/api/webhook.js` 转发到 Worker 根路径时
   **不带 `X-Webhook-Key`**，而 Worker 根路径的鉴权块是被注释掉的 ⇒ 该入口对公网**无鉴权即可写
   `mqtt_messages`**。本次探针正是走了这条路（`G4PROBE-` 标记可全部识别）。
   已登记为 `C2-SEC-WEBHOOK-NOAUTH`，**不属于 C-2 授权范围**，未修改。
