#include "device/arm_controller.h"
#include "device/lamp_aim_state.h"
#include "network/ws_client.h"
#include "diagnostics/diagnostic_logger.h"

// Current speed preset.
String currentArmSpeed = "normal";

// Continuous joystick motion state.
bool armJoystickActive = false;
unsigned long joystickExpireAt = 0;
unsigned long lastArmMotionUpdateAt = 0;
bool nanoLineSeen = false;
String lastNanoLine = "";
unsigned long lastNanoRxAt = 0;
bool lastNanoHomingOk = false;
bool lastNanoHallStatusOk = false;
bool nanoEnableStateKnown = false;
bool nanoEnabled = true;
bool nanoSliderArrived = false;
float nanoSliderArrivedTargetMm = 0.0f;
long nanoSliderArrivedPositionSteps = 0;
unsigned long nanoSliderArrivedAt = 0;

static const float JOYSTICK_INPUT_DEADZONE = 0.05f;
static const float JOYSTICK_STEP_EPS_DEG = 0.02f;
static const unsigned long JOYSTICK_PACKET_DT_FALLBACK_MS = 120;
static const unsigned long JOYSTICK_PACKET_DT_MIN_MS = 30;
static const unsigned long JOYSTICK_PACKET_DT_MAX_MS = 220;
static const unsigned long JOYSTICK_IDLE_RELEASE_MS = 400;
static const float TILT_COMMAND_RATIO =30.0f / 48.0f;
static const unsigned long NANO_STATUS_PROBE_TIMEOUT_MS = 2000;
static const unsigned long NANO_STATUS_PROBE_RETRY_MS = 300;
static const uint8_t NANO_STATUS_PROBE_MAX_ATTEMPTS = 3;
static const float NANO_BOOT_REFERENCE_TOLERANCE_DEG = 0.25f;

static float panEstimateDeg = 0.0f;
static float tiltEstimateDeg = 0.0f;
static bool nanoStartupEnableRecovered = false;
static bool nanoStartupEnableRecoveryPending = false;
static NanoBootSession nanoBootSession;
static NanoStatusProbe nanoStatusProbe;
static NanoStatusSnapshot nanoStatusSnapshot{};
static String nanoStatusLine = "";
static float nanoStartupAimPan = 0.0f;
static float nanoStartupAimTilt = 0.0f;
static bool nanoSliderArrivalReportPending = false;
static bool nanoSliderCaptureTaskPending = false;
static String nanoSliderCaptureTaskId = "";
static float nanoSliderCaptureTargetMm = 0.0f;

static const float SLIDER_CAPTURE_TARGET_TOLERANCE_MM = 0.05f;

static bool parseNanoEnableStateLine(const String& line, bool& enabledOut) {
  const char* cstr = line.c_str();
  const char* marker = strstr(cstr, "Enable state:");
  if (!marker) {
    return false;
  }
  marker += 13;  // skip "Enable state:"
  while (*marker == ' ') marker++;  // skip whitespace (inline trim)
  if (*marker == '\0') {
    return false;
  }
  enabledOut = (*marker != '0');
  return true;
}

float clampPanTargetDeg(float value) {
  return constrain(value, (float)PAN_MIN, (float)PAN_MAX);
}

float clampTiltTargetDeg(float value) {
  return constrain(value, (float)TILT_MIN, (float)TILT_MAX);
}

static float toTiltNanoCommandDeg(float valueDeg) {
  return clampTiltTargetDeg(valueDeg) * TILT_COMMAND_RATIO;
}

static void sendNanoFloat(char cmd, float value) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%.2f", value);
  sendNano(cmd, buf);
}

void sendPanTarget(float valueDeg) {
  sendNanoFloat('p', clampPanTargetDeg(valueDeg));
}

void sendTiltTarget(float valueDeg) {
  sendNanoFloat('t', toTiltNanoCommandDeg(valueDeg));
}

void sendPanSpeed(float valueDegPerSec) {
  sendNanoFloat('s', max(0.0f, valueDegPerSec));
}

