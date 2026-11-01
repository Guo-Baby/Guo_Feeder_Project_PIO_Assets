# 米家 SIID 2 · PIID 全表只读扫描报告

> 日期：**2026-10-31** · 设备 did `644924350`（米家喂食器）
> 扫描方式：云端 `POST /miotspec/prop/get`（**只读属性**）
> 生产证据：`Cloudflare_Assets/docs/evidence/piid-scan-siid2-1-40.md`、`piid-scan-siid2-41-100.md`
> 执行脚本：`Cloudflare_Assets/scripts/scan_piid_prod.mjs` → Worker `GET /api/ui/xiaomi/piid-scan`

---

## 0. 安全声明（先看这条）

| 项 | 结果 |
|---|---|
| 调用了出粮命令？ | **没有**。`/miotspec/action` 在本仓库**没有任何调用点**（代码层结构性保证） |
| 调用了插座通电/断电？ | **没有**。`/miotspec/prop/set` 同样**没有任何调用点** |
| 扫描期间有无物理动作？ | **无**。只发"读属性"请求 |
| 已知 action 是否被调用？ | **没有**。只登记名称与风险（见 §3） |
| 扫描范围 | siid 2 的 piid **1–100**（41–100 全部"不存在"⇒ 属性空间到 32 为止） |

实现层面：`Cloudflare_Assets/guo-feeder-api/src/xiaomi/piid-scan.js`
只有一个出口 `fetchPropGet()`（→ `/miotspec/prop/get`），
且 `writable` 字段恒为 `false`，`probe_method` 恒为「cloud prop/get（只读）」。

---

## 1. 结论速览

* **可读属性 16 个**（`code=0`）：piid 1、6、7、10、11、13、15、18、20、22、23、26、28、29、31、32
* **只写属性 7 个**（`-704030013`，设备拒绝读取）：piid 8、16、17、21、24、25、27
* **属性不存在 77 个**（`-704040003`）：piid 2–5、9、12、14、19、30、33–100

### MIoT 错误码释义（实测归纳）

| 错误码 | 含义 |
|---|---|
| `-704040003` | 该 piid **不在本设备规格内**（属性不存在） |
| `-704030013` | 该 piid **不可读**（只写属性，设备拒绝 `get`） |

---

## 2. 完整表（siid 2，piid 1–40）

| piid | 名称 | 类型 | 状态 | code | 返回值 | 可读 | 可写 | 风险 |
|---|---|---|---|---|---|---|---|---|
| 1 | sensor_fault | property | 探测成功 | 0 | 0 | 是 | 否 | low |
| 2 | — | unknown | 探测失败 | -704040003 | — | 否 | 否 | unknown |
| 3 | — | unknown | 探测失败 | -704040003 | — | 否 | 否 | unknown |
| 4 | — | unknown | 探测失败 | -704040003 | — | 否 | 否 | unknown |
| 5 | — | unknown | 探测失败 | -704040003 | — | 否 | 否 | unknown |
| 6 | food_low | property | 探测成功 | 0 | 0 | 是 | 否 | low |
| 7 | unknown_7 | property | 探测成功 | 0 | 1 | 是 | 否 | low |
| 8 | **dispense_portions** | **action_input** | 探测失败 | -704030013 | — | 否 | 否 | **high（出粮入参，禁止写入）** |
| 9 | — | unknown | 探测失败 | -704040003 | — | 否 | 否 | unknown |
| 10 | food_stuck | property | 探测成功 | 0 | 0 | 是 | 否 | low |
| 11 | bowl_abnormal | property | 探测成功 | 0 | 0 | 是 | 否 | low |
| 12 | — | unknown | 探测失败 | -704040003 | — | 否 | 否 | unknown |
| 13 | refill_due | property | 探测成功 | 0 | 0 | 是 | 否 | low |
| 14 | — | unknown | 探测失败 | -704040003 | — | 否 | 否 | unknown |
| 15 | bowl_heap | property | 探测成功 | 0 | 0 | 是 | 否 | low |
| 16 | write_only_16 | write_only | 探测失败 | -704030013 | — | 否 | 否 | **high** |
| 17 | write_only_17 | write_only | 探测失败 | -704030013 | — | 否 | 否 | **high** |
| 18 | **eat_grams** | property | 探测成功 | 0 | 0 | 是 | 否 | low |
| 19 | — | unknown | 探测失败 | -704040003 | — | 否 | 否 | unknown |
| 20 | unknown_20 | property | 探测成功 | 0 | 0 | 是 | 否 | low |
| 21 | write_only_21 | write_only | 探测失败 | -704030013 | — | 否 | 否 | **high** |
| 22 | bowl_weight | property | 探测成功 | 0 | 0 | 是 | 否 | low |
| 23 | unknown_23 | property | 探测成功 | 0 | 0 | 是 | 否 | low |
| 24 | write_only_24 | write_only | 探测失败 | -704030013 | — | 否 | 否 | **high** |
| 25 | write_only_25 | write_only | 探测失败 | -704030013 | — | 否 | 否 | **high** |
| 26 | **dispensing** | property | 探测成功 | 0 | 0 | 是 | 否 | low（固件在用） |
| 27 | write_only_27 | write_only | 探测失败 | -704030013 | — | 否 | 否 | **high** |
| 28 | unknown_28 | property | 探测成功 | 0 | 0 | 是 | 否 | low |
| 29 | unknown_29 | property | 探测成功 | 0 | **100** | 是 | 否 | low |
| 30 | — | unknown | 探测失败 | -704040003 | — | 否 | 否 | unknown |
| 31 | unknown_31 | property | 探测成功 | 0 | 0 | 是 | 否 | low |
| 32 | low_intake | property | 探测成功 | 0 | 0 | 是 | 否 | low |
| 33–40 | — | unknown | 探测失败 | -704040003 | — | 否 | 否 | unknown |

