# `src/homie/` —— Homie 5 投影模块

> **上位冻结契约**：`docs/architecture/P2-0-architecture-review-and-design-freeze.md`（FROZEN，§5/§8/§10–§15/§17/§23）
> **阶段状态**：`P2 = SOFTWARE CLOSED / HARDWARE DEFERRED`（见 `docs/architecture/P2-8-Closure-Review.md`）
> **本文档范围**：**只描述已冻结并已实现的 P2 内容**；**不含任何未来规划功能**（不含 D-4 runtime `$description` refresh、
> 不含 D-8 设备友好名、不含 D-13 `$target`）。**改本目录代码前先读冻结文档。**

---

## 1. 职责

把设备侧已有的 **Device Model（语义定义）** + **Capability Registry（runtime 可用性）** 投影为 **Homie 5** 约定的
MQTT topic / payload，并把客户端 `/set` 反向翻译为 **既有 `CommandManager` 命令**。

| 文件 | 角色 | 说明 |
|---|---|---|
| `device_model.h` / `.cpp` | **语义定义（纯数据）** | `constexpr` 表：4 node / 16 property / 6 命令映射；**DRAM 静态 0 B**（全部落 Flash `.rodata`）。**不依赖** Capability Registry（兄弟模块，互不 include）。 |
| `homie_projection.h` | **纯逻辑层（单一真源）** | Topic 构造 / `$description` 生成 / 值序列化 / ID 校法 / `/set` topic 解析 / pending 结果分类。**零 MQTT / 零硬件 / 零业务依赖**；固件与宿主测试**编译同一份**，避免实现与测试漂移。 |
| `homie_bridge.h` / `.cpp` | **薄桥接层** | 相位机（`$state`）· boot `$description` 发布 · 属性变化检测与限流发布 · `/set` → `CommandMessage` · pending 表与结果收尾。**经 Transport Facade 收发，不持有 MQTT 客户端。** |

**边界（铁律）**：

- **单向依赖**：`homie_bridge → cloud/mqtt_transport → cloud/cloud_manager → esp_mqtt_client`。
  CloudManager **不** include `homie_bridge.h`；Homie **不**持有 `esp_mqtt_client*`。
- **绝不绕过 CommandManager**：`/set` 唯一写路径 = `command_manager_execute()`（`source="homie"`）。
- **内部模块不处理 JSON**：JSON 仅出现在 `$description` 序列化（纯逻辑层）与 topic 载荷边界。
- **不阻塞**：`homie_task()` 全程非阻塞，无 `delay()`；MQTT 未连接时不发布任何 Homie 消息。
- **编译期开关**：`GF_HOMIE_ENABLED`（**默认 0**）。为 0 时本模块**全部为空实现** ⇒ 生产零 Flash/RAM 代价。
  开启方式：`PLATFORMIO_BUILD_DIR=.pio/build/<名> PLATFORMIO_BUILD_FLAGS="-DGF_HOMIE_ENABLED=1" pio run`
  （**不要**写进 `platformio.ini`）。

---

## 2. Topic

**base topic** = `homie/5/<device_id>/`（`<device_id>` 来自 `services/device_identity.h`）

| 方向 | Topic | retained | QoS | 说明 |
|---|---|:---:|:---:|---|
| pub | `homie/5/<id>/$state` | **是**（规范强制） | 1 | 生命周期：`init` → `ready`；断连由 **broker LWT** 发 `lost` |
| pub | `homie/5/<id>/$description` | **是**（规范强制） | 1 | **仅 boot `init` 生成一次**；> 4096 B 则不发布并停在 `init` |
| pub | `homie/5/<id>/<node>/<prop>` | 按属性（只读状态默认 **是**） | 1 | 只读状态回传，变化驱动 + 死区 + 最小发布间隔 |
| sub | `homie/5/<id>/+/+/set` | — | 1 | **精确 filter，不含 `#`，不跨设备**（全工程**唯一** Homie 订阅点） |
| LWT | `homie/5/<id>/$state` = `lost` | 是（规范） | 1 | 仅在 Homie 启用时设置 |

- **QoS1 属 Guo Feeder 的 transport profile 选择，不是 Homie 规范要求。**
- `$state` 6 态取 4：`init` / `ready` / `lost`（disconnected 由 `lost` 表达）；**`ready` = 五条件合取**
  （MQTT 已连 · `$description` 已发 · 只读初值已发 · Registry 已就绪 · CommandManager 已初始化）。
- `$description` 在 **Registry rebuild 后不重生成、不重发布、不改 contract**；仅打一条 fingerprint 陈旧 `WARN`。

---

## 3. Property（4 node / 16 property）