void sendTiltSpeed(float valueDegPerSec) {
  sendNanoFloat('S', max(0.0f, valueDegPerSec) * TILT_COMMAND_RATIO);
}

void sendNanoBootFinish(float garmentPan, float garmentTilt) {
  const float nanoPan = nanoBootPanCommand(garmentPan);
  const float nanoTilt = nanoBootTiltCommand(garmentTilt);

  char value[32];
  snprintf(value, sizeof(value), ",%.2f,%.2f", nanoPan, nanoTilt);
  sendNano('F', String(value));
}

void ensureNanoStatusProbe() {
  const unsigned long now = millis();
  if (nanoStatusProbe.result() == NanoProbeResult::Idle) {
    nanoStatusProbe.begin(now);
  }
  if (nanoStatusProbe.takeSendRequest(
        now,
        NANO_STATUS_PROBE_RETRY_MS,
        NANO_STATUS_PROBE_MAX_ATTEMPTS
      )) {
    sendNano('Q');
  }
}

NanoProbeResult getNanoStatusProbeResult() {
  return nanoStatusProbe.result();
}

String getNanoStatusProbeLine() {
  return nanoStatusLine;
}

void markNanoStartupAimReady(float garmentPan, float garmentTilt) {
  if (nanoBootSession.startupAimReady()) {
    return;
  }
  nanoStartupAimPan = garmentPan;
  nanoStartupAimTilt = garmentTilt;
  nanoBootSession.markStartupAimReady();
}

static void dispatchNanoBootAction() {
  const NanoBootAction action = nanoBootSession.pendingAction();
  if (action == NanoBootAction::None) {
    return;
  }

  nanoBootSession.markActionSent(action);
  switch (action) {
    case NanoBootAction::SendBoot:
      // Nano now starts its animation autonomously. Keep the enum branch only
      // for protocol compatibility; pendingAction() no longer emits it.
      return;
    case NanoBootAction::SendFinish:
      DEBUG_SERIAL.printf(
        "[NANO] startup aim ready pan=%.2f tilt=%.2f\n",
        nanoStartupAimPan,
        nanoStartupAimTilt
      );
      sendNanoBootFinish(nanoStartupAimPan, nanoStartupAimTilt);
      return;
    case NanoBootAction::SendCancel:
      DEBUG_SERIAL.println("[NANO] cancelling boot animation before OTA");
      sendNano('C');
      return;
    case NanoBootAction::None:
      return;
  }
}

void beginNanoOtaCancel() {
  nanoBootSession.beginOtaCancel();
  dispatchNanoBootAction();
}

void resumeNanoAfterOtaFailure() {
  nanoBootSession.resumeAfterOtaFailure();
}

void handleNanoStartupSync() {
  const unsigned long now = millis();
  if (nanoStatusProbe.updateTimeout(now, NANO_STATUS_PROBE_TIMEOUT_MS)) {
    DEBUG_SERIAL.println("[NANO] nano status timeout");
    diagnosticLogNano("nano status timeout", true);
  } else if (nanoStatusProbe.result() == NanoProbeResult::Pending) {
    ensureNanoStatusProbe();
  }

  dispatchNanoBootAction();

  if (nanoStartupEnableRecoveryPending) {
    nanoStartupEnableRecoveryPending = false;
    DEBUG_SERIAL.println("[NANO] startup recovery: re-enabling drivers");
    diagnosticLogNano("re-enabling disabled drivers");
    sendNano('e');
  }

}

static bool joystickAxisActive(float value) {
  return fabs(value) > JOYSTICK_INPUT_DEADZONE;
}

static unsigned long clampJoystickPacketDt(unsigned long rawMs) {
  return (unsigned long)constrain(
    (long)rawMs,
    (long)JOYSTICK_PACKET_DT_MIN_MS,
    (long)JOYSTICK_PACKET_DT_MAX_MS
  );
}

