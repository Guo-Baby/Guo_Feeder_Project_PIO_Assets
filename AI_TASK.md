# Weight 模块升级需求

目标：
按照《新增动作模板.md》的标准，将 weight 模块改造成标准 Workflow Trigger + Action 模块。
修改当前阻塞执行机制，改为状态机。
本次任务仅修改weight.cpp weight.h，禁止修改其他文件。如果由于本模块提供的接口在其他模块中无相关实现或调用导致编译失败，不得做临时修改以通过编译，保留完整相关接口，忽略由于该原因导致的编译失败。


# 1. Weight Trigger 注册

新增 Workflow Trigger：

ID:

weight_decrease


Name:

重量减少

 description = "当重量减少*克时执行下一步",

类型：

Workflow Trigger


按照新增动作模板.md内trigger模板实现：

- reset
- start
- poll


Descriptor 生命周期：

workflow_register_trigger()


参数：


gram


单位：

克


2. Weight Decrease Trigger 行为


Trigger 启动时：
首先检查 STATE_WEIGHT_ERROR如果为true则发布事件EVENT_WEIGHT_ERROR，返回 TRIGGER_FAILED。


 如果为false则执行下方步骤：
记录当前重量：
start_weight

运行期间：

持续读取重量。

计算：

weight_loss =
start_weight - current_weight

注意该weight不是HX711提供的raw数据，而是滤波算法后得到的重量数据
当：

weight_loss >= gram

触发：

TRIGGER_SUCCESS

否则：

TRIGGER_RUNNING
3. Trigger 参数保护

workflow传入参数需要增加保护。

合法范围：

0 < gram <= 300

以下情况视为非法：

参数不存在
params为空
gram=0
gram<0
gram>300

非法参数自动替换：

gram = 20

并发布WEIGHT_ERROR事件

4. HX711采样机制优化

当前：

固定 millis 周期读取。

修改：

采用：

HX711 is_ready()

作为采样触发。

逻辑：

loop中：

if(scale.is_ready())
{
    read()
}
else
{
    return;
}

只有成功读取新的HX711数据：

才进入：

滤波
重量计算
SystemState更新

禁止：

重复读取同一个HX711数据。

5. 滤波算法调整

HX711输出：

10Hz

目标：

调整：

当前：

8~10点平均

修改：

使用：

5点窗口

即：

平均约0.5秒更新一次weight，实际采用HX711库提供的scale.is_ready()每提供5次数据计算1次重量，但按窗口期更新system state，见下第7点。

滤波：

保留：

去最大值
去最小值
剩余平均

6. Weight Error事件

新增异常检测。

算法：

比较两个连续重量窗口。

例如：

窗口A：

最近5次平均

窗口B：

下一组5次平均

计算：

delta = windowB - windowA

判断：

不能使用 abs(int)

必须使用：

fabs(float)

异常条件：

fabs(delta) >= 50.0

触发：

EVENT_WEIGHT_ERROR

事件内容：

"Weight jump detected"
EVENT_PRIORITY_CRITICAL   设置为3
其余按照event_manager.h提供的模板设置。

7. System State
从system_state.h获得修改state的入口函数。
维护：
STATE_WEIGHT_VALUE

更新策略：

空闲状态：weight_active =false则 30秒更新一次。


weight active=true状态：按照设计的滤波算法，每次重量值更新都同步更新 STATE_WEIGHT_VALUE


维护： STATE_WEIGHT_ERROR
如果重量raw值长期为零，或5s内重量跳动触发 EVENT_WEIGHT_ERROR 5次，则设置 STATE_WEIGHT_ERROR 为true。

8. Workflow Active状态接口

增加weight模块状态：

weight_active

状态：

false
空闲

true
workflow重量触发运行

进入weight trigger：

weight_active=true

trigger完成：

weight_active=false

reset：

恢复：

false
9. Weight Zero Action

新增 Workflow Action：

ID:

WEIGHT_ZERO

功能：

name:电子秤零点校准


 description = "保证电子秤空载时执行此命令",


执行零点校准。

实现：

按照新增动作模板：

reset
start
poll

start执行：

调用：

weight_zero_calibrate()

10. Zero Offset配置保存接口

当前：

weight_zero_calibrate()

只修改内存。

需要增加config_manager接口。

config_manager新增：

bool config_set_weight_zero_offset(int value);

weight模块：

负责计算新的zero offset。

config_manager：

负责：

修改配置
保存config.json

调用链：

CommandManager

↓

Workflow

↓

WEIGHT_ZERO Action

↓

weight计算zero offset

↓

config_set_weight_zero_offset()

↓

config_save()

此次只修改weight.cpp和weight.h，不修改其他模块。
如果由于config.cpp无相关函数实现导致编译报错，则忽略，下一步按命令修改config.cpp。

11. Weight模块职责边界

weight负责：

HX711读取
数据滤波
重量计算
Weight Trigger
Weight Error事件
Weight Action

config_manager负责：

参数读取
参数修改
config.json保存

Workflow负责：

trigger/action流程

CommandManager负责：

云端命令解析
workflow/action调用
12. 禁止阻塞

所有weight功能：

禁止：

while等待HX711
长delay
阻塞workflow

所有任务：

必须通过：

weight_task()

在loop周期运行。

