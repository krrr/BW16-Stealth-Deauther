#include "ap_powersave.h"
#include "settings.h"
#include "main.h"
#include "attacker.h"
#include <wifi_conf.h>
#include <wifi_util.h>
#include <wifi_structures.h>
#include "rtc_api.h"

#ifdef __cplusplus
extern "C" {
#include "ameba_soc.h"
}
#endif

// 调试日志开关
#define AP_PS_DEBUG 1

// wext_suspend_softap 未在 SDK 头文件中声明，这里按 wifi_util.c 中的定义补声明
extern "C" void wifi_suspend_softap();
// rltk_resume_softap 未在 SDK 头文件中声明，由 lib_wlan.a 导出
extern "C" int rltk_resume_softap(const char *ifname);


// SoftAP 关联客户端列表，布局与 SDK wifi_conf.c 中 wifi_suspend_softap() 使用的一致：
// 先调用方填入 count 作为容量，驱动会回填实际关联数量并填充 mac_list
struct SoftApClientList {
    int count;
    rtw_mac_t mac_list[AP_STA_NUM];
};

static volatile int g_busy_count = 0;      // 占用射频的异步任务计数
static ApPowerSaveState g_state = ApPowerSaveState::DISABLED;
static bool g_suspended = false;           // softAP 当前是否已挂起
static bool g_saw_client = false;          // 本窗口内是否出现过客户端
static uint32_t g_window_start_ms = 0;     // 当前占空比窗口起点
static uint32_t g_last_client_ms = 0;      // 最后一次检测到客户端的时间
static uint32_t g_last_poll_ms = 0;        // 客户端轮询节流
static uint32_t g_wake_at_ms = 0;          // DUTY_SLEEP 唤醒时刻（millis 时间轴）
static bool g_schedule_empty_warned = false; // 已提示"调度开启但未选小时"

static const uint32_t CLIENT_POLL_INTERVAL_MS = 2000;
static const uint32_t SLEEP_CHUNK_MS = 2000;
static const uint32_t SCHEDULE_OFF_SLEEP_CHUNK_S = 10;

// 开机后强制保持 AP 开启的时长（秒）：即使当前时间不在允许调度内，也不在此时间段内关闭 AP
#define BOOT_AP_HOLD_SEC 30

// 判断 RTC 时间是否有效（2020-09-13 之后才认为是设置过的时间）
static bool rtcTimeValid() {
    return rtc_read() > 1600000000UL;
}

// 判断某 unix 秒所在小时是否在允许开启的时段内
// 注意：schedule_hours_mask 按 UTC 小时解释（(unix_sec/3600)%24 即为 UTC 时区的小时），
// 前端 Web 页面负责在浏览器本地时区与 UTC 之间换算后再上报
static bool hourAllowed(uint32_t unix_sec) {
    if (g_appSettings.schedule_enable != 1) return true;
    int hour = (unix_sec / 3600) % 24;
    return (g_appSettings.schedule_hours_mask >> hour) & 1UL;
}

// 从 unix_sec 开始找到下一个"允许开启"的小时起点（整点）
static uint32_t nextAllowedHourStart(uint32_t unix_sec) {
    uint32_t base = (unix_sec / 3600) * 3600;
    for (int i = 1; i <= 24; i++) {
        uint32_t cand = base + (uint32_t)i * 3600;
        int hour = (cand / 3600) % 24;
        if ((g_appSettings.schedule_hours_mask >> hour) & 1UL) {
            return cand;
        }
    }
    return base + 24 * 3600;  // 兜底：无允许小时时顺延 24 小时
}

// debug用
static void printWakelockStatus(const char *tag) {
    uint32_t lock_mask = pmu_get_wakelock_status();
    Serial.print("[APPowerSave] ");
    Serial.print(tag);
    Serial.print(" Wakelock: 0x");
    Serial.print(lock_mask, HEX);
    if (lock_mask == 0) {
        Serial.println(" [ALL RELEASED -> CPU SLEEP READY]");
    } else {
        Serial.print(" [HELD BY: ");
        if (lock_mask & (1 << 0)) Serial.print("PMU_OS ");
        if (lock_mask & (1 << 1)) Serial.print("WLAN ");
        if (lock_mask & (1 << 2)) Serial.print("LOGUART ");
        if (lock_mask & (1 << 3)) Serial.print("SDIO ");
        if (lock_mask & (1 << 4)) Serial.print("USB ");
        if (lock_mask & (1 << 5)) Serial.print("BT ");
        Serial.println("]");
    }
}

