#pragma once
#include <Arduino.h>

struct BatteryStatus {
    bool connected;     // 是否接入了电池 (Vbat >= 2.5V)
    float voltage;      // 电池电压 (单位: V，保留2位小数)
    uint8_t level;      // 4格电量档位 (0, 1, 2, 3, 4)
    uint8_t percent;    // 电量百分比 (0 ~ 100%)
};

/**
 * 初始化电池测量引脚与 ADC
 */
void batteryInit();

/**
 * 采样并读取当前电池状态（多重滤波与档位计算）
 */
BatteryStatus readBatteryStatus();
