#include "network/ws_client.h"
#include "network/arm_position_policy.h"
#include "device/light_control.h"
#include "device/sensor_manager.h"
#include "device/arm_controller.h"
#include "device/ota_manager.h"
#include "device/self_test.h"
#include "device/garment_aim.h"
#include "device/lamp_aim_state.h"
#include "network/http_reporter.h"
#include "online_logger.h"
#include "diagnostics/diagnostic_logger.h"

static const uint32_t WS_HEARTBEAT_PING_INTERVAL_MS = 30000;
static const uint32_t WS_HEARTBEAT_PONG_TIMEOUT_MS = 10000;
static const uint8_t WS_HEARTBEAT_DISCONNECT_COUNT = 3;

static LampAimState lampAimState{
  LampAimPose{
    GARMENT_AIM_DEFAULT_PAN_DEG,
    GARMENT_AIM_DEFAULT_TILT_DEG,
  },
  LampAimPose{
    GARMENT_AIM_DEFAULT_PAN_DEG,
    GARMENT_AIM_DEFAULT_TILT_DEG,
  },
  LampAimPose{
    LAMP_DEFAULT_PERSON_PAN_DEG,
    LAMP_DEFAULT_PERSON_TILT_DEG,
  },
  LampAimPose{
    LAMP_DEFAULT_PERSON_PAN_DEG,
    LAMP_DEFAULT_PERSON_TILT_DEG,
  },
  false,
  false,
  false,
  false,
  false,
};
static String activeCollisionGuardId = "";

bool isCollisionParkActive() {
  return lampAimState.collisionParkActive;
}
static LampAimSyncGate lampAimSyncGate;
static bool appliedAimInitialized = false;
static LampAimSelection lastAppliedAim{
  LampAimSource::DefaultGarment,
  LampAimPose{
    GARMENT_AIM_DEFAULT_PAN_DEG,
    GARMENT_AIM_DEFAULT_TILT_DEG,
  },
};

static LampAimPose normalizeLampAimPose(const LampAimPose& pose) {
  return LampAimPose{
    clampPanTargetDeg(pose.panDeg),
    clampTiltTargetDeg(pose.tiltDeg),
  };
}

static bool updateDefaultAimAxis(
  JsonObject payload,
  const char* key,
  float& target,
  bool panAxis
) {
  if (!payload.containsKey(key)) {
    return false;
  }
  const float requested = payload[key].as<float>();
  if (!isfinite(requested)) {
    DEBUG_SERIAL.printf("[LAMP_AIM] ignore invalid default angle: %s\n", key);
    return false;
  }
  const float normalized = panAxis
    ? clampPanTargetDeg(requested)
    : clampTiltTargetDeg(requested);
  if (fabs(target - normalized) <= 0.001f) {
    return false;
  }
  target = normalized;
  return true;
}

static bool updateConfiguredDefaultAims(JsonObject payload) {
  bool changed = false;
  changed = updateDefaultAimAxis(
    payload, "garmentDefaultPan", lampAimState.defaultGarment.panDeg, true
  ) || changed;
  changed = updateDefaultAimAxis(
    payload, "garmentDefaultTilt", lampAimState.defaultGarment.tiltDeg, false
  ) || changed;
  changed = updateDefaultAimAxis(
    payload, "personDefaultPan", lampAimState.defaultPerson.panDeg, true
  ) || changed;
  changed = updateDefaultAimAxis(
    payload, "personDefaultTilt", lampAimState.defaultPerson.tiltDeg, false
  ) || changed;

  if (changed) {
    if (!lampAimState.garmentTargetValid) {
      lampAimState.garment = lampAimState.defaultGarment;
    }
    if (!lampAimState.personTargetValid) {
      lampAimState.person = lampAimState.defaultPerson;
    }
  }
  return changed;
}