// 断开所有已关联客户端（与 SDK wifi_suspend_softap 行为一致），然后挂起 softAP
static void suspendSoftAP() {
    if (g_suspended) return;

    wifi_enable_powersave();
    wifi_suspend_softap();  // 内部会踢掉所有客户端然后调用rltk_suspend_softap
    g_suspended = true;

    // printWakelockStatus("suspendSoftAP");
}

// 重新挂载 softAP 的 beacon 与 WPA2 安全上下文。
// 反汇编 lib_wlan.a 确认：rltk_resume_softap 只做驱动侧"取消挂起"——重开 beacon
// 硬件开关（rtw_hal_set_hwreg(hwvar8)）、清挂起标志（adapter+0x2315）、上报事件，
// 但不会把 beacon 帧重新下发给固件，也不会重建 AP 的 WPA2/PSK 上下文。
// 因此恢复后表现为：能响应 probe/auth、能收 assoc 请求（rtw_ap_update_sta_ra_info
// 正常执行），但固件不发 beacon、4 次握手无法开始，客户端最终 deauth reason code(4)。
//
// 挂载 beacon 的真正路径是驱动模式切换：
//   wext_set_mode(MASTER) -> setopmode_hdl -> rtl8721d_var_set_opmode
//   -> ROM_WIFI_SetOpmodeAP + ROM_WIFI_RESUME_TxBeacon + start_bss_network
// 首次上电由 wifi_start_ap 的 wext_set_mode(WLAN1, MASTER) 触发；恢复后驱动已处于
// "MASTER 模式"，直接再调 MASTER 会被当成无变化不重挂，所以先切走（INFRA）再切回。
// 这里复用 wifi_start_ap() 的纯 ioctl 配置流程（跳过会长时间假死的 apActivate 轮询）：
// 反汇编 rtw_wx_set_ap_essid 确认 wext_set_ap_ssid 内部会走
// rtw_generate_bcn_ie -> rtw_check_beacon_data -> rtw_hal_set_hwreg(beacon) -> psk_init，
// 把 beacon 内容重新推给固件并重建安全上下文。
static void reattachSoftApConfig() {
    const char* ssid = g_appSettings.ap_ssid;
    const char* pass = g_appSettings.ap_pass;

    wext_set_auth_param(WLAN1_NAME, RTW_AUTH_80211_AUTH_ALG, RTW_AUTH_ALG_OPEN_SYSTEM);
    wext_set_key_ext(WLAN1_NAME, RTW_ENCODE_ALG_CCMP, NULL, 0, 0, 0, 0, NULL, 0);
    wext_set_passphrase(WLAN1_NAME, (const u8*)pass, strlen(pass));
    wext_set_ap_ssid(WLAN1_NAME, (const u8*)ssid, strlen(ssid));
    wext_set_channel(WLAN1_NAME, ap_channel);
}

// 恢复 softAP 的 beacon 发射：模式翻转（INFRA->MASTER）重新挂载 beacon，
// 再走一遍配置 ioctl 重建安全上下文，最后直清固件 beacon 停止位兜底
static void remountSoftAPBeacon() {
    int ret_infra = wext_set_mode(WLAN1_NAME, RTW_MODE_INFRA);
    int ret_master = wext_set_mode(WLAN1_NAME, RTW_MODE_MASTER);
#if AP_PS_DEBUG
    Serial.print("[APPowerSave] remount: mode->INFRA ret=");
    Serial.println(ret_infra);
    Serial.print("[APPowerSave] remount: mode->MASTER ret=");
    Serial.println(ret_master);
#endif

    reattachSoftApConfig();
}