void sendNano(char cmd, const String& value, const String& captureTaskId) {
  if (cmd == 'x') {
    nanoSliderArrived = false;
    nanoSliderArrivalReportPending = false;
    nanoSliderCaptureTaskId = captureTaskId;
    nanoSliderCaptureTaskId.trim();
    nanoSliderCaptureTaskPending = nanoSliderCaptureTaskId.length() > 0;
    nanoSliderCaptureTargetMm = value.toFloat();
  }

  nanoSerial.print(cmd);
  if (value.length() > 0) {
    nanoSerial.print(value);
  }
  nanoSerial.print('\n');

  delay(25);
  pollNano();

  DEBUG_SERIAL.print("[NANO] TX ");
  DEBUG_SERIAL.print(cmd);
  DEBUG_SERIAL.println(value);
}

static bool readNanoField(
    const String& line,
    const char* key,
    String& valueOut) {
  String marker = String(key) + "=";
  int start = line.indexOf(marker);
  if (start < 0) {
    return false;
  }

  start += marker.length();
  int end = line.indexOf(' ', start);
  if (end < 0) {
    end = line.length();
  }

  valueOut = line.substring(start, end);
  valueOut.trim();
  return valueOut.length() > 0;
}

static void reportNanoSliderArrival() {
  const bool captureTaskArrival = nanoSliderCaptureTaskPending &&
    fabs(nanoSliderArrivedTargetMm - nanoSliderCaptureTargetMm) <=
      SLIDER_CAPTURE_TARGET_TOLERANCE_MM;

  StaticJsonDocument<320> doc;
  doc["type"] = "sliderStatus";
  doc["chipId"] = deviceId;
  doc["status"] = "arrived";
  doc["targetMm"] = nanoSliderArrivedTargetMm;
  doc["positionSteps"] = nanoSliderArrivedPositionSteps;
  doc["uptimeMs"] = nanoSliderArrivedAt;
  if (captureTaskArrival) {
    doc["taskId"] = nanoSliderCaptureTaskId;
  }

  if (!wsConnected) {
    DEBUG_SERIAL.println("[NANO] slider arrival not reported: WS disconnected");
    diagnosticLogNano("slider arrival not reported: WS disconnected", true);
    return;
  }

  String message;
  serializeJson(doc, message);
  webSocket.sendTXT(message);
  nanoSliderArrivalReportPending = false;
  if (captureTaskArrival) {
    nanoSliderCaptureTaskPending = false;
    nanoSliderCaptureTaskId = "";
  }
}

void reportPendingNanoSliderArrival() {
  if (nanoSliderArrivalReportPending) {
    reportNanoSliderArrival();
  }
}

static void handleNanoSliderDoneLine(const String& line) {
  if (!line.startsWith("SLIDER_DONE ")) {
    return;
  }

  String targetText;
  String positionText;
  if (
    !readNanoField(line, "target_mm", targetText) ||
    !readNanoField(line, "pos_steps", positionText)
  ) {
    DEBUG_SERIAL.println("[NANO] malformed SLIDER_DONE");
    diagnosticLogNano("malformed SLIDER_DONE", true);
    return;
  }

  nanoSliderArrivedTargetMm = targetText.toFloat();
  nanoSliderArrivedPositionSteps = positionText.toInt();
  nanoSliderArrivedAt = millis();
  nanoSliderArrived = true;
  nanoSliderArrivalReportPending = true;

  DEBUG_SERIAL.println(
    "[NANO] slider arrived target=" +
    String(nanoSliderArrivedTargetMm, 2) +
    " posSteps=" +
    String(nanoSliderArrivedPositionSteps)
  );
  reportNanoSliderArrival();
}

