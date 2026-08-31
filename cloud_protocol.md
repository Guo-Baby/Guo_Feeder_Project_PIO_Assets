# Cloud Protocol 云通信协议文档

> 
> 项目：Guo Feeder Project
> 版本：V1.0
> 日期：2026-08-23
> 说明：设备 ↔ 云平台（巴法云）双向通信，MQTT 传输，支持 JSON / CBOR 双编码，推荐 CBOR 降低报文长度；QoS 1，保证消息至少送达一次。
> 配套：内置协议枚举定义（C 语言枚举，设备端直接引用）
> 重要变更：启用**全局标准缩写字段**，上下行统一，用于报文压缩，减少载荷体积；移除业务示例报文，保留纯协议规范；心跳由 MQTT 底层保活实现，应用层不实现心跳上报。

## 0 设备端 C 协议枚举定义

> 
> 存放于 `cloud_protocol_enum.h`，协议文档与代码枚举严格一一对应，不可单方面修改

```
// =====================================================
// 协议枚举（云端协议定义）
// =====================================================

// 状态枚举: 上行统一 status -> s
enum CloudStatus
{
    STATUS_OK = 0,
    STATUS_ACCEPTED,
    STATUS_RUNNING,
    STATUS_FAILED,
    STATUS_ERROR        // 协议错误（版本不匹配 / 非法命令 / 非法对象等）
};

// 错误码: 上行统一 error -> e
enum ErrorCode
{
    ERROR_NONE = 0,
    ERROR_INVALID_COMMAND,     // 非法命令
    ERROR_INVALID_OBJECT,      // 非法对象 / stable id 不存在
    ERROR_VERSION_MISMATCH,    // registry 版本不匹配
    ERROR_NOT_FOUND,
    ERROR_BUSY,
    ERROR_TIMEOUT,
    ERROR_INTERNAL
};

// registry 类型: 下行 { "c":"registry", "k":0/1/2 }
enum RegistryType
{
    REGISTRY_ACTION = 0,
    REGISTRY_TRIGGER,
    REGISTRY_WORKFLOW
};

// 内部消息类型判断
enum CloudMessageType
{
    CLOUD_MSG_COMMAND = 0,
    CLOUD_MSG_RESULT,
    CLOUD_MSG_ACK,
    CLOUD_MSG_REGISTRY,
    CLOUD_MSG_FRAGMENT
};

// 分片类型: BEGIN 0 / DATA 1 / END 2
enum FragmentType
{
    FRAG_BEGIN = 0,
    FRAG_DATA,
    FRAG_END
};
```

## 1 基础通信规范

### 1.1 传输层

- 协议：MQTT 3.1.1
- QoS：**QoS 1**
- 保活时间：30s（底层 MQTT 库自主处理，应用层无需实现心跳逻辑）
- 编码选项：
  1. JSON：调试、日志、PC 端查看使用，人类可读
  2. CBOR：正式业务上行下行，二进制压缩，节约流量
- 消息最大载荷：≤1024 字节（巴法云平台限制），超长数据启用分片传输

### 1.2 Topic 规范（巴法云格式）

> 
> `{设备私钥}/{指令方向}`

- 上行（设备 → 云）：`{uid}/up`
- 下行（云 → 设备）：`{uid}/down`

### 1.3 顶层固定缩写字段（上下行通用，必选头部）

> 
> 全部报文顶层使用缩写，不再使用长字段名，压缩报文体积
> | 缩写字段 | 原始全称 | 类型 | 说明 |
> | ---- | ---- | ---- | ---- |
> | v | ver | string | 协议版本，固定 `1.0` |
> | mid | msg_id | string /uint | 消息唯一 ID，应答匹配，自增数字优先 |
> | d | dir | string | 方向：`up` 上行，`down` 下行 |
> | t | type | string | 消息类型，映射 CloudMessageType 枚举 |
> | ts | ts | uint32 | UTC 时间戳（秒） |
> | dat | data | any | 业务载荷主体，所有业务字段存放于此 |

> 
> type 字符串与 CloudMessageType 枚举映射表
> | type 缩写串 | CloudMessageType |
> | ---- | ---- |
> | cmd | CLOUD_MSG_COMMAND |
> | res | CLOUD_MSG_RESULT |
> | ack | CLOUD_MSG_ACK |
> | reg | CLOUD_MSG_REGISTRY |
> | frag | CLOUD_MSG_FRAGMENT |

## 2 dat 载荷内标准缩写字段（上下行通用）