static void resumeSoftAP() {
    if (!g_suspended) return;

#if AP_PS_DEBUG
    Serial.println("[APPowerSave] start resume SoftAP");
    uint32_t resume_t0 = millis();
#endif
    //  - 方案（rltk_resume_softap + WiFi.apbegin）：apbegin 内部重复走 wext_set_mode
    //    的 beacon 挂载，会长时间假死（apActivate 的 20s 轮询超时）；
    //  - 方案（rltk_reattach_softap_in_concurrent_mode）：rltk_wlan_start(1) 重开
    //    已打开接口会触发 _netdev_open/_netdev_if2_open 无限递归 → 栈溢出崩溃。
    //  - 方案 wext_suspend_softap_beacon加wifi_rf_off的组合也无效，无法进入省电。
    //  - 方案 wifi_disable_powersave加rltk_resume_softap的组合能正常进入和恢复省电，但是恢复后ap不发beacon，恢复过程也耗费100多ms
    // 反汇编 rtw_pm_set 确认：wifi_resume_powersave(type8) 只是切回 IPS=3/LPS=9，
    // 不会在 SoftAP 挂起后先做完整的 IPS leave；紧接着的 rltk_resume_softap H2C 会卡在
    // 驱动唤醒流程。wifi_disable_powersave() 会走 _rtw_pwr_wakeup 把 RF 真正拉起，
    // 恢复 AP 后再重新使能 powersave，避免卡死并恢复 beacon。
    if (wifi_disable_powersave() != RTW_SUCCESS) {
        Serial.println("[APPowerSave] disable powersave failed, skip softap resume");
        return;
    }
#if AP_PS_DEBUG
    Serial.println("[APPowerSave] called disable_powersave");
#endif

    int ret = rltk_resume_softap(WLAN1_NAME);

    if (ret == 0) {
        // rltk_resume_softap 不会重新下发 beacon 帧/重建安全上下文，
        // 模式翻转重新挂载 beacon + 重建 WPA2 上下文 + 直清固件 beacon 停止位
        remountSoftAPBeacon();
        g_suspended = false;
#if AP_PS_DEBUG
        Serial.print("[APPowerSave] AP resumed in ");
        Serial.print(millis() - resume_t0);
        Serial.println(" ms");
#endif
    } else {
        Serial.print("[APPowerSave] SoftAP resume failed");
        Serial.println(ret);
    }
}

void apPowerSaveInit() {
    pmu_acquire_wakelock(PMU_OS);
    pmu_set_sleep_type(SLEEP_CG); // KM4 采用 Clock Gating 浅睡眠，不掉电、不关 RAM、中断即刻恢复
    // 在wifi初始化之后再启用省电模式
    if (wifi_enable_powersave() != RTW_SUCCESS) {
        Serial.println("[APPowerSave] wifi_enable_powersave() failed!");
    }
    rltk_wlan_enable_powersave_in_STA_AP_mode();
    g_state = ApPowerSaveState::DISABLED;
    g_suspended = false;
}

int apPowerSaveClientCount() {
    SoftApClientList client_info;
    client_info.count = AP_STA_NUM;
    if (wifi_get_associated_client_list(&client_info, sizeof(client_info)) != RTW_SUCCESS) {
        return -1;
    }
    int n = client_info.count;
    if (n > AP_STA_NUM) n = AP_STA_NUM;
    return n;
}

static void enterApOn();

void apPowerSaveHold() {
    taskENTER_CRITICAL();
    g_busy_count++;
    taskEXIT_CRITICAL();

    if (g_suspended) {
        enterApOn();
    }
}

void apPowerSaveRelease() {
    taskENTER_CRITICAL();
    if (g_busy_count > 0) g_busy_count--;
    taskEXIT_CRITICAL();
}

ApPowerSaveState apPowerSaveGetState() {
    return g_state;
}

bool apPowerSaveIsSuspended() {
    return g_suspended;
}

const char* apPowerSaveStateName() {
    switch (g_state) {
        case ApPowerSaveState::DISABLED:    return "DISABLED";
        case ApPowerSaveState::AP_ON:       return "AP_ON";
        case ApPowerSaveState::DUTY_SLEEP:  return "DUTY_SLEEP";
        case ApPowerSaveState::SCHEDULE_OFF:return "SCHEDULE_OFF";
    }
    return "UNKNOWN";
}

uint32_t apPowerSaveNextOnInSec() {
    switch (g_state) {
        case ApPowerSaveState::AP_ON:
        case ApPowerSaveState::DISABLED:
            return 0;
        case ApPowerSaveState::DUTY_SLEEP: {
            uint32_t now = millis();
            return (g_wake_at_ms - now) / 1000;
        }
        case ApPowerSaveState::SCHEDULE_OFF: {
            if (!rtcTimeValid()) return 0;
            return nextAllowedHourStart(rtc_read()) - rtc_read();
        }
    }
    return 0;
}

