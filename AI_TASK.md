# Task: Investigate ESP32 ESP-MQTT message loss / ordering issue

## Background

Project:
ESP32-S3 IoT device

MQTT implementation:
ESP-IDF native esp-mqtt

Current publish:

esp_mqtt_client_enqueue()

QoS1

store=true


## Observed problem

cloud_send_up() sends two messages:

1. ACK

2. RESULT


Serial output:

enqueue ACK
enqueue RESULT


However cloud receives:

Case A:
RESULT first
ACK second


Case B:
ACK only


Case C:
multiple old ACK messages


Case D:
After reboot:
first command may receive old online message before result


## Important facts

1. cloud_send_up() return success.
2. Serial confirms result JSON generated.
3. No delay used.
4. No cloud cmd_id deduplication currently.
5. MQTT broker/web frontend cannot distinguish up/set.
6. Problem happens even with small payload:
ACK <100 bytes
RESULT <150 bytes


## Questions to investigate

### 1. esp_mqtt_client_enqueue internals

Need confirm:

- Does enqueue guarantee FIFO?
- What is outbox structure?
- How is QoS1 message stored?
- When is message removed?
- What happens if PUBACK arrives?
- Can messages be reordered?


### 2. Need inspect:

esp_mqtt_client_enqueue()

mqtt_client_enqueue()

outbox_enqueue()

outbox_delete()

mqtt_task()


### 3. Need add diagnostics

Print:

- enqueue return msg_id
- MQTT_EVENT_PUBLISHED
- PUBACK time
- outbox size before/after enqueue
- outbox size after PUBACK
- MQTT reconnect events


Example:

[ENQUEUE]
msg_id=10
type=ACK
size=40
outbox=1


[ENQUEUE]
msg_id=11
type=RESULT
size=90
outbox=2


[PUBACK]
msg_id=10
time=xxxx
outbox=1


[PUBACK]
msg_id=11
time=xxxx
outbox=0


## Need determine

Is the problem:

A. ESP MQTT outbox
B. MQTT broker
C. cloud frontend queue
D. cloud_manager code
E. FreeRTOS scheduling

Do not modify architecture.
Only provide diagnosis first.