static void applySelectedLampAim(const char* reason, bool force = false) {
  LampAimSelection selected = selectLampAim(lampAimState);
  selected.pose = normalizeLampAimPose(selected.pose);
  if (!force && appliedAimInitialized && sameLampAimSelection(selected, lastAppliedAim)) {
    return;
  }

  stopArmJoystickMotion();
  panDeg = (int)round(selected.pose.panDeg);
  tiltDeg = (int)round(selected.pose.tiltDeg);
  sendPanTarget((float)panDeg);
  sendTiltTarget((float)tiltDeg);
  lastAppliedAim = selected;
  appliedAimInitialized = true;
  diagnosticRecordArm(DIAG_SOURCE_WS, lampAimSourceName(selected.source), panDeg, tiltDeg, sliderMm);
  DEBUG_SERIAL.printf(
    "[LAMP_AIM] source=%s reason=%s pan=%d tilt=%d slider=%d\n",
    lampAimSourceName(selected.source),
    reason ? reason : "state_changed",
    panDeg,
    tiltDeg,
    sliderMm
  );
}

static void cacheSelectedLampAimWithoutMotion() {
  LampAimSelection selected = selectLampAim(lampAimState);
  selected.pose = normalizeLampAimPose(selected.pose);
  lastAppliedAim = selected;
  appliedAimInitialized = true;
  DEBUG_SERIAL.printf(
    "[LAMP_AIM] initial state cached without motion source=%s pan=%.1f tilt=%.1f\n",
    lampAimSourceName(selected.source),
    selected.pose.panDeg,
    selected.pose.tiltDeg
  );
}

static void syncSelectedLampAim(const char* reason) {
  const LampAimSyncAction syncAction = lampAimSyncGate.onState(
    lampAimState.garmentTrackingEnabled
  );
  if (syncAction == LampAimSyncAction::CacheOnly) {
    cacheSelectedLampAimWithoutMotion();
    return;
  }
  applySelectedLampAim(reason, syncAction == LampAimSyncAction::ForceApply);
}

static void sendCollisionGuardStatus(const String& guardId, const char* status) {
  if (!wsConnected) return;
  StaticJsonDocument<224> doc;
  doc["type"] = "collisionGuardStatus";
  doc["chipId"] = deviceId;
  doc["guardId"] = guardId;
  doc["status"] = status ? status : "unknown";
  doc["pan"] = panDeg;
  doc["tilt"] = tiltDeg;
  doc["nanoFeedback"] = false;
  String msg;
  serializeJson(doc, msg);
  webSocket.sendTXT(msg);
}

static void handleGarmentAimState(JsonObject payload) {
  const bool defaultsChanged = updateConfiguredDefaultAims(payload);
  if (!payload.containsKey("garmentAimEnabled")) {
    if (defaultsChanged) {
      syncSelectedLampAim("default_aim_changed");
    }
    return;
  }

  const bool nextEnabled = payload["garmentAimEnabled"] | false;
  bool targetValid = nextEnabled && (payload["garmentTargetValid"] | false);
  float centerX = 0.5f;
  float centerY = 0.5f;
  bool calibrationValid = false;
  float calibratedPan = lampAimState.defaultGarment.panDeg;
  float calibratedTilt = lampAimState.defaultGarment.tiltDeg;

  if (targetValid) {
    if (!payload.containsKey("garmentCenterX") || !payload.containsKey("garmentCenterY")) {
      targetValid = false;
    } else {
      centerX = payload["garmentCenterX"].as<float>();
      centerY = payload["garmentCenterY"].as<float>();
      targetValid = isfinite(centerX) && isfinite(centerY)
        && centerX >= 0.0f && centerX <= 1.0f
        && centerY >= 0.0f && centerY <= 1.0f;
    }
  }

  if (targetValid && (payload["garmentCalibrationValid"] | false)) {
    if (payload.containsKey("garmentAimPan")
        && payload.containsKey("garmentAimTilt")) {
      calibratedPan = payload["garmentAimPan"].as<float>();
      calibratedTilt = payload["garmentAimTilt"].as<float>();
      calibrationValid = isValidCalibratedGarmentAimPose(
        calibratedPan, calibratedTilt
      );
    }
  }

  lampAimState.garmentTrackingEnabled = nextEnabled;
  lampAimState.garmentTargetValid = targetValid;
  if (targetValid) {
    if (calibrationValid) {
      lampAimState.garment = LampAimPose{
        calibratedPan, calibratedTilt,
      };
    } else {
      GarmentAimTarget target = calculateGarmentAimTarget(
        centerX,
        centerY,
        GarmentAimConfig{
          lampAimState.defaultGarment.panDeg,
          lampAimState.defaultGarment.tiltDeg,
          GARMENT_AIM_HORIZONTAL_FOV_DEG,
          GARMENT_AIM_VERTICAL_FOV_DEG,
        }
      );
      lampAimState.garment = LampAimPose{
        target.panDeg, target.tiltDeg,
      };
    }
  } else if (nextEnabled) {
    DEBUG_SERIAL.println("[GARMENT_AIM] no reliable detected target, using default preset");
  }

  syncSelectedLampAim(
    defaultsChanged ? "garment_state_defaults_changed" : "garment_state"
  );
}

