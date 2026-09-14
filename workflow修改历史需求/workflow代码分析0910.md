基于之前 Workflow Manager 审计报告，请继续补充以下架构问题分析。

目标：  
我们准备设计 WorkflowStorage + Definition/Runtime Separation。

请不要修改代码，只分析。

需要回答：

1.

如果 WorkflowStep 拆分为 Definition 和 Runtime Snapshot，  
请分析：

- Definition 是否应该全部常驻 RAM？
- Runtime 是否应该只在 workflow_start 时创建？
- 两种方案的 DRAM/PSRAM 占用估算。

1.

当前 action_instances / trigger_instances PSRAM pool，  
未来 Runtime Snapshot 是否继续复用？  
是否建议保留 pool？

1.

如果 Definition 和 Runtime 完全隔离：  
Workflow 正在运行时，是否允许修改自己的 Definition？  
例如：  
Workflow4正在运行，  
用户修改Workflow4参数，  
Runtime继续使用旧参数，  
下一次运行使用新参数。  
分析这种方案是否安全。

1.

WorkflowStorage启动加载策略：  
256个Step BIN：  
A. 全部启动加载  
B. 只加载Valid Workflow  
C. Lazy Load

请结合ESP32-S3 N16R8资源分析推荐方案。

1.

设计meta.bin最小数据结构。  
需要包含：

- Workflow Valid状态
- Version
- CRC是否需要
- 是否需要保存workflow级别状态

1.

CommandManager修改Workflow时，  
推荐调用链：

CommandManager  
?  
WorkflowManager  
?  
WorkflowStorage

请给出推荐分层。

1.

重新评估之前报告中：  
“运行中的Workflow修改应该拒绝或者Pending”  
这个结论。

在Definition/Runtime Separation之后，  
是否应该允许修改正在运行Workflow的Definition？  
说明原因。

输出：  
补充架构设计报告。
















