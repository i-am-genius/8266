#include "server/local_server.h"
#include "device/light_control.h"
#include "device/arm_controller.h"
#include "config/config_manager.h"
#include "network/http_reporter.h"
#include "network/ws_client.h"
#include "diagnostics/diagnostic_logger.h"

void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
}

void addCorsHeadersWithMethods() {
  addCorsHeaders();
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleStatus() {
  addCorsHeaders();
  server.send(200, "application/json", "{\"status\":\"已配网\"}");
}

void handleSetLight() {
  addCorsHeadersWithMethods();

  if (server.method() == HTTP_OPTIONS) {
    server.send(204);
    return;
  }

  if (isCollisionParkActive()) {
    stopArmJoystickMotion();
    server.send(423, "application/json", "{\"error\":\"collision park active\"}");
    return;
  }

  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"缺少 body\"}");
    return;
  }

  String body = server.arg("plain");
  DynamicJsonDocument doc(512);
  auto err = deserializeJson(doc, body);
  if (err) {
    server.send(400, "application/json", "{\"error\":\"JSON 解析失败\"}");
    return;
  }

  LightState before = diagnosticCurrentLightState();
  stopEffectWaveForManualControl();
  stopLocateBreath(false);

  brightness = doc["brightness"] | brightness;
  temp = doc["temp"] | temp;
  autoMode = doc["auto"] | autoMode;
  recommendedBrightness = doc["recommendedBrightness"] | recommendedBrightness;
  recommendedTemp = doc["recommendedTemp"] | recommendedTemp;
  luxAutoTarget = doc["luxAutoTarget"] | luxAutoTarget;
  safeCopyFabric(doc["fabric"]);

  DEBUG_SERIAL.printf("收到 HTTP 控制: bri=%d temp=%d auto=%d recB=%d recT=%d fabric=%s\n",
                brightness, temp, autoMode,
                recommendedBrightness, recommendedTemp, fabric);

  applyLightSettings(autoMode ? recommendedBrightness : brightness, autoMode ? recommendedTemp : temp);
  lastLightUpdate = millis();
  requestDeviceStateReport("LOCAL_SET_LIGHT");
  diagnosticLogLightChange("local", before, diagnosticCurrentLightState());

  server.send(200, "application/json", "{\"result\":\"OK\"}");
}

void handleResumeBroadcast() {
  enableBroadcast = true;
  enableAnnounce = true;
  addCorsHeaders();
  server.send(200, "application/json", "{\"status\":\"resumed\"}");
  DEBUG_SERIAL.println("接收到网页指令：恢复广播");
}

void handleStopBroadcast() {
  enableBroadcast = false;
  addCorsHeaders();
  server.send(200, "application/json", "{\"result\":\"Broadcast stopped\"}");
  DEBUG_SERIAL.println("接收到网页指令：停止广播");
}

void handleStopAnnounce() {
  enableAnnounce = false;
  addCorsHeaders();
  server.send(200, "application/json", "{\"result\":\"Announce stopped\"}");
  DEBUG_SERIAL.println("接收到网页指令：停止上报");
}

void handleResetWifi() {
  clearConfig();
  addCorsHeaders();
  server.send(200, "application/json", "{\"result\":\"WiFi config cleared, restarting\"}");
  delay(800);
  diagnosticRestart("wifi_config_cleared");
}

// POST /arm/joystick — 局域网摇杆连续控制，与 WS arm_joystick 逻辑一致
void handleArmJoystick() {
  addCorsHeadersWithMethods();

  if (server.method() == HTTP_OPTIONS) {
    server.send(204);
    return;
  }

  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"缺少 body\"}");
    return;
  }

  String body = server.arg("plain");
  DynamicJsonDocument doc(256);
  auto err = deserializeJson(doc, body);
  if (err) {
    server.send(400, "application/json", "{\"error\":\"JSON 解析失败\"}");
    return;
  }

  float x = doc["x"] | 0.0f;
  float y = doc["y"] | 0.0f;

  setArmJoystickMotion(x, y);
  diagnosticRecordArm(DIAG_SOURCE_LOCAL, "joystick", panDeg, tiltDeg, sliderMm);

  DEBUG_SERIAL.printf("[HTTP] arm_joystick x=%.2f y=%.2f\n", x, y);
  server.send(200, "application/json", "{\"result\":\"OK\"}");
}

