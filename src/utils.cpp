#include "utils.h"

/* 把冒号分隔的MAC字符串解析为6字节数组 */
bool parseMac(const String& str, uint8_t* mac) {
    // AmebaD (RTL8720DN) 的 newlib-nano sscanf 不支持 %02x 格式，
    // 调用会静默返回但不写入任何值，改用纯手动十六进制解析。
    const char* s = str.c_str();
    for (int i = 0; i < 6; i++) {
        if (*s == 0) return false;
        unsigned int v = 0;
        for (int j = 0; j < 2; j++) {
            char c = *s++;
            if (c >= '0' && c <= '9') v = (v << 4) | (c - '0');
            else if (c >= 'a' && c <= 'f') v = (v << 4) | (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = (v << 4) | (c - 'A' + 10);
            else return false;
        }
        mac[i] = (uint8_t)v;
        if (i < 5 && *s++ != ':') return false;
    }
    return true;
}

bool isMacValidUnicast(const uint8_t* mac) {
    bool allZero = true;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0) {
            allZero = false;
            break;
        }
    }
    if (allZero) return false;
    if (mac[0] & 0x01) return false;  // Multicast / Broadcast bit set
    return true;
}

String formatMac(const uint8_t* mac) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

void gpioIdlePinsInit() {
    // 闲置/悬空 GPIO 列表 (根据 BW16 模组 22-pin 表定义，排除 PB3、PA7、PA8)
    static const uint8_t s_idle_pins[] = {
        PA30, // Pin 2:  LP_PWM1
        PA27, // Pin 3:  SWD_DATA
        PA25, // Pin 4:  LP_I2C_SCL / LP_PWM4 / HSDM
        PA26, // Pin 5:  LP_I2C_SDA / LP_PWM5 / HSDP
        PB1,  // Pin 15: LP_UART_TXD
        PB2,  // Pin 16: LP_UART_RXD
        PA12, // Pin 18: LP_PWM0 / SPI1_MOSI
        PA13, // Pin 19: LP_PWM1 / SPI1_MISO
        PA14, // Pin 20: SPI1_CLK
        PA15  // Pin 21: SPI1_CS
    };

    for (size_t i = 0; i < sizeof(s_idle_pins) / sizeof(s_idle_pins[0]); i++) {
        pinMode(s_idle_pins[i], INPUT_PULLDOWN);
    }
}