| node | property | datatype | 可写 | 说明 |
|---|---|:---:|:---:|---|
| `water` | `valve` | `enum` | ✅ | `format="open,close"`；映射 `VALVE_OPEN` / `VALVE_CLOSE` |
| `water` | `weight` | `float` | — | `0:10000:1`，单位 `g` |
| `water` | `weight-error` | `boolean` | — | 秤不可用/断线置位 |
| `water` | `zero` | `boolean` | ✅ | 映射 `WEIGHT_ZERO`（只写命令型，`retained=false`） |
| `climate` | `temperature` / `humidity` / `battery` | `float` | — | 米家读数；有效性由 `STATE_MI_THERMO_VALID` 表达 |
| `climate` | `scan-enabled` | `boolean` | — | 扫描状态 |
| `climate` | `scan` | `enum` | ✅ | `format="start,stop"`；映射 `MI_THERMO_START_SCAN` / `MI_THERMO_STOP_SCAN` |
| `system` | `mqtt-connected` / `wifi-rssi` / `wifi-signal` / `time-valid` / `event-queue` / `mqtt-retries` | `boolean` / `integer` / `enum` | — | 只读状态 |
| `control` | `computer-reset` | `boolean` | ✅ | 映射 `COMPUTER_RESET`（只写命令型，`retained=false`） |

**ID 规则**：node / property id 一律为**语义字符串** `[a-z0-9-]`；**禁止使用 `stable_id` 作为 Homie 身份**
（`stable_id` = runtime_id 字母序 index，会随新增 Action 平移）。

**boot-time 可用性过滤**（AD-15b）：写侧要求属性的**全部** Action 已注册；无自身 Action 的只读属性按
**HomieGate** 锚定的 Action 判定。典型后果：`weight.enable=false` ⇒ `WEIGHT_ZERO` 未注册 ⇒
**`water/weight`、`water/weight-error`、`water/zero` 三者同时不进入 `$description`**（16 → 13 属性）——
这是**预期行为**，不是缺陷。

---

## 4. `/set` 流程（客户端 → 设备）

```
client publish  homie/5/<id>/<node>/<prop>/set   (裸值，非 JSON 信封)
      │
      ▼  Transport Facade 的 foreign 队列（loop 上下文，已由 P2-2 保证）
homie_on_foreign(topic, data, len, retain)
      │
      ├─ ① `retain == true`  ⇒ **一律丢弃** + WARN「retained /set dropped」
      │     （覆盖 场景 A 实时 retained publish 与 场景 B broker 重连重放；
      │      判据**只有** retain，禁用 payload / topic / 时间 / cmd_id 等启发式替代）
      │
      ├─ ② homie_parse_set_topic()：必须属于**本设备** namespace 且恰好 3 段（node/prop/set）
      │     ⇒ 否则丢弃（跨设备 / 缺 `/set` / `$state` 等一律拒绝）
      │
      ├─ ③ Device Model 查表 → dm_validate_set()：存在性 / settable / dtype / enum 成员 / 数值范围
      │     ⇒ 失败：不发布、不执行，WARN 落 LogManager
      │
      ├─ ④ dm_resolve_command()：**显式映射表**得到 { action_id, payload_json }
      │     （★ 客户端永远无法提供或覆盖 action_id）
      │
      ├─ ⑤ command_manager_execute({ object: action_id, source: "homie", ... })
      │     ⇒ 进入**既有** CommandManager / Workflow 链路（不新增执行路径）
      │
      └─ ⑥ pending 表登记（定容 4）→ 结果分类 → 终态出表（超时兜底 30 s）
```

**只读属性**：`dm_validate_set()` 返回 `NOT_SETTABLE` ⇒ 丢弃（不执行、不发布）。
**未知枚举**：返回 `ENUM_NOT_FOUND` ⇒ 丢弃。

---

## 5. 契约自检与回归

| 项 | 位置 |
|---|---|
| 纯逻辑层宿主探针（222 断言） | `test/homie_bridge_host/run.sh` |
| 软件侧制品 dump（topic / `$description` / ID / set parser） | `test/homie_bridge_host/dump.sh` |
| P2-3 静态结构检查（116 项职责/顺序） | `test/p2_3_homie_check.py` |
| P2-4 `/set` 静态检查（64 项） | `test/p2_4_homie_set_check.py` |
| 设备语义（Device Model，97 + 7 负向） | `test/device_model_host/run.sh` |

> **本模块的 `$description` 实测体积**：**1,907 B**（16 属性）/ **1,618 B**（13 属性，`weight.enable=false`）—— 均 < 4096 B 上限，不分片。

---

## 6. 明确不在本模块范围

| 项 | 去向 |
|---|---|
| runtime `$description` refresh | **DEFERRED（§23 #24 / D-4）**，不在 P2 |
| 设备友好名 / `$description.name` | **DEFERRED（§23 #25 / D-8）** → P3 |
| Homie `$target` | **DEFERRED（§23 #26 / D-13）**，不在 P2 |
| 真机 / 真 broker 端到端验证 | **`NOT RUN`（硬件 DEFERRED）** → `docs/architecture/P2-7-Hardware-Deferral-Record.md` §4（R-1…R-6） |
| OLED / Web / Android UI | 共用 Device Model，**各自由对应端实现**（§23 #21） |
| Workflow 投影 | **不投影**（§23 #22） |
