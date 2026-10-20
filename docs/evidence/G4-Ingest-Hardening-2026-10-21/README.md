# 入站校验加固 · 影子校验证据（`2026-10-21 02:49`）

**目的**：在写任何代码之前，用**只读 SQL** 量化「若强制入站校验器，会拒掉多少历史行」。
**结论**：S2 校验器只拒 **9 / 5 754 = 0.16%**，且那 9 行全是空 topic 的历史垃圾 ⇒ **强制校验不会伤害任何真实消息**。

★ 全部查询均为 `SELECT`，wrangler 元数据 `changed_db=false` / `rows_written=0`（见各文件内 meta）。

## 文件索引

| 文件 | 内容 | 关键结果 |
|---|---|---|
| `q1-shadow-validate.sql` | 校验器 S1 定义（用 `direction` 列判定） | — |
| `q2-topic-shapes.sql` | topic 形态分布 | — |
| `shadow-validate.txt` | S1 + 形态的原始输出 | `n_total=5754` · `n_topic_empty=9` · `n_payload_empty=0` · `n_reject_strict=876`；形态 3 种（4 202 / 1 543 / 9） |
| `q3-direction-dist.sql` | `direction` 列分布 | — |
| `q4-shadow-validate-s2.sql` | ★ 校验器 **S2**（按 **topic 后缀**判定，不用 `direction` 列） | — |
| `shadow-validate-s2.txt` | ★ S2 原始输出 | `n_suffix_up=2625` · `n_suffix_down=2252` · `n_suffix_log=868` · `n_suffix_other=9` · **`n_reject_s2=9`** |
| `q5-empty-topic-rows.txt` | 被拒的 9 行逐行身份 | id `2265/2266/2267/2276/2451/2452/2453/2454/2455`，`2026-10-04`～`10-11`，空 topic / 空 device / 空 cmd / payload `""` |

## 两条关键判读

1. **S1（用 `direction` 列）会拒 876 行，S2（用 topic 后缀）只拒 9 行** —— 差别在于 `direction='unknown'` 的 876 行是
   `2026-10-16 01:20` 之前 `/log` 走 `mqtt_messages` 的历史窗口（id 2265–3483）。
   ⇒ **校验必须针对"入站 topic"，不能针对"已落库的 direction 列"**，否则会误杀 867 行历史合法日志。
2. `topic LIKE '%/log'` 868 行 vs `direction='log'` 1 行 ⇒ 差额 867 行证明 **`B-2c` 修的是历史盲区**，不只是新增枚举值。

## 复现

```bash
cd D:/Guo_Feeder_Project/Cloudflare_Assets/guo-feeder-api
node ../node_modules/wrangler/bin/wrangler.js d1 execute guofeeder --remote --json \
  --command "$(cat <本目录>/q4-shadow-validate-s2.sql)"
```

## 局限声明

- 影子校验只覆盖 `mqtt_messages` **已落库**的行；未落库（曾被拒/丢失）的消息不在样本内。
- 样本时点 `2026-10-21 02:49`，设备自 `01:20` 静默 ⇒ 样本不含"当前固件正在发什么"的最新形态。
- 「S2 通过」只证明**格式合法**，不证明**来源可信**。
