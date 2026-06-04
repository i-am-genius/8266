#pragma once
#include "app_config.h"

static const int PAN_MIN = -45;
static const int PAN_MAX = 45;
static const int TILT_MIN = -90;
static const int TILT_MAX = 90;
static const int SLIDER_MIN = 0;
static const int SLIDER_MAX = 1200;

// 当前速度档位（applyArmSpeed 设置）
extern String currentArmSpeed;

// 摇杆连续运动状态
extern bool armJoystickActive;
extern unsigned long joystickExpireAt;
extern unsigned long lastArmMotionUpdateAt;
extern bool nanoLineSeen;
extern String lastNanoLine;
extern unsigned long lastNanoRxAt;
extern bool lastNanoHomingOk;
extern bool lastNanoHallStatusOk;

void sendNano(char cmd, const String& value = "");
void pollNano();
void sendPanTarget(float valueDeg);
void sendTiltTarget(float valueDeg);
void sendPanSpeed(float valueDegPerSec);
void sendTiltSpeed(float valueDegPerSec);
void scheduleNanoStartupSync();
void handleNanoStartupSync();
void sendPanTilt();
void sendSlider();
void applyArmSpeed(const String& speed);
void handleArmAction(const String& action);

// 摇杆连续运动接口
void getArmJoystickMaxSpeed(float& maxPanSpeed, float& maxTiltSpeed);
void setArmJoystickMotion(float x, float y, int durationMs);
void stopArmJoystickMotion();
void updateArmJoystickMotion();