// POST /arm/stop — 停止摇杆运动
void handleArmStop() {
  addCorsHeadersWithMethods();

  if (server.method() == HTTP_OPTIONS) {
    server.send(204);
    return;
  }

  stopArmJoystickMotion();
  DEBUG_SERIAL.println("[HTTP] arm_stop");
  server.send(200, "application/json", "{\"result\":\"OK\"}");
}

// POST /arm/position — 精确位置控制
void handleArmPosition() {
  addCorsHeadersWithMethods();

  if (server.method() == HTTP_OPTIONS) {
    server.send(204);
    return;
  }

  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"缺少 body\"}");
    return;
  }

  String body = server.arg("plain");
  DynamicJsonDocument doc(256);
  auto err = deserializeJson(doc, body);
  if (err) {
    server.send(400, "application/json", "{\"error\":\"JSON 解析失败\"}");
    return;
  }

  const bool hasPan = doc.containsKey("pan");
  const bool hasTilt = doc.containsKey("tilt");
  const bool hasSlider = doc.containsKey("slider");
  if (!isCollisionParkActive() && isPersonTrackingAimActive() && updatePersonTrackingAim(
        hasPan,
        hasPan ? doc["pan"].as<float>() : 0.0f,
        hasTilt,
        hasTilt ? doc["tilt"].as<float>() : 0.0f
      )) {
    diagnosticRecordArm(DIAG_SOURCE_LOCAL, "person_tracking", panDeg, tiltDeg, sliderMm);
    DEBUG_SERIAL.printf(
      "[HTTP] person tracking pose pan=%d tilt=%d slider=%d\n",
      panDeg,
      tiltDeg,
      sliderMm
    );
    server.send(200, "application/json", "{\"result\":\"OK\",\"source\":\"person_tracking\"}");
    return;
  }

  bool changed = false;
  stopArmJoystickMotion();

  if (!isCollisionParkActive() && doc.containsKey("pan")) {
    panDeg = (int)clampPanTargetDeg(doc["pan"].as<int>());
    sendPanTarget(panDeg);
    changed = true;
  }

  if (!isCollisionParkActive() && doc.containsKey("tilt")) {
    tiltDeg = (int)clampTiltTargetDeg(doc["tilt"].as<int>());
    sendTiltTarget(tiltDeg);
    changed = true;
  }

  if (doc.containsKey("slider")) {
    sliderMm = doc["slider"].as<int>();
    sliderMm = constrain(sliderMm, SLIDER_MIN, SLIDER_MAX);
    sendNano('x', String((float)sliderMm, 2));
    changed = true;
  }

  if (changed) {
    diagnosticRecordArm(DIAG_SOURCE_LOCAL, "position", panDeg, tiltDeg, sliderMm);
    DEBUG_SERIAL.printf("[HTTP] arm_position pan=%d tilt=%d slider=%d\n", panDeg, tiltDeg, sliderMm);
    server.send(200, "application/json", "{\"result\":\"OK\"}");
  } else {
    server.send(400, "application/json", "{\"error\":\"缺少 pan/tilt/slider\"}");
  }
}

// POST /arm/speed — 速度切换
void handleArmSpeed() {
  addCorsHeadersWithMethods();

  if (server.method() == HTTP_OPTIONS) {
    server.send(204);
    return;
  }

  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"缺少 body\"}");
    return;
  }

  String body = server.arg("plain");
  DynamicJsonDocument doc(256);
  auto err = deserializeJson(doc, body);
  if (err) {
    server.send(400, "application/json", "{\"error\":\"JSON 解析失败\"}");
    return;
  }

  String speed = doc["speed"] | "normal";
  speed.trim();
  if (speed.length() == 0) speed = "normal";

  applyArmSpeed(speed);
  sendArmSpeedStatus("local_http");
  diagnosticRecordArm(DIAG_SOURCE_LOCAL, "speed", panDeg, tiltDeg, sliderMm);

  DEBUG_SERIAL.println("[HTTP] arm_speed: " + speed);
  server.send(200, "application/json", "{\"result\":\"OK\"}");
}

