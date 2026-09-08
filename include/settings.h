#pragma once
#include <Arduino.h>

#define SETTINGS_MAGIC 0x574B3031  // "WK01"
#define FLASH_MEMORY_APP_BASE 0x00100000  // 2MB flash

#pragma pack(push, 4)
// 定时攻击目标（闪存持久化，加入时快照）
struct AttackTargetRecord {
    uint8_t mac[6];       // 目标客户端 MAC
    uint8_t bssid[6];     // 所属热点 BSSID
    uint8_t channel;      // 热点信道
    uint8_t pad;          // 对齐
    char ssid[33];        // 热点名（页面显示）
    int8_t rssi;          // 加入计划时 RSSI 快照
};

struct BeaconTimeRecord {
    uint8_t bssid[6];        // AP MAC address
    uint8_t channel;         // AP Wi-Fi channel
    uint8_t reserved;        // Alignment padding
    uint64_t saved_tsf;      // 64-bit Beacon TSF (microseconds)
    int64_t time_offset_ms;  // RealTime_ms - (TSF / 1000)
};

struct AppSettings {
    uint32_t magic;         // Magic number (0x574B3031)
    uint32_t struct_size;   // Struct size for forward/backward compatibility
    char ap_ssid[33];       // SoftAP SSID (max 32 chars + null terminator)
    char ap_pass[33];       // SoftAP Password (max 32 chars + null terminator)
    uint8_t ap_mac[6];      // Custom SoftAP MAC (6 bytes binary, all 0s = default)
    uint8_t enable_beacon_time_sync; // 0 = disabled (default), 1 = enabled
    uint8_t beacon_record_count;     // Number of valid beacon records (0..4)
    uint8_t reserved[2];             // Alignment padding to 4-byte boundary
    BeaconTimeRecord beacon_records[4];

    // === AP 占空比省电 & 时段调度 ===
    uint8_t ap_powersave_enable; // 0 = disabled (default), 1 = enabled
    uint8_t schedule_enable;     // 0 = disabled, 1 = enabled
    uint16_t duty_period_sec;    // 占空比周期（秒），默认 120
    uint16_t duty_on_sec;        // 每周期 AP 开启时长（秒），默认 10
    uint16_t client_hold_sec;    // 客户端全部断开后的保持宽限（秒），默认 15
    uint32_t schedule_hours_mask;// bit0..23 = 0..23 点是否允许开启 AP
    uint8_t reserved2[4];        // Alignment padding

    // === 定时攻击（前端"开始/停止"时一次性提交完整计划落盘） ===
    uint8_t attack_enabled;      // 1 = 攻击运行中（重启后自动恢复）
    uint8_t attack_type;         // 0 = deauth（预留扩展）
    uint8_t attack_ps_enable;    // 攻击省电模式（仅 ap_powersave_enable==1 时允许）
    uint8_t attack_reserved;     // Alignment padding
    uint8_t attack_target_count; // 有效目标数量
    uint8_t attack_reserved2[3]; // Alignment padding
    AttackTargetRecord attack_targets[16];
    uint32_t attack_interval_ms; // 攻击间隔（毫秒），支持小数秒，1000..3600000

    uint32_t checksum;      // CRC-32 checksum at the end
};
#pragma pack(pop)

static_assert(offsetof(AppSettings, checksum) + sizeof(uint32_t) == sizeof(AppSettings), "AppSettings checksum must be at the very end with no trailing padding!");
static_assert(sizeof(AppSettings) <= 6 * 4096, "AppSettings size exceeds 24KB limit!");

extern AppSettings g_appSettings;

uint32_t calculate_crc32(const uint8_t* data, size_t length);
void loadSettings();
bool saveSettings();