// 进入占空比休眠：挂起 softAP，睡到"本窗口起点 + N*周期"的下一个相位点
static void enterDutySleep(uint32_t now) {
    suspendSoftAP();
    uint32_t period_sec = g_appSettings.duty_period_sec;
    if (period_sec == 0) period_sec = 120; // 兜底保护，防止除以 0 导致 HardFault 死机

    uint32_t elapsed = now - g_window_start_ms;
    uint32_t period_ms = period_sec * 1000;
    uint32_t next_phase = g_window_start_ms + (elapsed / period_ms + 1) * period_ms;
    g_wake_at_ms = next_phase;
    g_state = ApPowerSaveState::DUTY_SLEEP;
    Serial.print("[APPowerSave] -> DUTY_SLEEP, wake in ");
    Serial.print((g_wake_at_ms - now) / 1000);
    Serial.println("s");
    Serial.flush();
}

// 进入时段休眠：挂起 softAP，睡到下一个允许小时起点
static void enterScheduleOff() {
    suspendSoftAP();
    g_state = ApPowerSaveState::SCHEDULE_OFF;
    Serial.println("[APPowerSave] -> SCHEDULE_OFF");
    Serial.flush();
}

// 恢复 AP 并开启新的占空比窗口
static void enterApOn() {
    resumeSoftAP();
    g_window_start_ms = millis();
    g_saw_client = false;
    g_last_client_ms = 0;
    g_state = ApPowerSaveState::AP_ON;
    Serial.println("[APPowerSave] -> AP_ON");
    Serial.flush();
}

static void tickActive() {
    uint32_t now = millis();

    // 开机后 BOOT_AP_HOLD_SEC 秒内固定保持 AP 开启（即使时间不在调度内也不关闭）
    if (now < (uint32_t)BOOT_AP_HOLD_SEC * 1000) return;

    // 客户端轮询节流
    if (now - g_last_poll_ms < CLIENT_POLL_INTERVAL_MS) return;
    g_last_poll_ms = now;

    int clients = apPowerSaveClientCount();

    // 分时段开启AP：当前小时不允许开启时进入 SCHEDULE_OFF；
    // 但仍有客户端连接时保持 AP 开启
    if (g_appSettings.schedule_enable == 1) {
        if (g_appSettings.schedule_hours_mask == 0) {
            // 无任何允许时段：视为未配置，保持常开并告警一次
            if (!g_schedule_empty_warned) {
                Serial.println("[APPowerSave] schedule enabled but no hour selected, ignoring schedule");
                g_schedule_empty_warned = true;
            }
        } else if (rtcTimeValid() && !hourAllowed(rtc_read())) {
            if (clients > 0) {
                g_saw_client = true;
                g_last_client_ms = now;
                return;  // HOLD：时段不允许但仍有客户端连接，保持 AP 开启
            }
            enterScheduleOff();
            return;
        }
    }

    if (clients > 0) {
        g_saw_client = true;
        g_last_client_ms = now;
        return;  // HOLD：只要有客户端就一直保持开启
    }

    uint32_t elapsed = now - g_window_start_ms;
    uint32_t on_ms = (uint32_t)g_appSettings.duty_on_sec * 1000;
    uint32_t hold_ms = (uint32_t)g_appSettings.client_hold_sec * 1000;
    bool window_done = elapsed >= on_ms;
    bool need_hold = g_saw_client && hold_ms > 0 && (now - g_last_client_ms < hold_ms);
    if (window_done && !need_hold) {
        enterDutySleep(now);
    }
}

