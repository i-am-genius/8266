#include "network/ws_client.h"
#include "device/light_control.h"
#include "device/arm_controller.h"
#include "device/ota_manager.h"
#include "network/http_reporter.h"

void sendWsRegister() {
  StaticJsonDocument<320> doc;
  doc["type"] = "register";
  doc["id"] = deviceId;
  doc["chipId"] = deviceId;
  doc["deviceType"] = FW_DEVICE_TYPE;
  doc["fwVersion"] = FW_VERSION;
  doc["fwVersionCode"] = FW_VERSION_CODE;
  doc["firmwareChannel"] = FW_CHANNEL;
  doc["otaStatus"] = otaStatus;
  doc["otaProgress"] = otaProgress;
  doc["ip"] = WiFi.localIP().toString();
  doc["mac"] = WiFi.macAddress();

  String msg;
  serializeJson(doc, msg);
  webSocket.sendTXT(msg);
  DEBUG_SERIAL.println("[WS] register: " + msg);
}

void handleWsMessage(const String& text) {
  DEBUG_SERIAL.println("[WS] 收到消息: " + text);

  StaticJsonDocument<768> doc;
  DeserializationError err = deserializeJson(doc, text);
  if (err) {
    DEBUG_SERIAL.println("[WS] JSON解析失败");
    return;
  }

  JsonObject root = doc.as<JsonObject>();
  JsonObject payload = root;

  if (root["payload"].is<JsonObject>()) {
    payload = root["payload"].as<JsonObject>();
  } else if (root["data"].is<JsonObject>()) {
    payload = root["data"].as<JsonObject>();
  }

  String type = root["type"] | payload["type"] | "";

  if (type == "state" || type == "control") {
    String targetId = payload["id"] | "";
    String chipId = payload["chipId"] | "";

    if (targetId != deviceId && chipId != deviceId) {
      DEBUG_SERIAL.println("[WS] state 不是发给本设备，忽略");
      return;
    }

    stopEffectWaveForManualControl();

    brightness = payload["brightness"] | brightness;
    temp = payload["temp"] | temp;
    recommendedBrightness = payload["recommendedBrightness"] | recommendedBrightness;
    recommendedTemp = payload["recommendedTemp"] | recommendedTemp;
    luxAutoTarget = payload["luxAutoTarget"] | luxAutoTarget;

    if (payload.containsKey("autoMode")) {
      autoMode = payload["autoMode"];
    } else {
      autoMode = payload["auto"] | autoMode;
    }

    safeCopyFabric(payload["fabric"]);
    sendDeviceStateReport();

    DEBUG_SERIAL.printf("WS控制：亮度=%d 色温=%d 自动=%d 推荐亮度=%d 推荐色温=%d 面料=%s\n",
                  brightness, temp, autoMode,
                  recommendedBrightness, recommendedTemp, fabric);
    return;
  }

  if (type == "effect") {
    String effect = payload["effect"] | "";
    bool enabled = payload["enabled"] | false;

    if (effect != "wave") {
      DEBUG_SERIAL.println("[EFFECT] unsupported effect: " + effect);
      return;
    }

    if (enabled) {
      effectRestoreBrightness = autoMode ? recommendedBrightness : brightness;
      effectRestoreTemp = autoMode ? recommendedTemp : temp;
      effectBaseTemp = constrain(payload["baseTemp"] | effectBaseTemp, 2700, 6500);
      int nextAmplitude = payload["range"] | effectRange;
      if (payload.containsKey("amplitude")) {
        nextAmplitude = payload["amplitude"] | nextAmplitude;
      }
      effectRange = constrain(nextAmplitude, 0, 1900);
      effectSpeed = constrain(payload["speed"] | effectSpeed, 0.2f, 5.0f);
      effectBrightness = constrain(payload["brightness"] | effectBrightness, 0, 100);
      effectPhaseOffset = payload["phaseOffset"] | effectPhaseOffset;
      effectStartMs = millis();
      lastEffectUpdateMs = 0;
      effectWaveEnabled = true;
      autoMode = false;

      int initialTemp = effectBaseTemp + (int)round(sin(effectPhaseOffset) * effectRange);
      if (payload.containsKey("initialTemp")) {
        initialTemp = payload["initialTemp"] | initialTemp;
      }
      initialTemp = constrain(initialTemp, 2700, 6500);
      brightness = effectBrightness;
      temp = initialTemp;
      recommendedBrightness = effectBrightness;
      recommendedTemp = initialTemp;
      applyLightSettings(brightness, temp);
      lastLightUpdate = millis();
      sendDeviceStateReport();

      DEBUG_SERIAL.printf(
        "[EFFECT] wave start baseTemp=%d range=%d speed=%.2f brightness=%d phaseOffset=%.2f restoreB=%d restoreT=%d\n",
        effectBaseTemp,
        effectRange,
        effectSpeed,
        effectBrightness,
        effectPhaseOffset,
        effectRestoreBrightness,
        effectRestoreTemp
      );
    } else {
      effectWaveEnabled = false;
      applyLightSettings(brightness, temp);
      lastLightUpdate = millis();
      sendDeviceStateReport();
      DEBUG_SERIAL.printf("[EFFECT] wave stop restoreB=%d restoreT=%d\n", effectRestoreBrightness, effectRestoreTemp);
    }

    return;
  }

  if (type == "locate") {
    int times = root["times"] | 3;

    int cycleMs = root["duration"] | 1200;
    if (payload.containsKey("duration")) {
      cycleMs = payload["duration"] | cycleMs;
    } else if (payload.containsKey("interval")) {
      cycleMs = payload["interval"] | cycleMs;
    }

    DEBUG_SERIAL.printf("[LOCATE] 收到呼吸定位指令 times=%d cycleMs=%d\n", times, cycleMs);

    locateBreath(times, cycleMs);
    return;
  }

  if (type == "arm_joystick") {
    float x = payload["x"] | 0.0f;
    float y = payload["y"] | 0.0f;
    int durationMs = payload["durationMs"] | 500;

    setArmJoystickMotion(x, y, durationMs);
    return;
  }

  if (type == "arm_stop") {
    stopArmJoystickMotion();
    return;
  }

  if (type == "arm_position") {
    bool changed = false;

    stopArmJoystickMotion();

    if (payload.containsKey("pan")) {
      panDeg = payload["pan"].as<int>();
      panDeg = constrain(panDeg, PAN_MIN, PAN_MAX);
      sendNano('p', String((float)panDeg, 2));
      changed = true;
    }

    if (payload.containsKey("tilt")) {
      tiltDeg = payload["tilt"].as<int>();
      tiltDeg = constrain(tiltDeg, TILT_MIN, TILT_MAX);
      sendNano('t', String((float)tiltDeg, 2));
      changed = true;
    }

    if (payload.containsKey("slider")) {
      sliderMm = payload["slider"].as<int>();
      sliderMm = constrain(sliderMm, SLIDER_MIN, SLIDER_MAX);
      sendNano('x', String((float)sliderMm, 2));
      changed = true;
    }

    if (!changed) {
      DEBUG_SERIAL.println("[ARM] arm_position missing pan/tilt/slider");
    }

    return;
  }

  if (type == "arm_speed") {
    String speed = payload["speed"] | "normal";
    speed.trim();

    if (speed.length() == 0) {
      speed = "normal";
    }

    applyArmSpeed(speed);
    DEBUG_SERIAL.println("[ARM] speed changed: " + speed);
    return;
  }

  if (type == "arm") {
    String action = payload["action"] | "";
    action.trim();

    if (action.length() == 0) {
      action = payload["direction"] | "";
      action.trim();
    }

    if (action.length() == 0) {
      DEBUG_SERIAL.println("[ARM] missing action");
      return;
    }

    handleArmAction(action);
    return;
  }

  if (type == "command") {
    String cmd = root["cmd"] | payload["cmd"] | "";

    if (cmd == "resume_broadcast" || cmd == "resumeBroadcast") {
      enableBroadcast = true;
      enableAnnounce = true;

      lastBroadcast = 0;
      lastAnnounce = 0;

      DEBUG_SERIAL.println("[WS] resume broadcast command received");
      DEBUG_SERIAL.println("[WS] UDP broadcast resumed");
    } else {
      DEBUG_SERIAL.println("[WS] unknown command: " + cmd);
    }
    return;
  }

  if (type == "ota_update" || type == "ota:update") {
    String version = payload["version"] | "";
    String url = payload["url"] | "";
    int versionCode = payload["versionCode"] | 0;
    String channel = payload["channel"] | "";
    String md5 = payload["md5"] | "";

    if (channel.length() == 0) {
      channel = FW_CHANNEL;
    }
    channel.toLowerCase();

    if (version.length() == 0 || url.length() == 0 || versionCode <= 0) {
      DEBUG_SERIAL.println("[OTA] OTA message missing version/url/versionCode");
      otaStatus = "failed";
      sendDeviceStateReport();
      return;
    }

    bool sameChannel = (channel == String(FW_CHANNEL));

    if (sameChannel && versionCode <= FW_VERSION_CODE) {
      DEBUG_SERIAL.println("[OTA] Same channel and target versionCode is not newer, ignore");
      otaStatus = "idle";
      otaProgress = 0;
      sendDeviceStateReport();
      return;
    }

    if (!sameChannel) {
      DEBUG_SERIAL.println("[OTA] Cross-channel OTA allowed");
    }

    doOtaUpdate(url, version, versionCode, channel, md5);
    return;
  }

  DEBUG_SERIAL.println("[WS] 未处理消息类型: " + type);
}

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      DEBUG_SERIAL.println("[WS] 已断开");
      break;

    case WStype_CONNECTED:
      DEBUG_SERIAL.printf("[WS] 已连接: %s\n", payload);
      sendWsRegister();
      break;

    case WStype_TEXT:
      handleWsMessage(String((char*)payload));
      break;

    case WStype_PONG:
      DEBUG_SERIAL.println("[WS] PONG");
      break;

    default:
      break;
  }
}

void beginWebSocketClient() {
  webSocket.disconnect();
  delay(100);

  DEBUG_SERIAL.println("[WS] 准备连接:");
  DEBUG_SERIAL.println("host = " + cfg.serverHost);
  DEBUG_SERIAL.println("port = " + String(cfg.wsPort));
  DEBUG_SERIAL.println("path = " + String(WS_PATH));
  DEBUG_SERIAL.println("url  = ws://" + cfg.serverHost + ":" + String(cfg.wsPort) + String(WS_PATH));

  webSocket.begin(cfg.serverHost.c_str(), cfg.wsPort, WS_PATH);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
  webSocket.enableHeartbeat(15000, 3000, 2);
}
