# AI Coding Rules

Version: 1.0
Last Updated: 2026-08-02


# 1. Architecture Boundary


The system uses strict layered architecture.


## Allowed direction


Command flow:


CloudManager

↓

CommandManager

↓

WorkflowManager

↓

Action

↓

Hardware Driver



Status flow:


Hardware Driver

↓

Action

↓

WorkflowManager

↓

CommandManager

↓

CloudManager



No module may skip layers.


Forbidden examples:


CloudManager -> WorkflowManager

CloudManager -> Action

WorkflowManager -> CloudManager

Action -> CloudManager



CommandManager is the only bridge between external commands and execution.


---

# 2. Module Responsibility


Each module must only handle its own responsibility.


CloudManager:

- MQTT communication
- Cloud data exchange
- JSON conversion


CommandManager:

- Command parsing
- Command routing
- Command lifecycle management


WorkflowManager:

- Workflow execution
- Runtime state management
- Action scheduling


Action:

- Atomic hardware operations


Hardware Driver:

- Direct peripheral control
- GPIO/PWM/SPI/I2C/UART
- Sensor and actuator drivers


Never move responsibility between layers.


---

# 3. Communication Rules


Communication priority:


## Callback

Used for:

- Task completion notification
- Asynchronous operation result


Callback limitations:

A callback must NOT:

- execute hardware operation
- start workflow
- perform long computation
- communicate with cloud


A callback should only:

- update state
- store result
- push event


Heavy processing must happen in the main loop.


---

## Queue/Event


Used for:

- cross-module notification
- state changes
- asynchronous messages


Queue requirements:

- Fixed capacity
- No memory overflow
- Every task must have execution opportunity
- No early return blocking later tasks


When queue is full:

- Reject new item
- Generate warning log
- Notify upper layer if required


Never overwrite running tasks.


---

## Direct Function Call


Allowed only when:

- Same ownership layer
- Short execution time
- No blocking


Cross-layer synchronous calls are forbidden.


---

# 4. Non Blocking Design


The firmware main loop must never be blocked.


Forbidden:


- delay()
- infinite waiting loop
- blocking callback
- waiting for hardware completion synchronously


Use:


- State machine
- Runtime status
- Queue
- Callback


---

# 5. Error Handling


Errors must propagate upward through the normal architecture path.


Example:


Hardware Driver:

generates hardware error


↓

Action:

wraps hardware error


↓

WorkflowManager:

handles workflow failure


↓

CommandManager:

creates command result


↓

CloudManager:

converts result into JSON


Error information should contain:


- Error code
- Module name
- Timestamp
- Description


---

# 6. Avoid Duplicate Systems


Before adding functionality:


1. Check existing modules.
2. Reuse existing architecture.
3. Avoid parallel implementations.


Do not create:


- Another workflow engine
- Another runtime system
- Another action management system
- Another queue system


---

# 7. Memory Management


Avoid unnecessary memory operations.


Prefer:


- Reference
- Pointer
- Structure sharing


Avoid:


- Repeated String allocation
- Unnecessary JSON duplication
- Large temporary objects


---

# 8. Code Modification Rules


When modifying code:


1. Understand existing architecture first.
2. Identify affected modules.
3. Modify minimum required files.
4. Preserve existing interfaces.
5. Do not redesign architecture unless explicitly requested.

