#include <Arduino.h>
#include <WiFi.h>
#include <main.h>
#include <wifi_conf.h>
#include <wifi_structures.h>
#include <ArduinoJson.h>
#include <new>
#include "api_all.h"
#include "wifi_drv.h"
#include "utils.h"


#define SCAN_TIMEOUT_MS 15000  // 一般10秒内就好了
#define MAX_SCAN_APS 96


static void printBssid(const uint8_t* bssid) {
    for (int i=0;i<6;i++){if(bssid[i]<0x10)Serial.print("0");Serial.print(bssid[i],HEX);if(i<5)Serial.print(":");}
}

typedef struct {
    rtw_ssid_t ssid;
    rtw_mac_t  bssid;
    short      rssi;
    uint       channel;
    rtw_security_t security;
} ScanAP;

typedef enum {
    SCAN_IDLE = 0,
    SCAN_RUNNING,
    SCAN_COMPLETED
} ScanState;

static ScanAP* g_scan_aps = nullptr;
static volatile int g_scan_count = 0;
static volatile ScanState g_scan_state = SCAN_IDLE;

static rtw_result_t apScanResultHandler(rtw_scan_handler_result_t* scan_result) {
    if (!scan_result->scan_complete) {
        if (g_scan_state != SCAN_RUNNING || !g_scan_aps || g_scan_count >= MAX_SCAN_APS) return RTW_SUCCESS;
        rtw_scan_result_t* r = &scan_result->ap_details;
        ScanAP& ap = g_scan_aps[g_scan_count];
        ap.ssid = r->SSID;
        ap.bssid = r->BSSID;
        ap.rssi = r->signal_strength;
        ap.channel = r->channel;
        ap.security = r->security;
        g_scan_count++;
    } else {
        g_scan_state = SCAN_COMPLETED;
    }
    return RTW_SUCCESS;
}

static const char* securityTypeToString(rtw_security_t security) {
    switch (security) {
        case RTW_SECURITY_OPEN:                 return "OPEN";
        case RTW_SECURITY_WEP_PSK:              return "WEP";
        case RTW_SECURITY_WEP_SHARED:           return "WEP-SHARED";
        case RTW_SECURITY_WPA_TKIP_PSK:         return "WPA-TKIP";
        case RTW_SECURITY_WPA_AES_PSK:          return "WPA-AES";
        case RTW_SECURITY_WPA_MIXED_PSK:        return "WPA-MIXED";
        case RTW_SECURITY_WPA2_AES_PSK:         return "WPA2-AES";
        case RTW_SECURITY_WPA2_TKIP_PSK:        return "WPA2-TKIP";
        case RTW_SECURITY_WPA2_MIXED_PSK:       return "WPA2-MIXED";
        case RTW_SECURITY_WPA_WPA2_TKIP_PSK:    return "WPA/WPA2-TKIP";
        case RTW_SECURITY_WPA_WPA2_AES_PSK:     return "WPA/WPA2-AES";
        case RTW_SECURITY_WPA_WPA2_MIXED_PSK:   return "WPA/WPA2";
        case RTW_SECURITY_WPA2_AES_CMAC:        return "WPA2-AES-CMAC";
        case RTW_SECURITY_WPA_TKIP_ENTERPRISE:  return "WPA-TKIP-ENT";
        case RTW_SECURITY_WPA_AES_ENTERPRISE:   return "WPA-AES-ENT";
        case RTW_SECURITY_WPA_MIXED_ENTERPRISE: return "WPA-MIXED-ENT";
        case RTW_SECURITY_WPA2_TKIP_ENTERPRISE: return "WPA2-TKIP-ENT";
        case RTW_SECURITY_WPA2_AES_ENTERPRISE:  return "WPA2-AES-ENT";
        case RTW_SECURITY_WPA2_MIXED_ENTERPRISE:return "WPA2-MIXED-ENT";
        case RTW_SECURITY_WPA_WPA2_TKIP_ENTERPRISE:  return "WPA/WPA2-TKIP-ENT";
        case RTW_SECURITY_WPA_WPA2_AES_ENTERPRISE:   return "WPA/WPA2-AES-ENT";
        case RTW_SECURITY_WPA_WPA2_MIXED_ENTERPRISE: return "WPA/WPA2-ENT";
        case RTW_SECURITY_WPS_OPEN:             return "WPS-OPEN";
        case RTW_SECURITY_WPS_SECURE:           return "WPS-SECURE";
        case RTW_SECURITY_WPA3_AES_PSK:         return "WPA3-SAE";
        case RTW_SECURITY_WPA2_WPA3_MIXED:      return "WPA2/WPA3";
        case RTW_SECURITY_WPA3_ENTERPRISE:      return "WPA3-ENT";
        case RTW_SECURITY_WPA3_OWE:             return "WPA3-OWE";
        default:                                return "UNKNOWN";
    }
}

