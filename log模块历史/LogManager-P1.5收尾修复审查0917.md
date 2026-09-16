任务：修复 LogManager P1.5 收尾修复中的测试用例问题。

重要限制：

- 不修改任何 src/ 生产代码。
- 不修改 log_ack.h、log_manager.cpp、log_manager.h、main.cpp。
- 不改变已有测试语义。
- 只允许修改 test/log_fix_tests.txt。
- 不要重新设计测试框架，不要新增测试文件。
- 修改完成后只执行现有测试验证。

目标：  
根据 diff 审查报告修复两个 HIGH 问题：

==================================================  
问题 1：修复 F8 的 sonline 逻辑错误
=========================

文件：  
test/log_fix_tests.txt

位置：  
F8（D1 replay after_seq 测试）段。

当前错误：  
该测试中使用：

```
logt sonline 0
```

导致 cloud_poll() 因 MQTT 未连接提前 return，无法进入 cloud_replay_step()。

不要修改生产代码。

修改方法：

1. 将 F8 中用于触发 replay 流程的：

   logt sonline 0

改为：

```
logt sonline 1
```

原因：  
sonline 的作用只是控制测试环境中的强制在线状态。  
D1 测试不需要验证 MQTT 离线行为，而需要确保 cloud_poll() 可以执行 replay。

1. 修改 F8 注释文字。

删除类似：

"离线下 replay 推 16 条"

这样的描述。

改成表达：

"强制 online，使 replay 流程可执行；本测试验证删除 replay 当前段后的 after_seq 约束，不验证 MQTT 离线行为。"

不要修改 F8 的其他命令顺序。  
不要修改断言值。  
不要修改 rseg / replay_seq 相关检查。

==================================================  
问题 2：修复 F2 giveup 断言时序错误
========================

文件：  
test/log_fix_tests.txt

位置：  
F2 段。

当前问题：

存在：

```
logt ackst ||| giveup=1
```

但是该命令执行时间早于 ACK retry/give-up 完成时间。

不要修改 ACK 参数。  
不要修改生产代码。

修改方法：

找到 F2 中第一次：

```
logt ackst ||| giveup=1
```

在它后面立即增加等待，让 giveup 状态有时间产生。

优先采用已有测试文件中的等待命令格式。

如果测试框架没有 wait 命令，则复制该断言一次：

```
logt ackst ||| giveup=1
logt ackst ||| giveup=1
```

第二次用于真正捕获 giveup 状态。

不要删除第一次。  
不要改变 F2 其他断言。

==================================================  
修改后检查
=====

执行：

git diff -- test/log_fix_tests.txt

确认：

1. 只有 test/log_fix_tests.txt 被修改。
2. 没有 src/ 文件变化。
3. F8 只改变 sonline 和注释。
4. F2 只增加/调整 giveup 检查时序。

然后执行：

python test/log_contract/run_contract_test.py

不要执行：

- git commit
- git add
- git reset
- git checkout

完成后输出：

1. 修改文件列表
2. diff 摘要
3. 测试结果
4. 是否可以 commit（只根据测试资产修复结果判断）  