void startPersonTrackingAim() {
  lampAimState.personTrackingActive = true;
  lampAimState.personTargetValid = false;
  lampAimState.person = lampAimState.defaultPerson;
  applySelectedLampAim("person_tracking_start", true);
}

bool updatePersonTrackingAim(
  bool hasPan,
  float pan,
  bool hasTilt,
  float tilt
) {
  if (!lampAimState.personTrackingActive || (!hasPan && !hasTilt)) {
    return false;
  }
  if ((hasPan && !isfinite(pan))
      || (hasTilt && !isfinite(tilt))) {
    return false;
  }

  LampAimPose next = lampAimState.personTargetValid
    ? lampAimState.person
    : lampAimState.defaultPerson;
  if (hasPan) next.panDeg = pan;
  if (hasTilt) next.tiltDeg = tilt;
  lampAimState.person = next;
  lampAimState.personTargetValid = true;
  applySelectedLampAim("person_tracking_update");
  return true;
}

void stopPersonTrackingAim() {
  if (!lampAimState.personTrackingActive) {
    return;
  }
  lampAimState.personTrackingActive = false;
  lampAimState.personTargetValid = false;
  applySelectedLampAim("person_tracking_stop", true);
}

bool isPersonTrackingAimActive() {
  return lampAimState.personTrackingActive;
}

void getDefaultGarmentAim(float& pan, float& tilt) {
  pan = lampAimState.defaultGarment.panDeg;
  tilt = lampAimState.defaultGarment.tiltDeg;
}

void getDefaultPersonAim(float& pan, float& tilt) {
  pan = lampAimState.defaultPerson.panDeg;
  tilt = lampAimState.defaultPerson.tiltDeg;
}

void sendWsPing() {
  if (WiFi.status() != WL_CONNECTED || !wsConnected) {
    return;
  }

  StaticJsonDocument<96> doc;
  doc["type"] = "ping";
  doc["id"] = deviceId;
  doc["chipId"] = deviceId;

  String pingMsg;
  serializeJson(doc, pingMsg);
  webSocket.sendTXT(pingMsg);
  lastPing = millis();

#if LOG_WS_HEARTBEAT
  DEBUG_SERIAL.println("发送 WebSocket 心跳: " + pingMsg);
#endif
}

void handleWsHeartbeat() {
  if (millis() - lastPing >= wsPingInterval) {
    sendWsPing();
  }
}

void sendWsRegister() {
  StaticJsonDocument<1024> doc;
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
  LOG_INFO("WS", "WS 注册消息已发送");
}

