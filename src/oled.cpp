#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

#include "oled.h"
#include "config_manager.h"
#include "system_state.h"
#include "event_manager.h"
#include "oled_animation.h"

// 内部函数提前声明
static void oled_show_frame();

static void oled_restart();

static int frame_delay = 50;

static uint8_t oled_rotation_config = 0;
static const u8g2_cb_t* oled_rotation = U8G2_R0;

static int oled_speed = 100000;// I2C通信速度

static const u8g2_cb_t* oled_get_rotation(uint8_t rotation)
{

    switch(rotation)
    {

        case 1:
            return U8G2_R1;


        case 2:
            return U8G2_R2;


        case 3:
            return U8G2_R3;


        default:
            return U8G2_R0;

    }

}

// =======================
// 创建OLED对象，硬件I2C
// =======================

U8G2_SSD1315_128X64_NONAME_F_HW_I2C oled(
    U8G2_R0,
    U8X8_PIN_NONE
);


// =======================
// 动画参数
// =======================

static uint16_t currentFrame = 0;

static unsigned long lastFrameTime = 0;






// =======================
// OLED初始化
// =======================

void oled_init()
{

    Serial.println("OLED init...");
    oled_rotation_config =
    config_get_oled_rotation();
    oled_rotation =
    oled_get_rotation(
        oled_rotation_config
    );

    // 初始化ESP32硬件I2C
    Wire.begin(
        // ==========================
        // OLED I2C引脚配置
        config_get_oled_sda(),
        config_get_oled_scl()
    );
    
      Wire.setClock(
        oled_speed=
        config_get_oled_speed()
    );

      frame_delay =
        config_get_oled_frame_delay();
    //中间省略扫描OLED地址 0x3C / 0x3D的调试阶段
    // OLED初始化
    oled.begin();

    oled.setDisplayRotation(
        oled_rotation
    );


    Serial.println("OLED init OK");
    
}



// =======================
// OLED任务循环
// =======================

void oled_task()
{


    if(
        millis() - lastFrameTime
        >= frame_delay
    )
    {

        lastFrameTime = millis();

        oled_show_frame();

        currentFrame++;


        if(
            currentFrame >= ANIM_FRAME_COUNT
        )
        {
            currentFrame = 0;
        }

    }

}



// =======================
// 显示动画当前帧
// =======================

static void oled_show_frame()
{


    oled.clearBuffer();



    /*
       PROGMEM指针读取
       ESP32必须使用pgm_read_ptr
    */


    const uint8_t* frame_ptr =
        (const uint8_t*)
        pgm_read_ptr(
            &anim_table[currentFrame]
        );



    oled.drawXBMP(
        0,
        0,
        128,
        64,
        frame_ptr
    );



    oled.sendBuffer();

}



// =======================
// 清屏
// =======================

void oled_clear()
{

    oled.clearBuffer();

    oled.sendBuffer();

}


// =======================
// OLED事件处理
// =======================
// =======================
// OLED事件处理
// =======================

void oled_event_handler(
    EventMessage message
)
{

    SystemEvent event =
        message.event;


    switch(event)
    {

        case EVENT_WIFI_CONNECTED:

            Serial.println(
                "OLED wifi event"
            );

            break;


        case EVENT_WEIGHT_ERROR:

            Serial.println(
                "OLED weight error"
            );

            break;


        default:

            break;
    }

}

// =======================
// OLED重启
// =======================
static void oled_restart()
{

    oled.clearBuffer();

    oled.sendBuffer();


    oled_init();

}



// =======================
//事件队列
// =======================

void oled_event_init()
{

    event_subscribe(
        EVENT_CONFIG_CHANGED,
        oled_event_handler
    );


    event_subscribe(
        EVENT_NTP_SYNC_OK,
        oled_event_handler
    );


    event_subscribe(
        EVENT_WIFI_CONNECTED,
        oled_event_handler
    );


    event_subscribe(
        EVENT_WIFI_DISCONNECTED,
        oled_event_handler
    );


    Serial.println(
        "OLED event registered"
    );

}

