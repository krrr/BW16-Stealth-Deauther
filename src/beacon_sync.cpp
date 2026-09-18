/**
 * @file beacon_sync.cpp
 * @brief 网络 Beacon 时间掉电自动恢复 (Beacon TSF RTC Time Auto-Restore)
 *
 * 【工作机制】
 * 1. 原理：
 *    利用 Wi-Fi 802.11 Beacon 帧头部的 64 位 TSF (Timing Synchronization Function) 硬件微秒计数器。
 *    只要路由器未重启，TSF 就会持续累加。以此作为外部时钟基准，无需外挂 RTC 电池或连外网。
 *
 * 2. 时间采样（主动同步）：
 *    - 用户在 Web 界面同步当前 Unix 时间戳。
 *    - 若开启了该功能，启动异步任务进入混杂模式（Promiscuous Mode）逐信道扫描周围 Beacon。
 *    - 按 RSSI 筛选信号最强且稳定的 4 个 AP，计算时间偏移。
 *    - 将各 AP 的 BSSID、信道、TSF 及 Offset 写入 Flash。
 *
 * 3. 时间恢复（掉电重启）：
 *    - 掉电重启后，在 setup() 阶段监听历史 AP 所在信道抓取 Beacon 帧。
 *    - 校验 TSF_new >= TSF_saved（若路由器重启过则忽略该 AP）。
 *    - 利用 TSF_new + Offset 反算出当前绝对时间并写入芯片内部 RTC。
 */

#include <algorithm>
#include "beacon_sync.h"
#include <Arduino.h>
#include <WiFi.h>
#include "main.h"
#include "settings.h"
#include "rtc_api.h"
#include "wifi_conf.h"
#include "wifi_structures.h"
#include "wifi_drv.h"
#include "utils.h"
#include "ap_powersave.h"

#define MAX_SAMPLED_BEACONS 32
#define BEACON_CHANNEL_DWELL_MS 120
#define BEACON_RESTORE_CHANNEL_DWELL_MS 300    // 恢复阶段单信道驻留时间 (约 3 个 Beacon 周期，提高抓取成功率)
#define BEACON_FLASH_WRITE_INTERVAL_SEC 86400  // 恢复时间与保存时间差小于该秒数则不写Flash，减少擦写

struct TempBeaconInfo {
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
    uint64_t tsf;
    uint32_t rtc_sec;
};

static TempBeaconInfo g_sampled_beacons[MAX_SAMPLED_BEACONS];
static volatile int g_sampled_count = 0;
static uint8_t g_current_scan_channel = 1;

static void captureCallback(unsigned char* buf, unsigned int len, void* userdata) {
    if (!buf || len < 24 + 8) return;

    uint16_t fc = buf[0] | (buf[1] << 8);
    uint8_t type = (fc >> 2) & 0x03;
    uint8_t subtype = (fc >> 4) & 0x0F;

    // Beacon frame is Management (type 0), Subtype 8
    if (type != 0 || subtype != 8) return;

    const uint8_t* bssid = buf + 10;

    // Ignore broadcast or all-zero MACs
    if (bssid[0] == 0xFF || (bssid[0] == 0 && bssid[1] == 0 && bssid[2] == 0)) return;

    uint64_t tsf = 0;
    memcpy(&tsf, buf + 24, 8);
    if (tsf == 0) return;

    int8_t rssi = 0;
    if (userdata) {
        rssi = (int8_t)((ieee80211_frame_info_t*)userdata)->rssi;
    }

    // Check if BSSID is already in sampled list
    for (int i = 0; i < g_sampled_count; i++) {
        if (memcmp(g_sampled_beacons[i].bssid, bssid, 6) == 0) {
            // Update with latest TSF, RTC time and stronger RSSI
            g_sampled_beacons[i].tsf = tsf;
            g_sampled_beacons[i].rtc_sec = rtc_read();
            if (rssi > g_sampled_beacons[i].rssi) {
                g_sampled_beacons[i].rssi = rssi;
            }
            return;
        }
    }

    if (g_sampled_count < MAX_SAMPLED_BEACONS) {
        memcpy(g_sampled_beacons[g_sampled_count].bssid, bssid, 6);
        g_sampled_beacons[g_sampled_count].channel = g_current_scan_channel;
        g_sampled_beacons[g_sampled_count].rssi = rssi;
        g_sampled_beacons[g_sampled_count].tsf = tsf;
        g_sampled_beacons[g_sampled_count].rtc_sec = rtc_read();
        g_sampled_count++;
    }
}

