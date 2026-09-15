#include <algorithm>
#include "battery.h"
#include <Arduino.h>
#include <PowerSave.h>

#define BATTERY_ADC_PIN         PB3
#define VOLTAGE_DIVIDER_RATIO   2.0f      // 1M + 1M 分压，比例为 2.0
#define ADC_REF_VOLTAGE_MV      3300.0f   // ADC 基准电压 3.3V
#define ADC_MAX_VAL             4095.0f   // 12-bit ADC 最大量程
#define SAMPLE_COUNT            16        // 滑动窗口样本容量（多点采样减少射频毛刺影响）
#define SAMPLE_INTERVAL_MS      1000      // 单次采样间隔（消除高阻抗影响，至少200ms以上让100nF电容有时间充电)
#define BATTERY_MIN_VALID_VOLT  2.50f     // 低于该值视为未接电池

static bool s_adc_initialized = false;
static BatteryStatus s_cached_status = {false, 0.0f, 0, 0};

// 滑动窗口环形队列
static uint32_t s_samples[SAMPLE_COUNT];
static uint8_t  s_sample_idx = 0;
static uint32_t s_last_sample_ms = 0;
static uint32_t s_last_protect_check_ms = 0;
static uint8_t  s_low_bat_count = 0;

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

// 从样本队列中进行中值滤波与极值剔除，计算当前电池状态
static BatteryStatus computeStatusFromSamples(const uint32_t raw_samples[SAMPLE_COUNT]) {
    uint32_t sorted[SAMPLE_COUNT];
    memcpy(sorted, raw_samples, sizeof(sorted));

    // 排序以剔除最高与最低极值
    std::sort(sorted, sorted + SAMPLE_COUNT);

    // 去掉前 3 个最小值和后 3 个最大值，取中间 10 个平均
    const int trim = 3;
    uint32_t sum = 0;
    int valid_count = SAMPLE_COUNT - (trim * 2);
    for (int i = trim; i < SAMPLE_COUNT - trim; i++) {
        sum += sorted[i];
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

void batteryInit() {
    if (!s_adc_initialized) {
        analogReadResolution(12);
        s_adc_initialized = true;

        // 冷启动初值快速填充，确保启动瞬间即可读到有效状态
        uint32_t init_raw = analogRead(BATTERY_ADC_PIN);
        for (int i = 0; i < SAMPLE_COUNT; i++) {
            s_samples[i] = init_raw;
        }
        s_sample_idx = 0;
        s_cached_status = computeStatusFromSamples(s_samples);
        s_last_sample_ms = millis();
        s_last_protect_check_ms = millis();
    }
}

BatteryStatus getCachedBatteryStatus() {
    if (!s_adc_initialized) {
        batteryInit();
    }
    return s_cached_status;
}

void batteryTick() {
    if (!s_adc_initialized) {
        batteryInit();
    }

    uint32_t now = millis();

    // 1. 滑动窗口采样：每 SAMPLE_INTERVAL_MS (500ms) 采 1 个点
    if (now - s_last_sample_ms >= SAMPLE_INTERVAL_MS) {
        s_last_sample_ms = now;

        s_samples[s_sample_idx] = analogRead(BATTERY_ADC_PIN);
        s_sample_idx = (s_sample_idx + 1) % SAMPLE_COUNT;

        // 实时刷新滑动窗口计算结果
        s_cached_status = computeStatusFromSamples(s_samples);
    }

    // 2. 低电量保护判定：每 BATTERY_CHECK_INTERVAL_SEC (30s) 评估一次
    if (now - s_last_protect_check_ms >= (uint32_t)BATTERY_CHECK_INTERVAL_SEC * 1000) {
        s_last_protect_check_ms = now;

        // 开机豁免期保护：开机后 BOOT_LOW_BAT_GRACE_SEC 秒内不触发低电休眠
        if (now < (uint32_t)BOOT_LOW_BAT_GRACE_SEC * 1000) {
            s_low_bat_count = 0;
            return;
        }

        // 电池未接检测：未接入电池（如纯 USB 供电）时绝不触发低电休眠
        if (!s_cached_status.connected) {
            s_low_bat_count = 0;
            return;
        }

        // 低电量防抖判定与深度睡眠触发
        if (s_cached_status.percent <= BATTERY_LOW_PERCENT_THRESHOLD) {
            s_low_bat_count++;
            Serial.print("[Battery] Low battery detected: ");
            Serial.print(s_cached_status.percent);
            Serial.print("% (");
            Serial.print(s_cached_status.voltage);
            Serial.print("V), debounce count: ");
            Serial.print(s_low_bat_count);
            Serial.print("/");
            Serial.println(BATTERY_LOW_DEBOUNCE_COUNT);

            if (s_low_bat_count >= BATTERY_LOW_DEBOUNCE_COUNT) {
                Serial.println("[Battery] Low battery threshold reached. Entering deep sleep to protect battery...");
                Serial.flush();
                delay(100);

                PowerSave.begin(DEEPSLEEP_MODE);
                PowerSave.enable();
            }
        } else {
            s_low_bat_count = 0;
        }
    }
}
