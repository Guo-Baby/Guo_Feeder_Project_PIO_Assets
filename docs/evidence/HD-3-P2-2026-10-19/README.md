# 原始证据归档 · `P-2` 生产部署与真机 E2E（2026-10-19）

> **用途**：`P-2` 关闭的**原始证据**（非摘要）。所有文件均为 2026-10-19 当晚执行时的**原样输出**，
> 未编辑、未重排。摘要与结论见 `docs/architecture/HD-3-P2-Production-Deployment-and-E2E-Acceptance-Report.md`。
>
> **为何归档**：原始文件原位于 `Cloudflare_Assets/.wrangler/`（**gitignore**，换机/清理即丢失）。
> 本目录是它们的唯一持久副本。

## 文件清单

| 文件 | 内容 | 采集方式 |
|---|---|---|
| `01-deployments-before.json` | 部署**前**最近 10 条 deployment（末条 = `c28f2eba` / version `f9647a00`） | `wrangler deployments list --json` |
| `02-deployments-after.json` | 部署**后**最近 10 条（末条 = `c666aeeb` / version `826332ed`） | 同上 |
| `03-device-subscription.json` | 设备 `dev_288485896ce4` 的订阅：`guo_feeder/288485896ce4/down` · QoS 1 | EMQX `GET /clients/{cid}/subscriptions` |
| `04-cmd1-p21792414675-rows.json` | 命令 1 的 5 条 `mqtt_messages` 行（down + tenant 副本 + ack + s=1 + s=0） | `wrangler d1 execute --remote`（只读 `SELECT`） |
| `05-cmd2-p2b1792414735-rows.json` | 命令 2 的 5 条同类行（可重复性） | 同上 |
| `06-wrangler-tail-failure.txt` | `wrangler tail` 的 502 失败原文（取证通道不可用的证据） | `wrangler tail --format=json` |
| `07-cmd1-id.txt` / `08-cmd2-id.txt` | 两轮命令 ID 原文 | shell 生成 |

## 采集时的命令（可复核）

```bash
# 部署前后版本
wrangler deployments list --json
wrangler versions list --json

# 设备订阅
curl -sS -u "$EMQX_APP_ID:$EMQX_APP_SECRET" \
  "https://<host>:8443/api/v5/clients/dev_288485896ce4/subscriptions"

# 报文证据
wrangler d1 execute guofeeder --remote \
  --command "SELECT id,topic,direction,cmd,status,error_code,created_at,raw_payload
             FROM mqtt_messages WHERE msg_id='<id>' ORDER BY id;"
```

## 完整性说明（诚实标注）

- `04` / `05` 是 D1 `--json` 的**直接输出**，未裁剪字段。
- `06` 证明 **`[cloud_api] published ...` 日志行未能取得**（代理隧道 502）——
  该条在验收报告中标为 `NOT VERIFIED`，结论靠 `04/05` 的下行行 + 设备 ACK + EMQX 计数三者支撑。
- 归档文件**不含**任何令牌、密码或私钥；`01/02` 中的 `secret` 字样来自 commit message
  `"N-3 secret redaction (d604f2e)"`，非凭据。
