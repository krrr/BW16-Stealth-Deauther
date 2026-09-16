#include "settings.h"
#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "flash_api.h"
#include "cmsis_os.h"

#ifdef __cplusplus
}
#endif

AppSettings g_appSettings;
flash_t flash_obj;  // 并没有什么用，sdk的函数需要

// Standard CRC-32 (IEEE 802.3) implementation
uint32_t calculate_crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

static void setDefaultSettings() {
    memset(&g_appSettings, 0, sizeof(AppSettings));
    g_appSettings.magic = SETTINGS_MAGIC;
    g_appSettings.struct_size = sizeof(AppSettings);
    strncpy(g_appSettings.ap_ssid, "DIRECT-87-SD", 32);
    strncpy(g_appSettings.ap_pass, "temp3333", 32);
    // AP 占空比省电默认关闭，周期 2 分钟、开启 10 秒、断开保持 15 秒
    g_appSettings.ap_powersave_enable = 0;
    g_appSettings.duty_period_sec = 120;
    g_appSettings.duty_on_sec = 10;
    g_appSettings.client_hold_sec = 15;
    g_appSettings.schedule_enable = 0;
    g_appSettings.schedule_hours_mask = 0;
    // 定时攻击默认关闭，间隔 5 秒，攻击省电模式关闭（与 ap_powersave_enable 联动校验）
    g_appSettings.attack_enabled = 0;
    g_appSettings.attack_type = 0; // DEAUTH
    g_appSettings.attack_ps_enable = 0;
    g_appSettings.attack_interval_ms = 5000;  // 5 秒
    g_appSettings.attack_target_count = 0;
}

void loadSettings() {
    uint32_t header[2];
    flash_stream_read(&flash_obj, FLASH_MEMORY_APP_BASE, sizeof(header), (uint8_t*)header);

    if (header[0] != SETTINGS_MAGIC) {
        Serial.println("[Settings] Flash magic mismatch or unformatted flash. Using default settings.");
        setDefaultSettings();
        return;
    }
    uint32_t stored_size = header[1];
    if (stored_size < 8 || stored_size > sizeof(AppSettings)) { // magic (4) + struct_size (4)
        Serial.println("[Settings] Invalid struct_size stored. Using default settings.");
        setDefaultSettings();
        return;
    }

    uint8_t buffer[sizeof(AppSettings)];
    flash_stream_read(&flash_obj, FLASH_MEMORY_APP_BASE, stored_size, buffer);

    size_t stored_checksum_offset = stored_size - sizeof(uint32_t);
    uint32_t stored_checksum;
    memcpy(&stored_checksum, &buffer[stored_checksum_offset], sizeof(uint32_t));

    uint32_t computed_crc = calculate_crc32(buffer, stored_checksum_offset);
    if (computed_crc != stored_checksum) {
        Serial.print("[Settings] Checksum mismatch! Stored: 0x");
        Serial.print(stored_checksum, HEX);
        Serial.print(", Calculated: 0x");
        Serial.println(computed_crc, HEX);
        Serial.println("[Settings] Falling back to default settings.");
        setDefaultSettings();
        return;
    }

    // 校验通过：先全量填充当前默认值，确保新新增字段有正确的默认值
    setDefaultSettings();

    // 仅复制 Flash 中存储的旧 Payload（排除 Flash 里存的旧 CRC）
    memcpy(&g_appSettings, buffer, stored_checksum_offset);

    // 不需要在这里更新struct_size和crc，保存时候再更新

    Serial.println("[Settings] Config loaded successfully from flash");
}

bool saveSettings() {
    g_appSettings.magic = SETTINGS_MAGIC;
    g_appSettings.struct_size = sizeof(AppSettings);

    size_t checksum_offset = offsetof(AppSettings, checksum);
    g_appSettings.checksum = calculate_crc32((uint8_t*) &g_appSettings, checksum_offset);

    flash_erase_sector(&flash_obj, FLASH_MEMORY_APP_BASE);
    flash_stream_write(&flash_obj, FLASH_MEMORY_APP_BASE, sizeof(AppSettings), (uint8_t*) &g_appSettings);  // 不用管返回值，根本就是无效的

    Serial.println("[Settings] Config saved to flash successfully.");
    return true;
}
