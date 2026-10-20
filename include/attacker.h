#pragma once
#include <Arduino.h>
#include "http/HttpClient.h"

/**
 * @file attacker.h
 * @brief 定时攻击引擎（当前支持 Deauth，可扩展其他攻击方式）
 *
 * 攻击计划（目标列表 + 间隔 + 省电开关 + 类型）持久化在 AppSettings，
 * 由前端"开始/停止"时一次性提交完整计划写入闪存，重启后自动恢复攻击。
 * 统计信息（轮次/包数）仅存 RAM，不落盘避免闪存磨损。
 */

#define MAX_ATTACK_TARGETS 16

typedef enum {
    ATTACK_TYPE_DEAUTH = 0,
    ATTACK_TYPE_COUNT
} AttackType;

// setup() 中调用：恢复闪存中的攻击计划
void attackerInit();

// loop() 中调用：驱动攻击状态机（ps 模式下间隔期分块浅睡眠）
void attackerTick();

// 距离下次攻击开火的毫秒数（未启用时返回 UINT32_MAX），供 ap_powersave 睡眠分块对齐
uint32_t attackerMsUntilNextFire();

bool attackerIsRunning();

// 挂起 / 恢复攻击引擎（支持嵌套计数）
// 用于在扫描 AP、扫描设备、OTA 升级或单次测试发包期间临时挂起后台攻击任务
void attackerSuspend();
void attackerResume();
bool attackerIsSuspended();

// RAII 保护守卫：在作用域内自动挂起，退出作用域（包括提前 return）自动恢复
class AttackerSuspendGuard {
public:
    AttackerSuspendGuard() { attackerSuspend(); }
    ~AttackerSuspendGuard() { attackerResume(); }
    AttackerSuspendGuard(const AttackerSuspendGuard&) = delete;
    AttackerSuspendGuard& operator=(const AttackerSuspendGuard&) = delete;
};

void handleAttackStatusApi(HttpClient& client);
void handleAttackPlanApi(HttpClient& client);