static void tickDutySleep() {
    uint32_t now = millis();
    uint32_t remaining = g_wake_at_ms - now;
    if (remaining == 0 || remaining > 0x80000000UL) {
        pmu_acquire_wakelock(PMU_OS);
        // 已到唤醒时刻：若此时处于非允许时段则转入 SCHEDULE_OFF，否则开启新窗口
        if (g_appSettings.schedule_enable == 1 && rtcTimeValid() && !hourAllowed(rtc_read())) {
            enterScheduleOff();
        } else {
            enterApOn();
        }
        return;
    }

    // 按 SLEEP_CHUNK_MS 分块休眠，在 delay 期间释放 PMU_OS wakelock 让 CPU 进入 tickless 浅睡眠
    // 攻击感知：分块不超过距下次攻击开火的时间，保证定时攻击准点醒开发包
    uint32_t sleep_ms = (remaining < SLEEP_CHUNK_MS) ? remaining : SLEEP_CHUNK_MS;
    uint32_t attack_remain = attackerMsUntilNextFire();
    if (attack_remain != UINT32_MAX && attack_remain < sleep_ms) {
        sleep_ms = attack_remain;
    }
    if (sleep_ms == 0) return;  // 已到攻击时刻，立即交由 attackerTick 发包

    pmu_set_max_sleep_time(sleep_ms);
    pmu_release_wakelock(PMU_OS);

    delay(sleep_ms);

    pmu_acquire_wakelock(PMU_OS);
#if AP_PS_DEBUG
    if (sleep_ms >= SLEEP_CHUNK_MS) {
        Serial.print("[APPowerSave] SLEEP_CHUNK_MS wake at ");
        Serial.print(millis());
        Serial.println(", still alive");
        Serial.flush();
    }
#endif
}

// 分时段关闭
static void tickScheduleOff() {
    pmu_acquire_wakelock(PMU_OS);
    // 调度被关闭或 RTC 失效：直接恢复 AP（异常兜底）
    if (g_appSettings.schedule_enable != 1 || !rtcTimeValid()) {
        enterApOn();
        return;
    }
    if (g_appSettings.schedule_hours_mask == 0) {
        enterApOn();
        return;
    }

    uint32_t rtc_now = rtc_read();
    uint32_t next = nextAllowedHourStart(rtc_now);
    uint32_t remaining_sec = next - rtc_now;
    if (remaining_sec == 0) {
        enterApOn();
        return;
    }
    uint32_t chunk_sec = min(remaining_sec, SCHEDULE_OFF_SLEEP_CHUNK_S);
    uint32_t chunk_ms = chunk_sec * 1000;

    // 攻击感知：分块不超过距下次攻击开火的时间，保证定时攻击准点醒开发包
    uint32_t attack_remain = attackerMsUntilNextFire();
    if (attack_remain != UINT32_MAX && attack_remain < chunk_ms) {
        chunk_ms = attack_remain;
    }
    if (chunk_ms == 0) return;  // 已到攻击时刻，立即交由 attackerTick 发包

    pmu_set_max_sleep_time(chunk_ms);
    pmu_release_wakelock(PMU_OS);

    delay(chunk_ms);

    pmu_acquire_wakelock(PMU_OS);
#if AP_PS_DEBUG
    if (chunk_ms >= SLEEP_CHUNK_MS) {
        Serial.print("[APPowerSave] SLEEP_CHUNK_MS wake at ");
        Serial.print(millis());
        Serial.println(", still alive");
        Serial.flush();
    }
#endif
}

// 主入口，由main调用
void apPowerSaveTick() {
    // BUSY 抑制：beacon 抓取等异步任务占用射频期间不允许休眠
    if (g_busy_count > 0) {
        if (g_suspended) {
            enterApOn();
        }
        return;
    }

    if (g_appSettings.ap_powersave_enable != 1) {
        if (g_suspended) {
            resumeSoftAP();
            g_window_start_ms = millis();
            g_saw_client = false;
            g_last_client_ms = 0;
            Serial.println("[APPowerSave] AP resumed (power save disabled)");
        }
        g_state = ApPowerSaveState::DISABLED;
        return;
    }

    switch (g_state) {
        case ApPowerSaveState::DISABLED:
            // 首次启用：以当前时刻为窗口起点，AP 本来就是开启的
            g_window_start_ms = millis();
            g_saw_client = false;
            g_last_client_ms = 0;
            g_state = ApPowerSaveState::AP_ON;
            Serial.println("[APPowerSave] -> AP_ON (enabled)");
            break;
        case ApPowerSaveState::AP_ON:
            tickActive();
            break;
        case ApPowerSaveState::DUTY_SLEEP:
            tickDutySleep();
            break;
        case ApPowerSaveState::SCHEDULE_OFF:
            tickScheduleOff();
            break;
    }
}