static void handleNanoProtocolLine(const String& line) {
  const bool hadNanoBootSession = nanoBootSession.readySeen()
    || nanoBootSession.bootRequested()
    || nanoBootSession.finishSent();
  if (nanoBootSession.noteInitializationLine(line.c_str()) && hadNanoBootSession) {
    DEBUG_SERIAL.println("[NANO] new initialization detected");
    nanoStatusProbe.reset();
    nanoStatusSnapshot = NanoStatusSnapshot{};
    nanoStatusLine = "";
    nanoEnableStateKnown = false;
    nanoEnabled = true;
    nanoStartupEnableRecovered = false;
    nanoStartupEnableRecoveryPending = false;
    // Nano starts its own animation. Do not wait for READY/Q/B before sending F.
  }

  if (acceptNanoReadyLine(
        nanoBootSession,
        nanoStatusProbe,
        line.c_str()
      )) {
    nanoStatusSnapshot = NanoStatusSnapshot{};
    nanoStatusLine = "";
  }

  const NanoBootReply bootReply = nanoBootSession.onBootReply(line.c_str());
  if (bootReply == NanoBootReply::RejectedReference) {
    DEBUG_SERIAL.println("[NANO] boot animation rejected: not at power-on reference");
    diagnosticLogNano("boot rejected: not at power-on reference", true);
  } else if (bootReply == NanoBootReply::RejectedOther) {
    diagnosticLogNano(line.c_str(), true);
  }

  NanoStatusSnapshot parsedStatus{};
  if (parseNanoStatusLine(line.c_str(), parsedStatus)) {
    nanoStatusSnapshot = parsedStatus;
    nanoStatusLine = line;
    nanoStatusProbe.succeed();

    if (nanoStatusSnapshot.hasEnabled) {
      nanoEnableStateKnown = true;
      nanoEnabled = nanoStatusSnapshot.enabled;
      if (!nanoEnabled && !nanoStartupEnableRecovered) {
        nanoStartupEnableRecovered = true;
        nanoStartupEnableRecoveryPending = true;
      }
    }
    if (nanoStatusSnapshot.hasBoot) {
      const bool recoveredReady = nanoBootSession.recoverReadyFromStatus(
        nanoStatusSnapshot,
        NANO_BOOT_REFERENCE_TOLERANCE_DEG
      );
      nanoBootSession.noteNanoBootState(nanoStatusSnapshot.boot);
      if (recoveredReady) {
        DEBUG_SERIAL.println(
          "[NANO] recovered missed READY from safe Q status"
        );
      }
    }

    DEBUG_SERIAL.printf(
      "[NANO] status en=%d moving=%d boot=%s pan=%.2f tilt=%.2f timeout=%d\n",
      nanoStatusSnapshot.hasEnabled ? (nanoStatusSnapshot.enabled ? 1 : 0) : -1,
      nanoStatusSnapshot.hasMoving ? (nanoStatusSnapshot.moving ? 1 : 0) : -1,
      nanoStatusSnapshot.hasBoot ? nanoStatusSnapshot.boot : "unknown",
      nanoStatusSnapshot.hasPan ? nanoStatusSnapshot.pan : 0.0f,
      nanoStatusSnapshot.hasTilt ? nanoStatusSnapshot.tilt : 0.0f,
      nanoStatusSnapshot.hasTimeout ? (nanoStatusSnapshot.timedOut ? 1 : 0) : -1
    );
  }

  if (line.startsWith("ERR F ") || line.startsWith("ERR C ")) {
    diagnosticLogNano(line.c_str(), true);
  }
}

void pollNano() {
  static String line;

  while (nanoSerial.available() > 0) {
    char ch = (char)nanoSerial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      if (line.length() > 0) {
        const String completeLine = line;
        line = "";
        lastNanoLine = completeLine;
        lastNanoRxAt = millis();
        nanoLineSeen = true;
        DEBUG_SERIAL.println("[NANO] RX " + completeLine);
        handleNanoSliderDoneLine(completeLine);
        handleNanoProtocolLine(completeLine);

        bool parsedEnabled = true;
        if (parseNanoEnableStateLine(completeLine, parsedEnabled)) {
          nanoEnableStateKnown = true;
          nanoEnabled = parsedEnabled;
          DEBUG_SERIAL.printf("[NANO] enable=%d\n", nanoEnabled ? 1 : 0);

          if (!nanoEnabled && !nanoStartupEnableRecovered) {
            nanoStartupEnableRecovered = true;
            nanoStartupEnableRecoveryPending = true;
          }
        } else if (completeLine == "Enabled") {
          nanoEnableStateKnown = true;
          nanoEnabled = true;
        } else if (completeLine == "Disabled") {
          nanoEnableStateKnown = true;
          nanoEnabled = false;
        }
      }
      continue;
    }

    if (line.length() < 120) {
      line += ch;
    } else {
      line = "";
      DEBUG_SERIAL.println("[NANO] RX line too long, dropped");
      diagnosticLogNano("RX line too long", true);
    }
  }
}

