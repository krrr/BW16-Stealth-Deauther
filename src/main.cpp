#include <main.h>
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "wifi_constants.h"
#include "wifi_conf.h"
#include "webpage.h"
#include "favicon.h"
#include "utils.h"
#include "api_all.h"
#include "settings.h"
#include "beacon_sync.h"
#include "ap_powersave.h"
#include "attacker.h"
#include "wifi_drv.h"
#include "lwip_netconf.h"
#include "http/HttpServer.h"
#include "battery.h"

int ap_channel = 1;

HttpServer server(80);

// === Route Handlers ===

static void handleRoot(HttpClient& client) {
    client.sendHtml(webpage_gz, webpage_gz_len, true);
}

static void handleFavicon(HttpClient& client) {
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: image/x-icon");
    client.print("Content-Length: ");
    client.println(favicon_ico_len);
    client.println("Connection: close");
    client.println();
    client.write((const uint8_t*)favicon_ico, favicon_ico_len);
}

static void handleNotFound(HttpClient& client) {
    client.sendNotFound();
}



// === Routing ===

static constexpr uint32_t djb2_hash(const char* s, uint32_t h = 5381) {
    return *s ? djb2_hash(s + 1, ((h << 5) + h) + (unsigned char)*s) : h;
}

static constexpr uint32_t HASH_ROOT               = djb2_hash("/");
static constexpr uint32_t HASH_INDEX_HTML         = djb2_hash("/index.html");
static constexpr uint32_t HASH_FAVICON            = djb2_hash("/favicon.ico");
static constexpr uint32_t HASH_API_SCAN           = djb2_hash("/api/scan");
static constexpr uint32_t HASH_API_SCAN_DEVICES   = djb2_hash("/api/scan-devices");
static constexpr uint32_t HASH_API_STATUS         = djb2_hash("/api/status");
static constexpr uint32_t HASH_API_SET_TIME       = djb2_hash("/api/set-time");
static constexpr uint32_t HASH_API_DEAUTH         = djb2_hash("/api/attack/test-deauth");
static constexpr uint32_t HASH_API_CHANGE_CHANNEL = djb2_hash("/api/change-channel");
static constexpr uint32_t HASH_API_OTA            = djb2_hash("/api/ota");
static constexpr uint32_t HASH_API_REBOOT         = djb2_hash("/api/reboot");
static constexpr uint32_t HASH_API_DEEPSLEEP      = djb2_hash("/api/deepsleep");
static constexpr uint32_t HASH_API_SETTINGS       = djb2_hash("/api/settings");
static constexpr uint32_t HASH_API_ATTACK         = djb2_hash("/api/attack");
static constexpr uint32_t HASH_API_ATTACK_PLAN    = djb2_hash("/api/attack/plan");

static void dispatchRequest(HttpClient& client) {
    String path = client.path();

    switch (djb2_hash(path.c_str())) {
        case HASH_ROOT:
        case HASH_INDEX_HTML:
            handleRoot(client);
            break;
        case HASH_FAVICON:
            handleFavicon(client);
            break;
        case HASH_API_SCAN:
            handleApScanApi(client);
            break;
        case HASH_API_SCAN_DEVICES:
            handleDeviceScanApi(client);
            break;
        case HASH_API_STATUS:
            handleStatusApi(client);
            break;
        case HASH_API_SET_TIME:
            handleSetTimeApi(client);
            break;
        case HASH_API_DEAUTH:
            handleTestDeauthApi(client);
            break;
        case HASH_API_CHANGE_CHANNEL:
            handleChangeChannelApi(client);
            break;
        case HASH_API_OTA:
            handleOtaApi(client);
            break;
        case HASH_API_REBOOT:
            handleRebootApi(client);
            break;
        case HASH_API_DEEPSLEEP:
            handleDeepSleepApi(client);
            break;
        case HASH_API_SETTINGS:
            handleSettingsApi(client);
            break;
        case HASH_API_ATTACK:
            handleAttackStatusApi(client);
            break;
        case HASH_API_ATTACK_PLAN:
            handleAttackPlanApi(client);
            break;
        default:
            handleNotFound(client);
            break;
    }
}

void startAP(const char* ssid, const char* password, int channel) {
    // 设置AP热点
    // 本单片机有两个虚拟接口，WLAN0和WLAN1，分别用于STA和AP。但其实共享同一个 PHY，频道切换是联动的
    // 虽然不连任何AP，但是也要开启AP+STA并发，不然混杂模式抓包会干扰AP
    WiFi.enableConcurrent();
    IPAddress ip(192, 168, 4, 1);
    IPAddress gw(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.config(ip, gw, gw, subnet);

    WiFiDrv::wifiDriverInit();  // 本来apbegin里会调用，但是wifi_change_mac_address_from_ram需要先初始化

    if (isMacValidUnicast(g_appSettings.ap_mac)) {
        wifi_change_mac_address_from_ram(WLAN1_IDX, g_appSettings.ap_mac);
        // 同步 LwIP 协议栈 hwaddr
        memcpy(xnetif[1].hwaddr, g_appSettings.ap_mac, 6);
        Serial.print("[INFO] Custom SoftAP MAC applied: ");
        Serial.println(formatMac(g_appSettings.ap_mac));
    }

    char chan_char[4];
    itoa(channel, chan_char, 10);
    WiFi.apbegin((char*)ssid, (char*)password, chan_char, FALSE);
}

void setup() {
    // 将所有闲置/悬空 GPIO 配置为内部弱下拉，杜绝 I/O 漏电
    gpioIdlePinsInit();

    Serial.begin(115200);
    delay(1000);
    Serial.println("\nBW16 Stealth Deauther init ...");

    loadSettings();

    startAP(g_appSettings.ap_ssid, g_appSettings.ap_pass, ap_channel);

    Serial.println("[INFO] SoftAP started successfully. ");
    Serial.print("SSID: ");
    Serial.print(g_appSettings.ap_ssid);
    Serial.print(", IP: ");
    Serial.println(WiFi.localIP(WLAN1_IDX));

    tryRestoreRtcFromBeacons();

    server.begin();

    // 初始化 powersave（IPS+LPS）：softAP 挂起期间驱动自动关 RF，
    // 配合 FreeRTOS tickless 让主 CPU 进入浅睡眠
    apPowerSaveInit();

    // 恢复闪存中的定时攻击计划
    attackerInit();

    // 初始化电池 ADC 与首次采样缓存
    batteryInit();
}

void loop() {
    HttpClient client = server.available();
    if (client) {
        if (client.parseRequest()) {
            dispatchRequest(client);
        }
        client.stop();
    }

    // 定时攻击引擎（到期轮询发包）
    attackerTick();

    // 占空比省电状态机（睡眠期间内部会分块阻塞等待唤醒）
    apPowerSaveTick();

    // 电池状态轮询与低电量自动深度睡眠保护
    batteryTick();

    // 让出 CPU：配合 tickless 在空闲时进入浅睡眠，同时避免忙轮询
    delay(1);
}
