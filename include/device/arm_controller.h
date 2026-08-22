#pragma once
#include "app_config.h"
#include "device/arm_limits.h"
#include "device/nano_boot_session.h"
static const int TILT_MIN = -90;
static const int TILT_MAX = 90;
static const int SLIDER_MIN = 0;
static const int SLIDER_MAX = 2500;

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
extern bool nanoEnableStateKnown;
extern bool nanoEnabled;
extern bool nanoSliderArrived;
extern float nanoSliderArrivedTargetMm;
extern long nanoSliderArrivedPositionSteps;
extern unsigned long nanoSliderArrivedAt;

void sendNano(
  char cmd,
  const String& value = "",
  const String& captureTaskId = ""
);
void pollNano();
void reportPendingNanoSliderArrival();
void sendPanTarget(float valueDeg);
void sendTiltTarget(float valueDeg);
void sendPersonTrackingPoseNoWait(float panDegValue, float tiltDegValue);
void sendPanSpeed(float valueDegPerSec);
void sendTiltSpeed(float valueDegPerSec);
void sendNanoBootFinish(float garmentPan, float garmentTilt);
void handleNanoStartupSync();
void markNanoStartupAimReady(float garmentPan, float garmentTilt);
void ensureNanoStatusProbe();
NanoProbeResult getNanoStatusProbeResult();
String getNanoStatusProbeLine();
void beginNanoOtaCancel();
void resumeNanoAfterOtaFailure();
void sendPanTilt();
void sendSlider();
void applyArmSpeed(const String& speed);
bool handleArmAction(const String& action);

// 边界钳位 (供外部模块使用)
float clampPanTargetDeg(float value);
float clampTiltTargetDeg(float value);

// 摇杆连续运动接口
void getArmJoystickMaxSpeed(float& maxPanSpeed, float& maxTiltSpeed);
void setArmJoystickMotion(float x, float y);
void stopArmJoystickMotion();
void updateArmJoystickMotion();