void sendPanTilt() {
  panDeg = (int)round(clampPanTargetDeg(panDeg));
  tiltDeg = (int)round(clampTiltTargetDeg(tiltDeg));
  sendPanTarget(panDeg);
  sendTiltTarget(tiltDeg);
}

void sendSlider() {
  sliderMm = constrain(sliderMm, SLIDER_MIN, SLIDER_MAX);
  sendNanoFloat('x', (float)sliderMm);
}

void applyArmSpeed(const String& speed) {
  String normalized = speed;
  normalized.trim();
  normalized.toLowerCase();

  if (normalized == "slow") {
    angleStep = 5;
    sliderStep = 20;
    panSpeedDeg = 20;
    tiltSpeedDeg = 30;
    sliderSpeedMm = 120;
  } else if (normalized == "fast") {
    angleStep = 12;
    sliderStep = 45;
    panSpeedDeg = 45;
    tiltSpeedDeg = 65;
    sliderSpeedMm = 270;
  } else {
    // normal (原 fast 档)
    normalized = "normal";
    angleStep = 8;
    sliderStep = 30;
    panSpeedDeg = 30;
    tiltSpeedDeg = 45;
    sliderSpeedMm = 180;
  }

  currentArmSpeed = normalized;

  sendPanSpeed((float)panSpeedDeg);
  sendTiltSpeed((float)tiltSpeedDeg);
  sendNanoFloat('X', (float)sliderSpeedMm);
}

void getArmJoystickMaxSpeed(float& maxPanSpeed, float& maxTiltSpeed) {
  if (currentArmSpeed == "slow") {
    maxPanSpeed = 15.0f;
    maxTiltSpeed = 10.0f;
  } else if (currentArmSpeed == "fast") {
    maxPanSpeed = 33.0f;
    maxTiltSpeed = 22.0f;
  } else {
    // normal (原 fast 档)
    maxPanSpeed = 22.0f;
    maxTiltSpeed = 15.0f;
  }
}

void setArmJoystickMotion(float x, float y) {
  x = constrain(x, -1.0f, 1.0f);
  y = constrain(y, -1.0f, 1.0f);
  unsigned long now = millis();

  float maxPanSpeed = 8.0f;
  float maxTiltSpeed = 5.0f;
  getArmJoystickMaxSpeed(maxPanSpeed, maxTiltSpeed);

  if (!armJoystickActive) {
    panEstimateDeg = panDeg;
    tiltEstimateDeg = tiltDeg;
  }

  float panVel = x * maxPanSpeed;
  float tiltVel = y * maxTiltSpeed;

  if (!joystickAxisActive(x) && !joystickAxisActive(y)) {
    stopArmJoystickMotion();
    DEBUG_SERIAL.println("[ARM] joystick neutral, release control");
    return;
  }

  unsigned long dtMs = JOYSTICK_PACKET_DT_FALLBACK_MS;
  if (lastArmMotionUpdateAt > 0) {
    dtMs = clampJoystickPacketDt(now - lastArmMotionUpdateAt);
  }

  float panDeltaDeg = panVel * ((float)dtMs / 1000.0f);
  float tiltDeltaDeg = tiltVel * ((float)dtMs / 1000.0f);
  bool sent = false;

  if (fabs(panDeltaDeg) >= JOYSTICK_STEP_EPS_DEG) {
    panEstimateDeg = clampPanTargetDeg(panEstimateDeg + panDeltaDeg);
    panDeg = (int)round(panEstimateDeg);
    sendPanTarget(panEstimateDeg);
    sent = true;
  }

  if (fabs(tiltDeltaDeg) >= JOYSTICK_STEP_EPS_DEG) {
    tiltEstimateDeg = clampTiltTargetDeg(tiltEstimateDeg + tiltDeltaDeg);
    tiltDeg = (int)round(tiltEstimateDeg);
    sendTiltTarget(tiltEstimateDeg);
    sent = true;
  }

  armJoystickActive = true;
  lastArmMotionUpdateAt = now;
  joystickExpireAt = now + JOYSTICK_IDLE_RELEASE_MS;

  DEBUG_SERIAL.printf(
    "[ARM] joystick x=%.2f y=%.2f dt=%lu panDelta=%.2f tiltDelta=%.2f sent=%d\n",
    x,
    y,
    dtMs,
    panDeltaDeg,
    tiltDeltaDeg,
    sent ? 1 : 0
  );
}