int captureAndSaveBeaconRecords() {
    Serial.println("[BeaconSync] Starting Beacon capture...");
    g_sampled_count = 0;

    int orig_channel = ap_channel;
    wifi_set_promisc(RTW_PROMISC_ENABLE_2, captureCallback, 1);

    // Hop across channels 1 to 13 (2.4GHz) for BEACON_CHANNEL_DWELL_MS each (stop early if max sampled beacons reached)
    for (uint8_t ch = 1; ch <= 13; ch++) {
        if (g_sampled_count >= MAX_SAMPLED_BEACONS) {
            break;
        }
        g_current_scan_channel = ch;
        wext_set_channel(WLAN0_NAME, ch);
        for (int ms = 0; ms < BEACON_CHANNEL_DWELL_MS; ms += 10) {
            delay(10);
            if (g_sampled_count >= MAX_SAMPLED_BEACONS) {
                break;
            }
        }
    }

    wifi_set_promisc(RTW_PROMISC_DISABLE, NULL, 1);
    wext_set_channel(WLAN0_NAME, orig_channel);

    Serial.print("[BeaconSync] Captured Beacons from ");
    Serial.print(g_sampled_count);
    Serial.println(" unique APs.");

    if (g_sampled_count == 0) return 0;

    // Sort by RSSI descending (strongest first) using std::sort
    // 每个c++模板sort都会占用700B左右的flash，但是flash空间充足无所谓
    std::sort(g_sampled_beacons, g_sampled_beacons + g_sampled_count, [](const TempBeaconInfo& a, const TempBeaconInfo& b) {
        return a.rssi > b.rssi;
    });

    int record_count = min(g_sampled_count, MAX_BEACON_TIME_RECORD);

    for (int i = 0; i < record_count; i++) {
        memcpy(g_appSettings.beacon_records[i].bssid, g_sampled_beacons[i].bssid, 6);
        g_appSettings.beacon_records[i].channel = g_sampled_beacons[i].channel;
        g_appSettings.beacon_records[i].saved_tsf = g_sampled_beacons[i].tsf;

        uint32_t beacon_rtc_sec = g_sampled_beacons[i].rtc_sec;
        int64_t beacon_rtc_ms = (int64_t)beacon_rtc_sec * 1000LL;
        int64_t tsf_ms = (int64_t)(g_sampled_beacons[i].tsf / 1000ULL);
        g_appSettings.beacon_records[i].time_offset_ms = beacon_rtc_ms - tsf_ms;

        Serial.print("[BeaconSync] Saved AP #");
        Serial.print(i + 1);
        Serial.print(": MAC=");
        Serial.print(formatMac(g_sampled_beacons[i].bssid));
        Serial.print(" CH=");
        Serial.print(g_sampled_beacons[i].channel);
        Serial.print(" RSSI=");
        Serial.print(g_sampled_beacons[i].rssi);
        Serial.print(" TSF=");
        Serial.println((uint32_t)(g_sampled_beacons[i].tsf / 1000000ULL));
    }

    g_appSettings.beacon_record_count = record_count;
    saveSettings();

    return record_count;
}

static volatile bool g_beacon_capture_running = false;

static void beaconCaptureTask(void* param) {
    (void)param;
    captureAndSaveBeaconRecords();
    g_beacon_capture_running = false;
    apPowerSaveRelease();  // 释放射频占用：允许省电调度继续
    vTaskDelete(NULL);
}

