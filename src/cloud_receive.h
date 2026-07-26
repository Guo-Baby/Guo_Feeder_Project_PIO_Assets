#ifndef CLOUD_RECEIVE_H
#define CLOUD_RECEIVE_H


#include <Arduino.h>


void cloud_receive_init();


void cloud_receive_handle(
    char* topic,
    String message
);


// 获取最近一次云端命令
String cloud_receive_get_command();



#endif