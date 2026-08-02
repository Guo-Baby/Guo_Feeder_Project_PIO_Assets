# AI Context - ESP32 Automatic Cat Food Powder Dispenser

Version: 1.0  
Last Updated: 2026-08-02


# 1. Project Overview


This project is an ESP32-S3 based automatic cat food powder dispensing machine.

The system is designed to automatically:

- Measure cat food powder
- Dispense powder
- Control valves and motors
- Execute user-defined workflows
- Communicate with cloud services
- Display device status


Hardware platform:

- ESP32-S3
- Stepper motors
- Valves
- Sensors
- OLED display
- WiFi
- MQTT communication


Development environment:

- PlatformIO
- Arduino ESP32 framework
- C++


The firmware uses a modular layered architecture.


---

# 2. System Architecture


The firmware is divided into the following layers:


## Cloud Layer


## CloudManager


CloudManager handles all cloud communication.


Responsibilities:

- MQTT communication
- Receive cloud messages
- Upload device status
- Convert between JSON and internal C++ structures


CloudManager does not control execution logic.

CloudManager does not:

- Start workflows
- Execute actions
- Access hardware drivers directly


All commands received from cloud must be forwarded to CommandManager.


---


# Command Layer


## CommandManager


CommandManager is the global command routing center.


Responsibilities:

- Receive commands from external sources
- Parse command structure
- Identify command type
- Route commands to execution modules
- Manage command lifecycle
- Return command results through callback


All external commands must pass through CommandManager.


Command sources:

- Cloud
- Local UI
- Other external interfaces


CommandManager is responsible for command management only.

It does not directly operate hardware.


---


# Execution Layer


## WorkflowManager


WorkflowManager is the execution engine.


Responsibilities:

- Execute workflows
- Maintain workflow runtime
- Schedule actions
- Manage workflow state
- Handle action execution sequence


WorkflowManager is the only module allowed to execute Actions.


WorkflowManager owns:

- Workflow queue
- Workflow runtime state
- Action execution state


---


# Action Layer


## Action


Action represents the smallest executable hardware operation.


Examples:

- Open valve
- Close valve
- Move motor
- Read sensor
- Control actuator


Responsibilities:

- Execute one hardware operation
- Communicate execution status
- Report success or failure


Actions should only perform their own operation.

Actions do not know:

- Cloud communication
- Command parsing
- JSON protocol
- User requests


Action execution path:


WorkflowManager

↓

Action

↓

Hardware Driver


---


# Hardware Driver Layer


Hardware Driver is the lowest software layer.


Responsibilities:

- GPIO control
- PWM control
- Stepper motor control
- Sensor communication
- Peripheral access


Hardware Driver only provides hardware abstraction.


Hardware Driver does not know:

- Cloud
- Command
- Workflow
- Action meaning
- User requests


Example:


Action

↓

Stepper Driver

↓

Motor


Action

↓

Valve Driver

↓

Valve


---


# Display Module


## DisplayManager


DisplayManager is an independent service module.


Responsibilities:

- Display device status
- Display workflow progress
- Display errors
- Display user information


DisplayManager receives information from system modules.


DisplayManager does not:

- Control hardware
- Execute workflows
- Communicate with cloud directly


Data flow:


System Status Sources

↓

DisplayManager

↓

OLED Driver


---


# 3. Data Flow


## Downstream Command Flow


External Command

↓

CloudManager

↓

CommandManager

↓

WorkflowManager

↓

Action

↓

Hardware Driver



## Upstream Status Flow


Hardware Driver

↓

Action

↓

WorkflowManager

↓

CommandManager

↓

CloudManager

↓

Cloud



All communication follows the layer order.


---


# 4. Communication Design


External communication uses JSON format.


Internal communication uses C++ structures.


CloudManager is responsible for:

- JSON parsing
- JSON generation
- Cloud protocol compatibility


Internal modules should not handle JSON directly.


Example:


CommandManager

↓

CommandResult structure

↓

CloudManager

↓

JSON message


CommandResult should contain:

- Command ID
- Execution status
- Error information
- Timestamp
- Optional result data


---


# 5. Configuration System


The device configuration is stored in JSON files.


Configuration files:


## config.json


Contains:

- WiFi configuration
- Device parameters
- System settings


## workflow.json


Contains:

- Workflow definitions
- Action sequences


## ConfigManager


ConfigManager is responsible for all configuration operations.


Responsibilities:

- Load configuration files
- Parse configuration data
- Validate configuration
- Modify configuration
- Save configuration files


Only ConfigManager can write configuration JSON files.


Configuration update flow:


Command

↓

CommandManager

↓

ConfigManager

↓

Validate

↓

Save JSON

↓

Reload affected module or restart system if required


---


# 6. Execution Model


The firmware uses a non-blocking queue and state-machine based execution model.


Main loop responsibilities:

- Process pending commands
- Advance workflow runtime
- Update action states
- Process callbacks and events


Long operations must maintain execution state instead of blocking.


Examples:

- Motor movement
- Dispensing process
- Sealing process


Each operation should be represented by:

- Runtime state
- Progress information
- Completion status


---


# 7. Callback Design


Callbacks are used for asynchronous completion notification.


Typical flow:


CommandManager starts workflow.

↓

WorkflowManager executes.

↓

WorkflowManager calls callback.

↓

CommandManager generates command result.


Callbacks are notification mechanisms.

They are not execution functions.


---


# 8. Project Structure


Typical project structure:


src/

- main.cpp
- managers/
- workflow/
- actions/
- drivers/
- display/


managers/

Contains system management modules:

- CloudManager
- CommandManager
- ConfigManager


workflow/

Contains workflow execution engine.


actions/

Contains hardware operation definitions.


drivers/

Contains low-level hardware drivers.


display/

Contains OLED and display related modules.


config/

Contains JSON configuration files.