**piid 41–100**：全部 `-704040003`（属性不存在）⇒ 该设备的属性空间到 **piid 32** 为止。

---

## 3. 已知功能映射（动作侧）

动作走 `aiid`（不是 `piid`），**只登记、绝不调用**：

| siid/aiid | 动作 | 风险 |
|---|---|---|
| **2/1** | **dispense（出粮）** | ⚠ **物理动作** —— 固件侧命令 `method:"action"`, `in:[{piid:8,value:份数}]` |
| 2/2 | calibrate（校准） | ⚠ 物理动作 |
| 5/1 | low_intake_settings | 写配置 |
| 5/2 | write_schedule | 写配置 |
| 5/3 | refill_settings | 写配置 |
| 5/4 | schedule_display_action | 写配置 |
| 5/5 | clear_low_intake | 写配置 |
| 5/6 | clear_refill | 写配置 |
| 6/1 | reset_desiccant | 写配置 |

> ★ 云端与局域网的**动作路径不同但都存在**：
>   云端 `/miotspec/action`；局域网 `method:"action"`（固件 `xiaomi_lan.cpp`）。
>   本轮**两端都没有调用**。

---

## 4. 本轮新增的认知（已回写进代码规格表）

| 发现 | 依据 | 处理 |
|---|---|---|
| `siid2/piid18` = **进食克数** | `eco.js` 解析喂食器历史时，`eiid=1`（进食）事件的克数就取自 `piid18`（同一命名空间） | 登记为 `eat_grams` |
| `siid2/piid26` = dispensing（出粮中） | 固件 `xiaomi_lan.cpp` 的 `REQ_READ_FEEDER` 读的就是 2/26 | 已在规格表 |
| `siid2/piid22` = bowl_weight（碗重） | 已知表 | 已在规格表 |
| `siid2/piid8` = 出粮份数**入参** | 固件动作负载 `in:[{piid:8,value:N}]` | 登记为 `action_input`（只读探测，禁止写入） |
| 7 / 20 / 23 / 28 / 29 / 31 **可读但语义未确认** | 读得到值（29 = 100，疑似容量/上限） | **如实登记为 `unknown_xx`，不编造功能名** |
| 16 / 17 / 21 / 24 / 25 / 27 **不可读** | 设备返回 -704030013 | 登记为 `write_only`（高风险，禁止写入） |

---

## 5. 对固件 / 业务的影响评估

1. **固件当前只读 2/26（dispensing）与 2/1（插座 power）** —— 这两个都在可读集合里 ✓
   （插座是**另一台设备**的 siid 2；喂食器 2/1 = sensor_fault，注意别与插座 2/1 = power 混淆。）
2. **出粮克数（piid 8）是只写属性** ⇒ 云端**读不到**"上次出粮多少克"。
   这从**协议层**解释了 P2-3「出粮克数不可信/不可得」：
   设备根本不允许读取该值，之前页面上的"出粮 10 克"只能是推算出来的。
   ⇒ **隐藏出粮克数的决定现在有了协议级依据**，不只是"不可信"，而是"读不到"。
3. **可读的 6 个新属性（7/20/23/28/29/31）语义未知** ⇒ 暂不接入业务；
   如需使用，需先做受控实验（例如手动投喂前后对比读数变化）才能确定含义，
   属于**需要用户在设备旁配合**的验收项。

---

## 6. 剩余 / 未完成

| 项 | 原因 |
|---|---|
| 未扫描 siid 1 / 3 / 4 / 5 / 6 | 用户本次授权范围是 **SIID 2**；其它 siid 属同一设备的其它服务，未纳入 |
| 6 个可读但语义未知的属性未定性 | 需要"投喂前后对比读数"的受控实验（需用户在设备旁） |
| 未调用任何 action | **按授权要求**（绝对禁止） |

---

## 7. 复现方式

```bash
# 生产扫描（需 UI 登录态；脚本自带登录）
node Cloudflare_Assets/scripts/scan_piid_prod.mjs 2 1 40
node Cloudflare_Assets/scripts/scan_piid_prod.mjs 2 41 100

# 主机侧契约测试（不联网、不碰真实账号）
node Cloudflare_Assets/guo-feeder-api/test/current/xiaomi_piid_scan.test.mjs
```
