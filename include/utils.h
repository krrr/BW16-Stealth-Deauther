#pragma once
#include <Arduino.h>

bool parseMac(const String& str, uint8_t* mac);
bool isMacValidUnicast(const uint8_t* mac);
String formatMac(const uint8_t* mac);

/**
 * @brief 将所有闲置/悬空的通用 GPIO 引脚统一配置为确定的内部弱下拉输入状态 (INPUT_PULLDOWN)，
 *        彻底杜绝 CMOS 输入缓冲悬空引起的上下管微导通漏电。
 * 
 * 引脚覆盖说明（基于 Ai-Thinker BW16 模组 22-pin 管脚功能定义表）：
 *  - 排除 PB3 (Pin 17): 电池电压 ADC 采样 (由 batteryInit() 独立配置为纯模拟输入并关闭上下拉)
 *  - 排除 PA7 (Pin 7), PA8 (Pin 6): 串口 Log 及固件烧录接口 (UART_LOG_TXD / UART_LOG_RXD)
 *  - 纳入闲置 GPIO (共10个): PA30, PA27, PA25, PA26, PB1, PB2, PA12, PA13, PA14, PA15
 */
void gpioIdlePinsInit();