void handleWsMessage(const String& text) {
  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, text);
  if (err) {
    DEBUG_SERIAL.println("[WS] JSON解析失败");
    diagnosticLogWs("json parse failed", true);
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

  if (type == "pong") {
#if LOG_WS_HEARTBEAT
    DEBUG_SERIAL.println("[WS] 收到消息: " + text);
#endif
    return;
  }

  DEBUG_SERIAL.println("[WS] 收到消息: " + text);

  if (type == "state" || type == "control") {
    String targetId = payload["id"] | "";
    String chipId = payload["chipId"] | "";

    if (targetId != deviceId && chipId != deviceId) {
      DEBUG_SERIAL.println("[WS] state 不是发给本设备，忽略");
      return;
    }

    LightState before = diagnosticCurrentLightState();
    stopEffectWaveForManualControl();
    stopLocateBreath(false);

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
    handleGarmentAimState(payload);
    float startupDefaultPan = 0.0f;
    float startupDefaultTilt = 0.0f;
    getDefaultGarmentAim(startupDefaultPan, startupDefaultTilt);
    markNanoStartupAimReady(startupDefaultPan, startupDefaultTilt);

    DEBUG_SERIAL.printf("WS控制：亮度=%d 色温=%d 自动=%d 推荐亮度=%d 推荐色温=%d 面料=%s\n",
                  brightness, temp, autoMode,
                  recommendedBrightness, recommendedTemp, fabric);
    diagnosticLogLightChange("ws", before, diagnosticCurrentLightState());
    return;
  }

  if (type == "effect") {
    String effect = payload["effect"] | "";
    bool enabled = payload["enabled"] | false;

    if (effect != "wave") {
      DEBUG_SERIAL.println("[EFFECT] unsupported effect: " + effect);
      diagnosticLogWs("unsupported effect", true);
      return;
    }

    stopLocateBreath(false);

    if (enabled) {
      LightState before = diagnosticCurrentLightState();
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
      requestDeviceStateReport("WS_EFFECT_START");
      diagnosticLogLightChange("effect", before, diagnosticCurrentLightState());

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
      requestDeviceStateReport("WS_EFFECT_STOP");
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

    startLocateBreath(times, cycleMs);
    return;
  }

  if (type == "arm_joystick") {
    if (lampAimState.collisionParkActive) {
      stopArmJoystickMotion();
      DEBUG_SERIAL.println("[COLLISION] joystick ignored while parked");
      return;
    }
    float x = payload["x"] | 0.0f;
    float y = payload["y"] | 0.0f;

    setArmJoystickMotion(x, y);
    diagnosticRecordArm(DIAG_SOURCE_WS, "joystick", panDeg, tiltDeg, sliderMm);
    return;
  }

  if (type == "arm_stop") {
    stopArmJoystickMotion();
    return;
  }

  if (type == "lampTrackingStart") {
    startPersonTrackingAim();
    DEBUG_SERIAL.println("[LAMP_AIM] person tracking session started");
    return;
  }

  if (type == "lampTrackingStop") {
    stopPersonTrackingAim();
    if (payload["clearClothTaken"] | false) {
      clearClothTakenState();
    }
    DEBUG_SERIAL.println("[LAMP_AIM] person tracking session stopped; base aim restored");
    return;
  }

  if (type == "lampCollisionGuard") {
    String action = payload["action"] | "park";
    String guardId = payload["guardId"] | "";
    action.trim();
    guardId.trim();
    if (guardId.length() == 0 || guardId.length() > 64) {
      DEBUG_SERIAL.println("[COLLISION] invalid guardId");
      return;
    }
    if (action == "release") {
      if (activeCollisionGuardId.length() > 0 && activeCollisionGuardId != guardId) {
        DEBUG_SERIAL.println("[COLLISION] stale release ignored");
        return;
      }
      lampAimState.collisionParkActive = false;
      activeCollisionGuardId = "";
      applySelectedLampAim("collision_release", true);
      sendCollisionGuardStatus(guardId, "released");
      return;
    }

    activeCollisionGuardId = guardId;
    lampAimState.collisionParkActive = true;
    applySelectedLampAim("collision_park", true);
    // accepted means ESP8266 forwarded pan=0/tilt=0. Nano cannot report arrival.
    sendCollisionGuardStatus(guardId, "accepted");
    return;
  }

  if (type == "arm_position") {
    bool changed = false;

    stopArmJoystickMotion();

    if (!lampAimState.collisionParkActive && payload.containsKey("pan")) {
      panDeg = (int)clampPanTargetDeg(payload["pan"].as<int>());
      sendPanTarget(panDeg);
      changed = true;
    }

    if (!lampAimState.collisionParkActive && payload.containsKey("tilt")) {
      tiltDeg = (int)clampTiltTargetDeg(payload["tilt"].as<int>());
      sendTiltTarget(tiltDeg);
      changed = true;
    }

    if (payload.containsKey("slider")) {
      sliderMm = payload["slider"].as<int>();
      sliderMm = constrain(sliderMm, SLIDER_MIN, SLIDER_MAX);
      sendNano(
        'x',
        String((float)sliderMm, 2)
      );
      changed = true;
    }

    if (!changed) {
      DEBUG_SERIAL.println("[ARM] arm_position missing pan/tilt/slider");
      diagnosticLogWs("arm_position missing fields", true);
    } else {
      diagnosticRecordArm(DIAG_SOURCE_WS, "position", panDeg, tiltDeg, sliderMm);
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
    sendArmSpeedStatus("ws_command");
    DEBUG_SERIAL.println("[ARM] speed changed: " + speed);
    diagnosticRecordArm(DIAG_SOURCE_WS, "speed", panDeg, tiltDeg, sliderMm);
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
      diagnosticLogWs("arm action missing", true);
      return;
    }

    if (lampAimState.collisionParkActive) {
      DEBUG_SERIAL.println("[COLLISION] manual arm action ignored while parked");
      return;
    }
    if (handleArmAction(action)) {
      diagnosticRecordArm(DIAG_SOURCE_WS, action.c_str(), panDeg, tiltDeg, sliderMm);
    }
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
      diagnosticLogWs("unknown command", true);
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
      diagnosticLogWs("invalid OTA message", true);
      otaStatus = "failed";
      requestDeviceStateReport("OTA_INVALID");
      return;
    }

    bool sameChannel = (channel == String(FW_CHANNEL));

    if (sameChannel && versionCode <= FW_VERSION_CODE) {
      DEBUG_SERIAL.println("[OTA] Same channel and target versionCode is not newer, ignore");
      otaStatus = "idle";
      otaProgress = 0;
      requestDeviceStateReport("OTA_NOT_NEWER");
      return;
    }

    if (!sameChannel) {
      DEBUG_SERIAL.println("[OTA] Cross-channel OTA allowed");
    }

    stopLocateBreath(true);
    doOtaUpdate(url, version, versionCode, channel, md5);
    return;
  }

  if (type == "registerAck" || type == "register_ack" || type == "register_ok") {
    DEBUG_SERIAL.println("[WS] register ack received");
    sendArmSpeedStatus("register_ack");
    return;
  }

  DEBUG_SERIAL.println("[WS] 未处理消息类型: " + type);
  diagnosticLogWs((String("unhandled type=") + type).c_str(), true);
}

