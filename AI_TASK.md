请先阅读当前 src/cloud_manager.h 和 src/cloud_manager.cpp 代码。

当前 CommandManager 已经完成定版，不需要重新设计。现在只修改 CloudManager，使其符合最终架构。

修改目标：

1. CloudManager 职责调整

CloudManager 只负责：
- MQTT 通信
- MQTT JSON → CommandMessage 转换
- 短周期消息防风暴

CloudManager 不负责：
- command 生命周期管理
- command 执行去重
- workflow/action 状态管理

command_id 的业务生命周期判断已经由 CommandManager Runtime 负责。

---

2. 修改 payload 传递方式

当前 CommandMessage.payload 定义为 String。

修改 CloudManager：

MQTT 接收：

{
  cmd,
  ob,
  id,
  src,
  pl,
  ts
}

转换为：

CommandMessage
{
  command,
  object,
  cmd_id,
  payload,
  source,
  timestamp
}

其中：

payload 保持原始 String。

禁止：
- CloudManager 解析 payload
- CloudManager 将 payload 转换成 JsonObject
- CloudManager 理解 payload 业务含义

payload 只负责完整传递。

---

3. 修改 MQTT cmd_id 防重复策略

删除当前长期 cmd_id cache 设计。

不要保存 10 分钟。

改为：

短周期 FIFO duplicate protection。

要求：

- 使用固定大小 FIFO ring buffer
- 保存最近收到的 cmd_id
- 保存时间戳 received_ms
- 有效时间约 30 seconds
- 超过30秒自动失效

目的：

只防止：
- MQTT 重复发送
- UI 短时间重复点击
- 网络异常导致的消息风暴

不是业务去重。

真正 command_id 冲突由 CommandManager Runtime 处理。

---

4. 保留已有功能

保持：

- MQTT connect/reconnect
- cloud_send_set()
- cloud_send_up()
- command result callback
- WiFi event处理
- MQTT状态同步

不要改变接口。

---

5. 代码限制

只修改：
- cloud_manager.h
- cloud_manager.cpp

禁止：
- 新建 manager
- 修改 CommandManager
- 修改 WorkflowManager
- 修改 Action
- 修改 system_state

---

请先基于当前代码输出修改计划。

计划中只包含：
- 修改文件
- 修改函数
- 修改数据结构
- 修改流程

不要重新设计架构，不要提出其他方案。

补充说明：当前代码中已经发现以下明确问题，请严格按照下面要求修复，不要自行设计其他方案。

1. CommandMessage.payload 类型不匹配问题

当前：
CommandMessage.payload 已经定义为 String。

但是 cloud_manager.cpp 中：

cmd.payload = payload_obj;

这里直接把 JsonObjectConst 赋值给 String，不符合当前架构。

修改要求：

MQTT 收到的 pl 字段必须保持原始数据传输。

请修改为：

- 如果 pl 存在，将 pl 序列化为 String 保存到 cmd.payload。
- CloudManager 不解析 pl 内部字段。
- CloudManager 不访问 payload 内部业务参数。

示例：

JsonDocument payload_doc;
serializeJson(doc["pl"], cmd.payload);

payload 只作为透明数据传递给 CommandManager。

---

2. CommandManager 调用缺失问题

当前 mqtt_callback() 中：

只有：

String command_manager_execute(CommandMessage &cmd);

这是错误的，只是声明，没有实际执行。

修改要求：

必须直接调用：

command_manager_execute(cmd)

执行收到的 CommandMessage。

同步返回结果按照现有 CommandResultCallback 机制处理。

不要增加新的执行流程。

---

3. cmd_id 防重复机制修改

当前：

MQTT_DUP_CACHE_SIZE + cmd_id_cache

属于长期缓存设计。

修改要求：

改成短周期 FIFO ring buffer。

要求：

数据结构：

struct CmdIdCacheEntry
{
    String cmd_id;
    unsigned long received_ms;
};

固定数量，例如10个。

逻辑：

- 收到 MQTT command 时检查 cmd_id。
- 如果相同 cmd_id 在30秒内出现，则认为重复消息，直接丢弃。
- 超过30秒自动失效，可以重新接受。
- 新 cmd_id 按 FIFO 写入。

目的只是防止：
- MQTT重复投递
- UI短时间重复点击
- 网络异常造成消息风暴

不是业务层去重。

业务层 command_id 冲突由 CommandManager Runtime 判断。

---

4. 保留接口，不扩大 CloudManager 职责

禁止：

- CloudManager 保存 command runtime。
- CloudManager 判断 command 是否执行完成。
- CloudManager 判断 workflow/action 状态。
- CloudManager 解析 payload。
- CloudManager 增加 workflow/action 相关逻辑。

CloudManager 只负责：

MQTT通信
↓
CommandMessage转换
↓
发送CommandManager
↓
上传结果

---

5. 修改范围限制

只修改：

src/cloud_manager.h
src/cloud_manager.cpp

不要修改：

command_manager
workflow
action
system_state

除非为了编译错误进行最小接口适配，否则不要扩散修改。

请基于以上明确问题重新检查当前代码，并生成最终修改计划。