void stopArmJoystickMotion() {
  bool wasActive = armJoystickActive;

  armJoystickActive = false;
  joystickExpireAt = 0;
  lastArmMotionUpdateAt = 0;
  panEstimateDeg = panDeg;
  tiltEstimateDeg = tiltDeg;

  if (wasActive) {
    DEBUG_SERIAL.println("[ARM] joystick stopped");
  }
}

void updateArmJoystickMotion() {
  if (!armJoystickActive) {
    return;
  }

  if (joystickExpireAt > 0 && (long)(millis() - joystickExpireAt) >= 0) {
    stopArmJoystickMotion();
    DEBUG_SERIAL.println("[ARM] joystick idle timeout, control released");
  }
}

bool handleArmAction(const String& action) {
  String normalizedAction = action;
  normalizedAction.trim();
  normalizedAction.toLowerCase();

  if (normalizedAction == "slider_position") {
    DEBUG_SERIAL.println("[ARM] slider_position ignored by lamp firmware");
    return false;
  }

  stopArmJoystickMotion();

  if (normalizedAction == "up") {
    tiltDeg = (int)clampTiltTargetDeg(tiltDeg + angleStep);
    sendTiltTarget(tiltDeg);
  } else if (normalizedAction == "down") {
    tiltDeg = (int)clampTiltTargetDeg(tiltDeg - angleStep);
    sendTiltTarget(tiltDeg);
  } else if (normalizedAction == "left") {
    panDeg = (int)clampPanTargetDeg(panDeg - angleStep);
    sendPanTarget(panDeg);
  } else if (normalizedAction == "right") {
    panDeg = (int)clampPanTargetDeg(panDeg + angleStep);
    sendPanTarget(panDeg);
  } else if (normalizedAction == "center") {
    panDeg = 0;
    tiltDeg = 0;
    sendPanTilt();
  } else if (normalizedAction == "home") {
    sendNano('A');
  } else if (normalizedAction == "enable" || normalizedAction == "motor_enable") {
    sendNano('e');
  } else if (normalizedAction == "report" || normalizedAction == "status") {
    sendNano('R');
  } else if (normalizedAction == "stop") {
    DEBUG_SERIAL.println("[ARM] stop: keep current pan/tilt");
    sendPanTilt();
  } else if (normalizedAction == "aim_person") {
    float defaultPan = LAMP_DEFAULT_PERSON_PAN_DEG;
    float defaultTilt = LAMP_DEFAULT_PERSON_TILT_DEG;
    getDefaultPersonAim(defaultPan, defaultTilt);
    panDeg = (int)round(defaultPan);
    tiltDeg = (int)round(defaultTilt);
    sendPanTilt();
  } else if (normalizedAction == "aim_cloth") {
    float defaultPan = GARMENT_AIM_DEFAULT_PAN_DEG;
    float defaultTilt = GARMENT_AIM_DEFAULT_TILT_DEG;
    getDefaultGarmentAim(defaultPan, defaultTilt);
    panDeg = (int)round(defaultPan);
    tiltDeg = (int)round(defaultTilt);
    sendPanTilt();
  } else {
    DEBUG_SERIAL.println("[ARM] unsupported lamp action: " + normalizedAction);
    return false;
  }

  DEBUG_SERIAL.printf(
    "[ARM] action=%s pan=%d tilt=%d slider=%d angleStep=%d sliderStep=%d\n",
    normalizedAction.c_str(),
    panDeg,
    tiltDeg,
    sliderMm,
    angleStep,
    sliderStep
  );
  return true;
}
