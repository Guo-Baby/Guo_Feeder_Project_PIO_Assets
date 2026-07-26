#include <Arduino.h>
#include <HX711.h>

#include "weight.h"
#include "config_manager.h"
#include "system_state.h"
#include "event_manager.h"

// ==========================
// HX711对象
// ==========================
static HX711 scale;

// ==========================
// 参数
// ==========================
static int weight_dt;
static int weight_sck;

static int sample_interval = 50;
static float weight_scale_factor = 741.0;
static int zero_offset = -809930;

// ==========================
// 数据缓存
// ==========================
#define MAX_FILTER_SAMPLES 32
static int samples[MAX_FILTER_SAMPLES];
// 实际使用数量
static int filter_samples = 10;

static int sample_index = 0;
static bool buffer_ready = false;

static int raw_value = 0;
static float current_weight = 0;
static unsigned int last_sample_time = 0;
static unsigned int weight_error_start = 0;

// ==========================
// 初始化
// ==========================
void weight_init()
{
    Serial.println();
    Serial.println("Weight init...");


    weight_dt =
        config_get_weight_dt();

    weight_sck =
        config_get_weight_sck();


    sample_interval =
        config_get_weight_sample_interval();


   weight_scale_factor =
        config_get_weight_scale();


    zero_offset =
        config_get_weight_zero_offset();


    filter_samples =
        config_get_weight_filter_samples();


    if(filter_samples > MAX_FILTER_SAMPLES)
    {
        filter_samples = MAX_FILTER_SAMPLES;
    }


    if(filter_samples < 3)
    {
        filter_samples = 3;
    }


    scale.begin(
        weight_dt,
        weight_sck
    );


    delay(100);
    Serial.print("zero_offset=");
    Serial.println(zero_offset);
    Serial.print("scale=");
    Serial.println(weight_scale_factor);


}

// ==========================
// 滤波计算
// 去最大最小
// 八点平均
// ==========================
static int weight_filter()
{
    int max_value = samples[0];
    int min_value = samples[0];
    int sum = 0;

    for(int i = 0; i < filter_samples; i++)
    {
        if(samples[i] > max_value)
            max_value = samples[i];
        if(samples[i] < min_value)
            min_value = samples[i];

        sum += samples[i];
    }

    sum -= max_value;
    sum -= min_value;

    return sum / (filter_samples-2);
}

// ==========================
// 任务
// ==========================

void weight_task()
{
    if(millis() - last_sample_time < sample_interval)
    {
        return;
    }
    last_sample_time = millis();

    if(!scale.is_ready())
    {
        if(weight_error_start == 0)
    {
        weight_error_start = millis();
    }


    if(
        millis()-weight_error_start
        >
        5000
    )
    {

        event_push(
            EVENT_WEIGHT_ERROR
        );


        weight_error_start =
            millis();

    }

    return;
}
else
{
    weight_error_start = 0;
}
    

    raw_value = scale.read();
    samples[sample_index] = raw_value;
    sample_index++;

    if(sample_index >= filter_samples)
    {
        sample_index = 0;
        buffer_ready = true;
    }

    if(buffer_ready)
    {
        int filtered = weight_filter();
        float gram = (filtered - zero_offset) / weight_scale_factor;
        current_weight = gram;

        state_set_float(STATE_WEIGHT_VALUE, gram);


    }
}

// ==========================
// 原始值
// ==========================
int weight_get_raw()
{
    return raw_value;
}

// ==========================
// 设置当前为空载零点
// ==========================
void weight_zero_calibrate()
{
    int sum = 0;

    int count = 0;


    while(count < 20)
    {
        if(scale.is_ready())
        {
            sum += scale.read();
            count++;
        }

        delay(50);
    }


    zero_offset = 
        sum / 20;


    config_set_weight_zero_offset(
        zero_offset
    );


    config_save();


    Serial.println(
        "Weight zero calibrated"
    );
}