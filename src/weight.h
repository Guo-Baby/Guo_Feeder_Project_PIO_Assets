#pragma once

// HX711初始化
void weight_init();


// HX711任务
void weight_task();


// 获取原始值
int weight_get_raw();

// 获取重量值，单位克
float weight_get_gram();

// 设置当前为空载零点
void weight_zero_calibrate();