> 
> dat 对象内部统一缩写，所有业务报文遵守，自定义业务字段另行扩展，保留向后兼容
> | 缩写字段 | 原始全称 | 归属枚举 | 说明 |
> | ---- | ---- | ---- | ---- |
> | s | status | CloudStatus | 执行状态码 |
> | e | error | ErrorCode | 错误编码 |
> | k | kind | RegistryType | 注册表查询类型，reg 类型专用 |
> | ft | frag_type | FragmentType | 分片类型，frag 类型专用 |
> | tot | total | uint | 分片总数量，frag 类型专用 |
> | idx | index | uint | 当前分片序号，frag 类型专用 |
> | pl | payload | binary /string | 分片原始载荷，frag 类型专用 |
> | n | name | string | 动作名称，cmd 动作调用专用 |
> | p | params | object | 动作入参集合，cmd 动作调用专用 |
> | wid | workflow_id | string | 工作流 ID，cmd 流程调用专用 |
> | msg | message | string | 可读文本，错误 / 应答描述 |

## 3 消息类型约束

1. **cmd（CLOUD_MSG_COMMAND）**：下行专用，云端下发执行指令（action /workflow）
2. **reg（CLOUD_MSG_REGISTRY）**：下行专用，注册表查询指令，dat.k 指定查询分类
3. **ack（CLOUD_MSG_ACK）**：双向通用，应答报文，匹配 mid，携带 s/e/msg
4. **res（CLOUD_MSG_RESULT）**：上行专用，状态上报、执行结果返回
5. **frag（CLOUD_MSG_FRAGMENT）**：双向通用，超长报文分片传输

## 4 枚举映射强制规则

1. `dat.s` → enum CloudStatus
2. `dat.e` → enum ErrorCode
3. `dat.k` → enum RegistryType
4. `dat.ft` → enum FragmentType
5. 顶层 `t` → enum CloudMessageType

## 5 设备侧处理规则

1. MQTT 回调函数只做**入队**，业务逻辑禁止在回调执行，投递到 RTOS task 处理
2. 收到下行消息，校验顶层 `v` 协议版本，版本不匹配直接丢弃，回复对应 ack
3. 所有上行消息，封装完成后调用统一接口 `cloud_send_up()`
4. CBOR 模式：序列化输出二进制 payload，无换行、空格格式化字符；调试切换 JSON
5. 报文载荷超过 1024 字节：启用 frag 分片传输，接收端完成重组后再解析业务数据
6. 断电重启后：重连 MQTT，上报全量状态，重新订阅上下行主题
7. 解析规则：遇到未知字段（顶层 /dat 内部）直接忽略，禁止解析失败，保证向前兼容

## 6 协议扩展约束

1. 修改任意枚举值、顶层缩写、dat 内部标准缩写，必须同步更新本文档
2. 新增消息类型，同步补充 CloudMessageType 枚举 + t 缩写映射
3. **禁止修改顶层标准缩写集合（v/mid/d/t/ts/dat）**
4. 业务自定义字段：仅允许放在 dat 对象内部，不可新增顶层字段

## 7 配套代码约定

- `cloud_send_up()`：统一上行封装函数，自动支持 JSON / CBOR 切换
- 命令分发：查表路由（command registry）替代长 if-else 判断
- 可选增强：重要控制报文可在 dat 内增加 `crc` 字段，做载荷完整性校验

## 8 调试规范

1. 开发调试阶段：JSON 明文输出，串口完整打印报文
2. 正式联调上线：切换 CBOR 二进制编码，最大化压缩报文
3. 抓包工具：MQTT.fx/ MQTT Explorer 观测上下行交互
4. 日志持久化：保留 mid，用于全链路追踪请求与应答时序

如果你后续需要，我可以再输出：

1. 纯头文件 cloud_protocol_enum.h（可直接粘贴进工程）
2. 报文合法性校验伪代码（入参检查函数）
3. CBOR 序列化 / 反序列化标准封装模板



## 9 新增config相关命令
命令	紧凑 payload
查询单条    {"c":"system","i":"1003","p":{"o":"config_query","module":"weight","key":"skc"}}
查询模块	{"c":"system","i":"1002","p":{"o":"config_query","module":"wifi"}}
查询全部	{"c":"system","i":"1003","p":{"o":"config_query"}}
修改配置	{"c":"system","i":"1004","p":{"o":"config_set","module":"wifi","key":"ssid","value":"MyHome"}}
保存	{"c":"system","i":"1005","p":{"o":"config_save"}}
重启	{"c":"system","i":"1006","p":{"o":"config_restart"}}
取消重启	{"c":"system","i":"1007","p":{"o":"config_restart","cancel":true}}