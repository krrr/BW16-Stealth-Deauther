#include <algorithm>
#include "battery.h"
#include <Arduino.h>

#define BATTERY_ADC_PIN         PB3
#define VOLTAGE_DIVIDER_RATIO   2.0f      // 1M + 1M 分压，比例为 2.0
#define ADC_REF_VOLTAGE_MV      3300.0f   // ADC 基准电压 3.3V
#define ADC_MAX_VAL             4095.0f   // 12-bit ADC 最大量程
#define SAMPLE_COUNT            16        // 采样次数
#define BATTERY_MIN_VALID_VOLT  2.50f     // 判定电池连接的最低有效电压 (低于 2.5V 视为未接电池/悬空)

static bool s_adc_initialized = false;

void batteryInit() {
    analogReadResolution(12);
    s_adc_initialized = true;
}

static uint8_t calculatePercent(float v) {
    if (v >= 4.20f) return 100;
    if (v <= 3.00f) return 0;

    float pct = 0.0f;
    if (v >= 4.00f) {
        pct = 80.0f + ((v - 4.00f) / 0.20f) * 20.0f;
    } else if (v >= 3.80f) {
        pct = 50.0f + ((v - 3.80f) / 0.20f) * 30.0f;
    } else if (v >= 3.65f) {
        pct = 25.0f + ((v - 3.65f) / 0.15f) * 25.0f;
    } else if (v >= 3.40f) {
        pct = 5.0f + ((v - 3.40f) / 0.25f) * 20.0f;
    } else {
        pct = ((v - 3.00f) / 0.40f) * 5.0f;
    }

    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return (uint8_t)(pct + 0.5f);
}

static uint8_t calculateLevel(float v) {
    if (v >= 3.95f) {
        return 4; // 4格 (75% ~ 100%)
    } else if (v >= 3.80f) {
        return 3; // 3格 (50% ~ 75%)
    } else if (v >= 3.65f) {
        return 2; // 2格 (25% ~ 50%)
    } else if (v >= 3.40f) {
        return 1; // 1格 (5% ~ 25%)
    } else {
        return 0; // 0格 (< 5% 低电量)
    }
}

BatteryStatus readBatteryStatus() {
    if (!s_adc_initialized) {
        batteryInit();
    }

    // 16 次采样以滤除 1M 欧大电阻高阻抗回路可能存在的噪声
    uint32_t samples[SAMPLE_COUNT];
    for (int i = 0; i < SAMPLE_COUNT; i++) {
        samples[i] = analogRead(BATTERY_ADC_PIN);
        delayMicroseconds(150);
    }

    // 排序以剔除最高与最低极值
    std::sort(samples, samples + SAMPLE_COUNT);

    // 去掉前 3 个最小值和后 3 个最大值，取中间 10 个平均
    const int trim = 3;
    uint32_t sum = 0;
    int valid_count = SAMPLE_COUNT - (trim * 2);
    for (int i = trim; i < SAMPLE_COUNT - trim; i++) {
        sum += samples[i];
    }
    float avg_adc = (float)sum / valid_count;

    // 计算 PB3 引脚电压 (mV) -> 电池实际电压 (V)
    float pin_mv = (avg_adc * ADC_REF_VOLTAGE_MV) / ADC_MAX_VAL;
    float vbat_v = (pin_mv * VOLTAGE_DIVIDER_RATIO) / 1000.0f;

    BatteryStatus status;
    // 低于阈值判定为未接电池或悬空
    if (vbat_v < BATTERY_MIN_VALID_VOLT) {
        status.connected = false;
        status.voltage = 0.0f;
        status.level = 0;
        status.percent = 0;
    } else {
        status.connected = true;
        // 四舍五入保留 2 位小数
        status.voltage = roundf(vbat_v * 100.0f) / 100.0f;
        status.level = calculateLevel(status.voltage);
        status.percent = calculatePercent(status.voltage);
    }

    return status;
}