void startAsyncBeaconCapture() {
    if (g_beacon_capture_running) {
        Serial.println("[BeaconSync] Capture task already running. Skipping.");
        return;
    }
    g_beacon_capture_running = true;
    if (xTaskCreate(beaconCaptureTask, "beacon_cap", 1024, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        Serial.println("[BeaconSync] Failed to create beacon capture task!");
        g_beacon_capture_running = false;
    } else {
        apPowerSaveHold();  // 抓取期间占用射频，禁止省电调度挂起 softAP
    }
}


struct RestoreMatch {
    uint8_t record_idx;
    uint64_t new_tsf;
    int64_t calculated_unix_sec;
};

static RestoreMatch g_matches[4];
static volatile int g_match_count = 0;

static void restoreCallback(unsigned char* buf, unsigned int len, void* userdata) {
    (void)userdata;
    if (!buf || len < 24 + 8) return;

    uint16_t fc = buf[0] | (buf[1] << 8);
    uint8_t type = (fc >> 2) & 0x03;
    uint8_t subtype = (fc >> 4) & 0x0F;

    if (type != 0 || subtype != 8) return;

    const uint8_t* bssid = buf + 10;

    for (int i = 0; i < g_appSettings.beacon_record_count; i++) {
        if (memcmp(g_appSettings.beacon_records[i].bssid, bssid, 6) == 0) {
            uint64_t new_tsf = 0;
            memcpy(&new_tsf, buf + 24, 8);

            // TSF must be >= saved TSF (reject if router restarted and TSF reset)
            if (new_tsf >= g_appSettings.beacon_records[i].saved_tsf) {
                // Check if we already matched this record
                bool already_matched = false;
                for (int m = 0; m < g_match_count; m++) {
                    if (g_matches[m].record_idx == i) {
                        g_matches[m].new_tsf = new_tsf;
                        int64_t calc_ms = (int64_t)(new_tsf / 1000ULL) + g_appSettings.beacon_records[i].time_offset_ms;
                        g_matches[m].calculated_unix_sec = calc_ms / 1000LL;
                        already_matched = true;
                        break;
                    }
                }
                if (!already_matched && g_match_count < 4) {
                    g_matches[g_match_count].record_idx = i;
                    g_matches[g_match_count].new_tsf = new_tsf;
                    int64_t calc_ms = (int64_t)(new_tsf / 1000ULL) + g_appSettings.beacon_records[i].time_offset_ms;
                    g_matches[g_match_count].calculated_unix_sec = calc_ms / 1000LL;
                    g_match_count++;
                }
            } else {
                Serial.print("[BeaconSync] Router ");
                Serial.print(formatMac(bssid));
                Serial.println(" TSF reset detected (rebooted). Ignoring.");
            }
        }
    }
}

bool tryRestoreRtcFromBeacons() {
    if (g_appSettings.enable_beacon_time_sync != 1 || g_appSettings.beacon_record_count == 0) {
        return false;
    }

    Serial.println("[BeaconSync] Attempting RTC restore from saved Beacons...");
    g_match_count = 0;


    // Extract unique channels from saved records
    uint8_t unique_channels[4];
    int unique_channel_count = 0;
    for (int i = 0; i < g_appSettings.beacon_record_count; i++) {
        uint8_t target_ch = g_appSettings.beacon_records[i].channel;
        if (target_ch >= 1 && target_ch <= 14) {
            bool already_added = false;
            for (int c = 0; c < unique_channel_count; c++) {
                if (unique_channels[c] == target_ch) {
                    already_added = true;
                    break;
                }
            }
            if (!already_added) {
                unique_channels[unique_channel_count++] = target_ch;
            }
        }
    }

    int orig_channel = ap_channel;
    wifi_set_promisc(RTW_PROMISC_ENABLE_2, restoreCallback, 1);

    // Hop through unique channels to collect matches from all saved APs
    for (int i = 0; i < unique_channel_count; i++) {
        if (g_match_count >= g_appSettings.beacon_record_count) {
            break;
        }
        wext_set_channel(WLAN0_NAME, unique_channels[i]);
        for (int ms = 0; ms < BEACON_RESTORE_CHANNEL_DWELL_MS; ms += 10) {
            delay(10);
            if (g_match_count >= g_appSettings.beacon_record_count) {
                break;
            }
        }
    }

    wifi_set_promisc(RTW_PROMISC_DISABLE, NULL, 1);
    wext_set_channel(WLAN0_NAME, orig_channel);

    if (g_match_count == 0) {
        Serial.println("[BeaconSync] No matching Beacon found for saved APs.");
        return false;
    }

    // Sort matches ascending by calculated timestamp and select the median candidate
    std::sort(g_matches, g_matches + g_match_count, [](const RestoreMatch& a, const RestoreMatch& b) {
        return a.calculated_unix_sec < b.calculated_unix_sec;
    });
    uint32_t target_sec = (uint32_t)g_matches[g_match_count / 2].calculated_unix_sec;

    rtc_init();
    rtc_write(target_sec);

    // Update saved_tsf in Flash for matched APs only if restored time is significantly newer
    // to reduce Flash wear during development and frequent reboots
    bool need_flash_update = false;
    for (int m = 0; m < g_match_count; m++) {
        uint8_t idx = g_matches[m].record_idx;
        uint64_t diff_us = g_matches[m].new_tsf - g_appSettings.beacon_records[idx].saved_tsf;
        if (diff_us >= (uint64_t)BEACON_FLASH_WRITE_INTERVAL_SEC * 1000000ULL) {
            need_flash_update = true;
            break;
        }
    }

    if (need_flash_update) {
        for (int m = 0; m < g_match_count; m++) {
            uint8_t idx = g_matches[m].record_idx;
            g_appSettings.beacon_records[idx].saved_tsf = g_matches[m].new_tsf;
        }
        saveSettings();
        Serial.println("[BeaconSync] Updated saved TSF to Flash.");
    } else {
        Serial.println("[BeaconSync] Restored time is close to saved time. Skipping Flash write.");
    }

    Serial.print("[BeaconSync] RTC restored successfully to Unix timestamp: ");
    Serial.println(target_sec);

    return true;
}
