#pragma once
#include <Arduino.h>

#define BATTERY_LOW_PERCENT_THRESHOLD   15        // 低电量自动休眠阈值 (<= 15%)
#define BOOT_LOW_BAT_GRACE_SEC          60        // 开机低电量豁免时长 (秒)
#define BATTERY_CHECK_INTERVAL_SEC      30        // 轮询检查周期 (秒)
#define BATTERY_LOW_DEBOUNCE_COUNT      3         // 连续低电确认次数 (防 WiFi 发包压降误判)

struct BatteryStatus {
    bool connected;     // 是否接入了电池 (Vbat >= 2.5V)
    float voltage;      // 电池电压 (单位: V，保留2位小数)
    uint8_t level;      // 4格电量档位 (0, 1, 2, 3, 4)
    uint8_t percent;    // 电量百分比 (0 ~ 100%)
};

/**
 * 初始化电池测量引脚与 ADC，并进行首次采样初始化缓存
 */
void batteryInit();

/**
 * 获取最近一次 batteryTick 轮询缓存的电池状态
 */
BatteryStatus getCachedBatteryStatus();

/**
 * 电池状态轮询与低电量自动深度睡眠保护
 */
void batteryTick();