void sendLampClothState(const char* clothState, bool tracking, const char* lastTakenAt) {
  if (!wsConnected) return;

  StaticJsonDocument<192> doc;
  doc["type"] = "lampClothState";
  doc["chipId"] = deviceId;
  doc["clothState"] = clothState ? clothState : "unknown";
  doc["tracking"] = tracking;
  if (lastTakenAt && strlen(lastTakenAt) > 0) {
    doc["lastTakenAt"] = lastTakenAt;
  }

  String msg;
  serializeJson(doc, msg);
  webSocket.sendTXT(msg);
  DEBUG_SERIAL.println("[WS] lamp cloth state: " + msg);
}

void sendLampProximityState(bool nearby) {
  if (!wsConnected) return;

  StaticJsonDocument<128> doc;
  doc["type"] = "lampProximityState";
  doc["chipId"] = deviceId;
  doc["nearby"] = nearby;

  String msg;
  serializeJson(doc, msg);
  webSocket.sendTXT(msg);
  DEBUG_SERIAL.println("[WS] lamp proximity state: " + msg);
}

void sendArmSpeedStatus(const char* reason) {
  if (!wsConnected) return;
  StaticJsonDocument<192> doc;
  doc["type"] = "armSpeedStatus";
  doc["chipId"] = deviceId;
  doc["speed"] = currentArmSpeed;
  doc["reason"] = reason ? reason : "applied";
  doc["nanoFeedback"] = false;
  String msg;
  serializeJson(doc, msg);
  webSocket.sendTXT(msg);
  DEBUG_SERIAL.println("[WS] arm speed status: " + msg);
}

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      wsConnected = false;
      DEBUG_SERIAL.print("[WS] 已断开");
      if (payload && length > 0) {
        DEBUG_SERIAL.print(": ");
        DEBUG_SERIAL.write(payload, length);
      }
      DEBUG_SERIAL.println();
      {
        char reason[41];
        size_t reasonLength = min(length, sizeof(reason) - 1);
        for (size_t i = 0; i < reasonLength; ++i) {
          char ch = payload ? (char)payload[i] : '\0';
          reason[i] = (ch >= 32 && ch <= 126) ? ch : '.';
        }
        reason[reasonLength] = '\0';
        char message[96];
        snprintf(
          message,
          sizeof(message),
          "disconnected up=%lums reason=%s",
          (unsigned long)(millis() - lastWsConnectedMs),
          reason
        );
        diagnosticLogWs(message, true);
      }
      break;

    case WStype_CONNECTED:
      wsConnected = true;
      lastWsConnectedMs = millis();
      diagnosticNoteWsConnected();
      DEBUG_SERIAL.printf("[WS] 已连接: %s\n", payload);
      diagnosticLogWs((String("connected host=") + cfg.serverHost).c_str());
      sendWsRegister();
      sendWsPing();
      sendCurrentLampProximityState();
      if (!bootOnlineReportDone && !bootOnlineReportRequested) {
        bootOnlineReportRequested = true;
        requestDeviceStateReport("WS_CONNECTED");
      }
      if (!bootSelfTestStarted) {
        bootSelfTestStarted = true;
        startDeviceSelfTest();
      }
      break;

    case WStype_TEXT:
      handleWsMessage(String((char*)payload));
      break;

    case WStype_PONG:
#if LOG_WS_HEARTBEAT
      DEBUG_SERIAL.println("[WS] PONG");
#endif
      break;

    default:
      break;
  }
}

void beginWebSocketClient() {
  wsClientStarted = true;
  webSocket.disconnect();
  delay(100);

  DEBUG_SERIAL.println("[WS] 准备连接:");
  DEBUG_SERIAL.println("host = " + cfg.serverHost);
  DEBUG_SERIAL.println("port = " + String(cfg.wsPort));
  DEBUG_SERIAL.println("path = " + String(WS_PATH));
  DEBUG_SERIAL.println("url  = ws://" + cfg.serverHost + ":" + String(cfg.wsPort) + String(WS_PATH));

  webSocket.begin(cfg.serverHost.c_str(), cfg.wsPort, WS_PATH);
  // Remove the library's default "Origin: file://" header; Spring rejects it.
  webSocket.setExtraHeaders();
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
  webSocket.enableHeartbeat(
    WS_HEARTBEAT_PING_INTERVAL_MS,
    WS_HEARTBEAT_PONG_TIMEOUT_MS,
    WS_HEARTBEAT_DISCONNECT_COUNT
  );
}
