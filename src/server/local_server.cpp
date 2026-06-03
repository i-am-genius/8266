#include "server/local_server.h"
#include "device/light_control.h"
#include "config/config_manager.h"
#include "network/http_reporter.h"

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
  ESP.restart();
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

  server.begin();
}
