#pragma once
#include "app_config.h"

void addCorsHeaders();
void addCorsHeadersWithMethods();
void setupDeviceHttpServer();
void handleStatus();
void handleSetLight();
void handleResumeBroadcast();
void handleStopBroadcast();
void handleStopAnnounce();
void handleResetWifi();
void handleLampControl();

// 局域网摇杆/云台 HTTP 控制接口
void handleArmJoystick();
void handleArmStop();
void handleArmPosition();
void handleArmSpeed();
void handleArmAction();