void handleApScanApi(HttpClient& client) {
    // If a previous scan timed out but has now finished in the background, clean it up
    if (g_scan_state == SCAN_COMPLETED && g_scan_aps != nullptr) {
        delete[] g_scan_aps;
        g_scan_aps = nullptr;
        g_scan_state = SCAN_IDLE;
    } else if (g_scan_state == SCAN_RUNNING) {
        client.sendJsonFail("another scan is already running");
        return;
    }

    g_scan_aps = new (std::nothrow) ScanAP[MAX_SCAN_APS];
    if (!g_scan_aps) {
        client.sendJsonFail("out of memory");
        return;
    }

    g_scan_count = 0;
    g_scan_state = SCAN_RUNNING;

    Serial.println("[SNIFF] AP scan start");
    unsigned long start = millis();
    // 必须使用 _mcc 版本：普通 wifi_scan_networks 在STA+AP并发模式下 STA 链路未完整初始化，扫描结果为空
    // wifi_scan_networks_mcc() 是 SDK 专为并发模式设计的逐信道扫描版本，
    // 它逐个信道调用 wext_set_scan + 信道间插入 100ms 间隔让 AP 发送信标，
    // 保证扫描期间 AP 服务不中断且结果正确返回。
    if (wifi_scan_networks_mcc(apScanResultHandler, NULL) != RTW_SUCCESS) {
        g_scan_state = SCAN_IDLE;
        delete[] g_scan_aps;
        g_scan_aps = nullptr;
        client.sendJsonFail("scan failed to start");
        return;
    }
    while (g_scan_state == SCAN_RUNNING && millis() - start < SCAN_TIMEOUT_MS) {
        delay(30);
    }

    if (g_scan_state == SCAN_COMPLETED) {
        Serial.print("[SNIFF] Scan done, ap_count="); Serial.println(g_scan_count);
        JsonDocument doc;
        doc["success"] = true;
        doc["count"] = g_scan_count;
        JsonArray networks = doc["networks"].to<JsonArray>();
        for (int i = 0; i < g_scan_count; i++) {
            JsonObject net = networks.add<JsonObject>();
            uint8_t len = g_scan_aps[i].ssid.len;
            if (len > 32) len = 32;
            g_scan_aps[i].ssid.val[len] = 0;
            net["ssid"] = (const char*)g_scan_aps[i].ssid.val;

            char bssid_str[18];
            snprintf(bssid_str, sizeof(bssid_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                g_scan_aps[i].bssid.octet[0], g_scan_aps[i].bssid.octet[1], g_scan_aps[i].bssid.octet[2],
                g_scan_aps[i].bssid.octet[3], g_scan_aps[i].bssid.octet[4], g_scan_aps[i].bssid.octet[5]);
            net["bssid"] = bssid_str;

            net["rssi"] = g_scan_aps[i].rssi;
            net["channel"] = g_scan_aps[i].channel;
            net["security"] = securityTypeToString(g_scan_aps[i].security);
        }
        client.sendJson(doc);

        ScanAP* temp = g_scan_aps;
        g_scan_aps = nullptr;
        g_scan_state = SCAN_IDLE;
        delete[] temp;
    } else {
        // Timeout case: Do NOT free g_scan_aps or reset g_scan_state.
        // It stays as SCAN_RUNNING so that late callbacks can safely run.
        // It will be cleaned up on the next API call once it transitions to SCAN_COMPLETED.
        client.sendJsonFail("scan timeout");
    }
}

// ─── Device Scan (被动监听特定AP关联设备) ───

#define DEVICE_SCAN_TIMEOUT_MS 25000
#define MAX_DISCOVERED_DEVICES 64

typedef struct {
    uint8_t mac[6];
    int packets_out; // Device -> Router (Uplink / Tx)
    int packets_in;  // Router -> Device (Downlink / Rx)
    int handshakes;  // Handshake packets count
    int8_t rssi;     // Client RSSI (from STA->AP uplink frames only)
} DiscoveredDevice;

typedef struct {
    bool ap_beacon_parsed;
    int8_t ap_rssi;  // Target AP latest RSSI
    uint64_t ap_timestamp;
    bool pmf_capable;
    bool pmf_required;
    DiscoveredDevice devices[MAX_DISCOVERED_DEVICES];
} DeviceScanSession;

static DeviceScanSession* g_scan_session = nullptr;
static volatile int g_device_count = 0;
static uint8_t g_target_bssid[6];

// rssi 参数仅在 is_uplink=true 时有意义（STA→AP 方向，反映客户端信号强度）
static void addDiscoveredDevice(const uint8_t* mac, bool is_uplink, bool is_handshake = false, int8_t rssi = 0) {
    if (!g_scan_session) return;
    if (mac[0] == 0xFF || (mac[0] & 0x01)) return;
    if (memcmp(mac, g_target_bssid, 6) == 0) return;
    for (int i = 0; i < g_device_count; i++) {
        if (memcmp(g_scan_session->devices[i].mac, mac, 6) == 0) {
            if (is_uplink) {
                g_scan_session->devices[i].packets_out++;
                // 更新 RSSI（取最新上行帧的值）
                if (rssi != 0) g_scan_session->devices[i].rssi = rssi;
            } else {
                g_scan_session->devices[i].packets_in++;
            }
            if (is_handshake) {
                g_scan_session->devices[i].handshakes++;
            }
            return;
        }
    }
    if (g_device_count >= MAX_DISCOVERED_DEVICES) return;
    memcpy(g_scan_session->devices[g_device_count].mac, mac, 6);
    if (is_uplink) {
        g_scan_session->devices[g_device_count].packets_out = 1;
        g_scan_session->devices[g_device_count].packets_in = 0;
        g_scan_session->devices[g_device_count].rssi = rssi;
    } else {
        g_scan_session->devices[g_device_count].packets_out = 0;
        g_scan_session->devices[g_device_count].packets_in = 1;
        g_scan_session->devices[g_device_count].rssi = 0;
    }
    g_scan_session->devices[g_device_count].handshakes = is_handshake ? 1 : 0;
    g_device_count++;
}

static void parseBeaconFrame(const unsigned char* buf, unsigned int len, DeviceScanSession* session) {
    // 已实现无线启动时间（根据时间戳解析）和pmf解析。
    // 用qbss load element还可以解析出客户端数量、信道利用率，但是用不到。
    if (!session) return;

    session->pmf_capable = false;
    session->pmf_required = false;

    if (len >= 24 + 8) {
        uint64_t ts = 0;
        memcpy(&ts, buf + 24, 8);
        session->ap_timestamp = ts;
    }
    int offset = 36;
    while (offset + 2 <= (int)len) {
        uint8_t ie_id = buf[offset];
        uint8_t ie_len = buf[offset + 1];
        if (offset + 2 + ie_len > (int)len) {
            break;
        }
        if (ie_id == 48) { // RSN IE
            int curr = offset + 2;
            int ie_end = offset + 2 + ie_len;
            // RSN structure:
            // Version (2 bytes)
            // Group Cipher Suite (4 bytes)
            // Pairwise Cipher Suite Count (2 bytes)
            // Pairwise Cipher Suite List (4 * Count)
            // AKM Suite Count (2 bytes)
            // AKM Suite List (4 * Count)
            // RSN Capabilities (2 bytes)
            if (curr + 6 <= ie_end) {
                curr += 2; // skip Version
                curr += 4; // skip Group Cipher Suite
                if (curr + 2 <= ie_end) {
                    uint16_t pairwise_count = buf[curr] | (buf[curr + 1] << 8);
                    curr += 2;
                    int pairwise_bytes = (int)pairwise_count * 4;
                    if (pairwise_bytes >= 0 && curr + pairwise_bytes <= ie_end) {
                        curr += pairwise_bytes;
                        if (curr + 2 <= ie_end) {
                            uint16_t akm_count = buf[curr] | (buf[curr + 1] << 8);
                            curr += 2;
                            int akm_bytes = (int)akm_count * 4;
                            if (akm_bytes >= 0 && curr + akm_bytes <= ie_end) {
                                curr += akm_bytes;
                                if (curr + 2 <= ie_end) {
                                    session->pmf_capable = (buf[curr] & 0x80) != 0;
                                    session->pmf_required = (buf[curr] & 0x40) != 0;
                                }
                            }
                        }
                    }
                }
            }
        }
        offset += 2 + ie_len;
    }
}

#define SNIFF_DEBUG

static void deviceSniffCallback(unsigned char* buf, unsigned int len, void* userdata) {
    // userdata 实际是 ieee80211_frame_info_t*，包含 rssi/data_rate 等信息
    // SDK 汇编里用 ldrb（无符号）写入 rssi 字段，需要 (int8_t) 强转修正符号
    int8_t rssi = 0;
    if (userdata) {
        rssi = (int8_t)((ieee80211_frame_info_t*)userdata)->rssi;
    }
    if (!buf || len < 22 || !g_scan_session) return;

    #ifdef SNIFF_DEBUG
    // ── DEBUG: 打印原始包前若干字节 ──
    static unsigned int debug_count = 0;
    static unsigned long debug_last_log = 0;
    debug_count++;
    if (debug_count <= 20 || millis() - debug_last_log > 10000) {
        debug_last_log = millis();
        Serial.print("[SNIFF_DEBUG] #"); Serial.print(debug_count);
        Serial.print(" len="); Serial.print(len);
        Serial.print(" hex=");
        unsigned int dump_len = (len > 48) ? 48 : len;
        for (unsigned int i = 0; i < dump_len; i++) {
            if (buf[i] < 0x10) Serial.print("0");
            Serial.print(buf[i], HEX);
            if ((i & 1) && i < dump_len - 1) Serial.print(" ");
        }
        Serial.println();

        uint16_t fc = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
        uint8_t ftype = (fc >> 2) & 0x3;
        uint8_t fsubtype = (fc >> 4) & 0xF;
        // 只打印看起来有效的帧控制(非全0, 非全1, type合理)
        if (fc != 0 && fc != 0xFFFF && ftype <= 3) {
            Serial.print("[SNIFF_DEBUG] frame dump:");
            Serial.print(" fc=0x"); if (fc < 0x1000) Serial.print("0");
            if (fc < 0x100) Serial.print("0");
            if (fc < 0x10) Serial.print("0");
            Serial.print(fc, HEX);
            Serial.print(" type="); Serial.print(ftype);
            Serial.print(" subtype="); Serial.println(fsubtype);
        }
    }
    #endif

    uint16_t fc = buf[0] | (buf[1] << 8);
    unsigned int type = (fc >> 2) & 0x03;
    unsigned int subtype = (fc >> 4) & 0x0F;
    bool toDS = (fc & (1 << 8)) != 0;
    bool fromDS = (fc & (1 << 9)) != 0;

    const uint8_t* addr1 = buf + 4;
    const uint8_t* addr2 = buf + 10;
    const uint8_t* addr3 = buf + 16;

    // Derive BSSID from frame control flags (TA & RA are always addr2 & addr1)
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

    // 过滤掉不属于目标AP的所有无线数据包
    if (memcmp(bssid, g_target_bssid, 6) != 0) {
        #ifdef SNIFF_DEBUG
        Serial.print("[SNIFF] BSSID mismatch: got=");
        printBssid(bssid);
        Serial.print(" target=");
        printBssid(g_target_bssid);
        Serial.print(" fc=0x"); Serial.print(fc, HEX);
        Serial.print(" toDS="); Serial.print(toDS);
        Serial.print(" fromDS="); Serial.println(fromDS);
        #endif
        return;
    }

    const uint8_t* ta = addr2;
    const uint8_t* ra = addr1;

    // ── 发送方为目标 AP 的处理（Beacon / Probe Resp / 下行数据等） ──
    if (memcmp(ta, g_target_bssid, 6) == 0) {
        if (rssi != 0) {
            g_scan_session->ap_rssi = rssi;
        }
        // Parse target AP's Beacon frame (type 0, subtype 8)
        // 反正是顺便的，返回一些有趣信息
        if (!g_scan_session->ap_beacon_parsed && type == 0 && subtype == 8) {
            parseBeaconFrame(buf, len, g_scan_session);
            g_scan_session->ap_beacon_parsed = true;
        }
    }

    bool is_eapol = false;
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
                is_eapol = true;
                Serial.print("[SNIFF] EAPOL Handshake Packet detected: len=");
                Serial.print(len);
                Serial.print(" TA="); printBssid(ta);
                Serial.print(" RA="); printBssid(ra);
                Serial.println();
            }
        }
    }

    bool ra_broadcast = (ra[0] == 0xFF && ra[1] == 0xFF && ra[2] == 0xFF &&
                         ra[3] == 0xFF && ra[4] == 0xFF && ra[5] == 0xFF);
    bool ra_multicast = (ra[0] & 0x01);

    // AP bridging broadcast from wired side → skip entirely
    if ((ra_broadcast || ra_multicast) && memcmp(ta, g_target_bssid, 6) == 0) {
        return;
    }

    if (toDS && !fromDS) {
        // STA→AP: TA=station, RA=BSSID
        // 只有上行帧(STA→AP)的RSSI反映客户端信号强度
        if (memcmp(ta, g_target_bssid, 6) != 0) {
            #ifdef SNIFF_DEBUG
            Serial.print("[SNIFF] ADD TA (sta→ap)=");
            printBssid(ta);
            Serial.print(" rssi="); Serial.print(rssi);
            Serial.println();
            #endif
            addDiscoveredDevice(ta, true, is_eapol, rssi);
        }
    } else if (!toDS && fromDS) {
        // AP→STA: TA=BSSID, RA=station
        if (!ra_broadcast && memcmp(ra, g_target_bssid, 6) != 0) {
            #ifdef SNIFF_DEBUG
            Serial.print("[SNIFF] ADD RA (ap→sta)=");
            printBssid(ra);
            Serial.println();
            #endif
            addDiscoveredDevice(ra, false, is_eapol);
        }
    } else if (!toDS && !fromDS) {
        // Direct/management: TA=source, RA=destination
        if (memcmp(ta, g_target_bssid, 6) != 0) {
            #ifdef SNIFF_DEBUG
            Serial.print("[SNIFF] ADD TA (direct)=");
            printBssid(ta);
            Serial.println();
            #endif
            addDiscoveredDevice(ta, true, is_eapol);
        }
        if (!ra_broadcast && !ra_multicast && memcmp(ra, g_target_bssid, 6) != 0) {
            #ifdef SNIFF_DEBUG
            Serial.print("[SNIFF] ADD RA (direct)=");
            printBssid(ra);
            Serial.println();
            #endif
            addDiscoveredDevice(ra, false, is_eapol);
        }
    }
}