// POST /arm — 方向动作 (home, stop, up, down, left, right 等)
void handleArmActionHttp() {
  addCorsHeadersWithMethods();

  if (server.method() == HTTP_OPTIONS) {
    server.send(204);
    return;
  }

  if (isCollisionParkActive()) {
    stopArmJoystickMotion();
    server.send(423, "application/json", "{\"error\":\"collision park active\"}");
    return;
  }

  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"缺少 body\"}");
    return;
  }

  String body = server.arg("plain");
  DynamicJsonDocument doc(256);
  auto err = deserializeJson(doc, body);
  if (err) {
    server.send(400, "application/json", "{\"error\":\"JSON 解析失败\"}");
    return;
  }

  String action = doc["action"] | "";
  action.trim();

  if (action.length() == 0) {
    server.send(400, "application/json", "{\"error\":\"缺少 action\"}");
    return;
  }

  if (handleArmAction(action)) {
    diagnosticRecordArm(DIAG_SOURCE_LOCAL, action.c_str(), panDeg, tiltDeg, sliderMm);
    DEBUG_SERIAL.println("[HTTP] arm action: " + action);
    server.send(200, "application/json", "{\"result\":\"OK\"}");
  } else {
    server.send(400, "application/json", "{\"error\":\"未知 action\"}");
  }
}

// POST /lamp/control — 局域网内另一台 8266 (摄像头机位) 转发的跟踪控制指令
void handleLampControl() {
  addCorsHeadersWithMethods();

  if (server.method() == HTTP_OPTIONS) {
    server.send(204);
    return;
  }

  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"缺少 body\"}");
    return;
  }

  String body = server.arg("plain");
  DynamicJsonDocument doc(512);
  auto err = deserializeJson(doc, body);
  if (err) {
    server.send(400, "application/json", "{\"error\":\"JSON 解析失败\"}");
    return;
  }

  // 灯光控制 (可选字段)
  LightState before = diagnosticCurrentLightState();
  bool hasBri = !doc["brightness"].isNull();
  bool hasTemp = !doc["temp"].isNull();
  if (hasBri || hasTemp) {
    stopEffectWaveForManualControl();
    stopLocateBreath(false);

    if (hasBri)  brightness = doc["brightness"].as<int>();
    if (hasTemp) temp = doc["temp"].as<int>();

    applyLightSettings(autoMode ? recommendedBrightness : brightness,
                       autoMode ? recommendedTemp : temp);
    lastLightUpdate = millis();
  }

  // 云台角度控制 (绝对角度)
  bool hasPan = !doc["pan"].isNull();
  bool hasTilt = !doc["tilt"].isNull();
  if (!isCollisionParkActive() && hasPan)  sendPanTarget(doc["pan"].as<float>());
  if (!isCollisionParkActive() && hasTilt) sendTiltTarget(doc["tilt"].as<float>());
  if (hasPan || hasTilt) {
    int loggedPan = hasPan ? (int)round(doc["pan"].as<float>()) : panDeg;
    int loggedTilt = hasTilt ? (int)round(doc["tilt"].as<float>()) : tiltDeg;
    diagnosticRecordArm(DIAG_SOURCE_LOCAL, "position", loggedPan, loggedTilt, sliderMm);
  }

  DEBUG_SERIAL.printf("[LAMP_CONTROL] pan=%s tilt=%s bri=%s temp=%s\n",
    hasPan  ? String(doc["pan"].as<float>()).c_str()  : "-",
    hasTilt ? String(doc["tilt"].as<float>()).c_str() : "-",
    hasBri  ? String(doc["brightness"].as<int>()).c_str() : "-",
    hasTemp ? String(doc["temp"].as<int>()).c_str()  : "-");

  diagnosticLogLightChange("local", before, diagnosticCurrentLightState());

  server.send(200, "application/json", "{\"result\":\"OK\"}");
}

void setupDeviceHttpServer() {
  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) {
      addCorsHeadersWithMethods();
      server.send(204);
    } else {
      server.send(404);
    }
  });

  server.on("/status", handleStatus);
  server.on("/setLight", handleSetLight);
  server.on("/stopBroadcast", handleStopBroadcast);
  server.on("/resumeBroadcast", handleResumeBroadcast);
  server.on("/stopAnnounce", handleStopAnnounce);
  server.on("/resetWifi", HTTP_POST, handleResetWifi);
  server.on("/lamp/control", handleLampControl);

  // 局域网摇杆/云台 HTTP 控制接口
  server.on("/arm/joystick", handleArmJoystick);
  server.on("/arm/stop", handleArmStop);
  server.on("/arm/position", handleArmPosition);
  server.on("/arm/speed", handleArmSpeed);
  server.on("/arm", handleArmActionHttp);

  server.begin();
}