void handleDeviceScanApi(HttpClient& client) {
    if (g_scan_session != nullptr) {
        client.sendJsonFail("another scan is already running");
        return;
    }
    // 虽然可以同时监听同一个信道的所有客户端，但是ap的频道可能比较分散。还是设计成针对每个ap扫描设备
    String bssidStr = client.queryParam("bssid");
    String chStr = client.queryParam("channel");
    String durationStr = client.queryParam("duration");

    if (bssidStr.length() == 0 || chStr.length() == 0) {
        client.sendJsonFail("missing bssid or channel");
        return;
    }
    if (!parseMac(bssidStr, g_target_bssid)) {
        client.sendJsonFail("invalid bssid");
        return;
    }
    int target_channel = chStr.toInt();

    uint32_t scan_timeout_ms = DEVICE_SCAN_TIMEOUT_MS;

    if (durationStr.length() > 0) {
        int d = durationStr.toInt();
        scan_timeout_ms = (d < 0) ? UINT32_MAX : (unsigned int)d * 1000;
    }

    Serial.print("[SNIFF] targetAp="); Serial.print(bssidStr);
    Serial.print(" channel="); Serial.print(target_channel);
    Serial.print(" duration=");
    if (scan_timeout_ms == UINT32_MAX) {
        Serial.println("infinite");
    } else {
        Serial.print(scan_timeout_ms / 1000);
        Serial.println("s");
    }

    g_scan_session = new (std::nothrow) DeviceScanSession{};
    if (!g_scan_session) {
        client.sendJsonFail("out of memory");
        return;
    }

    g_device_count = 0;
    // WiFi.disablePowerSave();

    // 必须切换到目标信道监听。通知客户端一起切换
    if (target_channel != ap_channel) {
        if (wifi_ap_switch_chl_and_inform(target_channel) != RTW_SUCCESS) {
            client.sendJsonFail("failed to switch channel");
            delete g_scan_session;
            g_scan_session = nullptr;
            return;
        }
        wext_set_channel(WLAN0_NAME, target_channel);  // 必须，上面的调用不够
        ap_channel = target_channel;
        delay(100);
    }

    // 启用混杂模式
    if (wifi_set_promisc(RTW_PROMISC_ENABLE_2, deviceSniffCallback, 1) != RTW_SUCCESS) {
        client.sendJsonFail("failed to set promisc mode");
        delete g_scan_session;
        g_scan_session = nullptr;
        return;
    }

    // 发送 SSE 头部
    client.sendSseHeader();

    uint32_t start = millis();
    uint32_t last_send = 0;
    while ((millis() - start < scan_timeout_ms)) {
        // 如果客户端主动断开，提前退出
        if (!client.connected()) {
            Serial.println("[SNIFF] SSE client disconnected, aborting scan");
            break;
        }

        // 每秒推送一次当前扫描到的所有设备
        if (millis() - last_send >= 1000) {
            last_send = millis();

            JsonDocument doc;
            doc["success"] = true;
            doc["bssid"] = bssidStr;
            doc["channel"] = target_channel;
            doc["count"] = g_device_count;

            if (g_scan_session->ap_rssi != 0) {
                doc["ap_rssi"] = g_scan_session->ap_rssi;
            }

            if (g_scan_session->ap_beacon_parsed) {
                doc["ap_beacon_parsed"] = true;
                doc["ap_uptime"] = g_scan_session->ap_timestamp / 1000000ULL;
                doc["pmf_capable"] = g_scan_session->pmf_capable;
                doc["pmf_required"] = g_scan_session->pmf_required;
            } else {
                doc["ap_beacon_parsed"] = false;
            }

            JsonArray devices = doc["devices"].to<JsonArray>();
            for (int i = 0; i < g_device_count; i++) {
                JsonObject d = devices.add<JsonObject>();
                char mac[18];
                snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                    g_scan_session->devices[i].mac[0], g_scan_session->devices[i].mac[1], g_scan_session->devices[i].mac[2],
                    g_scan_session->devices[i].mac[3], g_scan_session->devices[i].mac[4], g_scan_session->devices[i].mac[5]);
                d["mac"] = mac;
                d["packets_out"] = g_scan_session->devices[i].packets_out;
                d["packets_in"] = g_scan_session->devices[i].packets_in;
                d["handshakes"] = g_scan_session->devices[i].handshakes;
                d["rssi"] = g_scan_session->devices[i].rssi;
            }

            client.sendSseData(doc);
        }
        delay(50);
    }

    // 发送 done 事件标识扫描完成
    if (client.connected()) {
        client.sendSseEvent("done", "{}");
    }

    wifi_set_promisc(RTW_PROMISC_DISABLE, NULL, 1);
    delay(50);  // 确保callback都执行完毕了

    Serial.print("[SNIFF] Scan done, device_count="); Serial.print(g_device_count);
    Serial.print(" ap_rssi="); Serial.println(g_scan_session->ap_rssi);
    for (int i = 0; i < g_device_count; i++) {
        Serial.print("[SNIFF]   device ");
        printBssid(g_scan_session->devices[i].mac);
        Serial.print(" tx_packets="); Serial.print(g_scan_session->devices[i].packets_out);
        Serial.print(" rx_packets="); Serial.print(g_scan_session->devices[i].packets_in);
        Serial.print(" handshakes="); Serial.print(g_scan_session->devices[i].handshakes);
        Serial.print(" rssi="); Serial.println(g_scan_session->devices[i].rssi);
    }
    DeviceScanSession* temp = g_scan_session;
    g_scan_session = nullptr;
    delete temp;
}
