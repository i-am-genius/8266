#include "device/arm_controller.h"
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

static const float JOYSTICK_INPUT_DEADZONE = 0.05f;
static const float JOYSTICK_STEP_EPS_DEG = 0.02f;
static const unsigned long JOYSTICK_PACKET_DT_FALLBACK_MS = 120;
static const unsigned long JOYSTICK_PACKET_DT_MIN_MS = 30;
static const unsigned long JOYSTICK_PACKET_DT_MAX_MS = 220;
static const unsigned long JOYSTICK_IDLE_RELEASE_MS = 400;
static const float TILT_COMMAND_RATIO =30.0f / 48.0f;
static const unsigned long NANO_STARTUP_SYNC_DELAY_MS = 1800;
static const unsigned long NANO_STARTUP_SYNC_RETRY_MS = 1200;
static const uint8_t NANO_STARTUP_SYNC_MAX_ATTEMPTS = 3;

static float panEstimateDeg = 0.0f;
static float tiltEstimateDeg = 0.0f;
static bool nanoStartupSyncScheduled = false;
static unsigned long nanoStartupSyncScheduledAt = 0;
static unsigned long nanoStartupSyncLastAttemptAt = 0;
static uint8_t nanoStartupSyncAttempts = 0;
static bool nanoStartupStatusRequested = false;
static unsigned long nanoStartupStatusRequestedAt = 0;
static bool nanoStartupEnableRecovered = false;
static bool nanoStartupEnableRecoveryPending = false;

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

void scheduleNanoStartupSync() {
  nanoStartupSyncScheduled = true;
  nanoStartupSyncScheduledAt = millis();
  nanoStartupSyncLastAttemptAt = 0;
  nanoStartupSyncAttempts = 0;
  nanoStartupStatusRequested = false;
  nanoStartupStatusRequestedAt = 0;
  nanoStartupEnableRecovered = false;
  nanoStartupEnableRecoveryPending = false;
  nanoEnableStateKnown = false;
  nanoEnabled = true;
}

void handleNanoStartupSync() {
  if (!nanoStartupSyncScheduled) {
    return;
  }

  unsigned long now = millis();
  if (now - nanoStartupSyncScheduledAt < NANO_STARTUP_SYNC_DELAY_MS) {
    return;
  }

  if (
    nanoStartupSyncAttempts > 0 &&
    now - nanoStartupSyncLastAttemptAt < NANO_STARTUP_SYNC_RETRY_MS
  ) {
    return;
  }

  nanoStartupSyncAttempts++;
  nanoStartupSyncLastAttemptAt = now;

  DEBUG_SERIAL.printf(
    "[NANO] startup sync %u/%u\n",
    nanoStartupSyncAttempts,
    NANO_STARTUP_SYNC_MAX_ATTEMPTS
  );

  sendNano('m', "16");
  applyArmSpeed(currentArmSpeed);

  if (nanoStartupSyncAttempts >= NANO_STARTUP_SYNC_MAX_ATTEMPTS) {
    nanoStartupSyncScheduled = false;
    nanoStartupStatusRequested = true;
    nanoStartupStatusRequestedAt = now;
    sendNano('R');
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

void sendNano(char cmd, const String& value) {
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

void pollNano() {
  static String line;

  while (nanoSerial.available() > 0) {
    char ch = (char)nanoSerial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      if (line.length() > 0) {
        lastNanoLine = line;
        lastNanoRxAt = millis();
        nanoLineSeen = true;
        DEBUG_SERIAL.println("[NANO] RX " + line);

        bool parsedEnabled = true;
        if (parseNanoEnableStateLine(line, parsedEnabled)) {
          nanoEnableStateKnown = true;
          nanoEnabled = parsedEnabled;
          DEBUG_SERIAL.printf("[NANO] enable=%d\n", nanoEnabled ? 1 : 0);

          if (nanoStartupStatusRequested && !nanoEnabled && !nanoStartupEnableRecovered) {
            nanoStartupEnableRecovered = true;
            nanoStartupEnableRecoveryPending = true;
          }
        } else if (line == "Enabled") {
          nanoEnableStateKnown = true;
          nanoEnabled = true;
        } else if (line == "Disabled") {
          nanoEnableStateKnown = true;
          nanoEnabled = false;
        }

        if (nanoStartupStatusRequested && nanoEnableStateKnown && nanoEnabled) {
          nanoStartupStatusRequested = false;
        }
        line = "";
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

  if (
    nanoStartupStatusRequested &&
    millis() - nanoStartupStatusRequestedAt > 3000 &&
    !nanoEnableStateKnown
  ) {
    DEBUG_SERIAL.println("[NANO] startup status check timed out");
    diagnosticLogNano("startup status timeout", true);
    nanoStartupStatusRequested = false;
  }

  if (nanoStartupEnableRecoveryPending) {
    nanoStartupEnableRecoveryPending = false;
    DEBUG_SERIAL.println("[NANO] startup recovery: re-enabling drivers");
    diagnosticLogNano("re-enabling disabled drivers");
    sendNano('e');
    sendNano('m', "16");
    applyArmSpeed(currentArmSpeed);
    sendNano('R');
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
    angleStep = 2;
    sliderStep = 5;
    panSpeedDeg = 4;
    tiltSpeedDeg = 3;
    sliderSpeedMm = 5;
  } else if (normalized == "fast") {
    angleStep = 5;
    sliderStep = 20;
    panSpeedDeg = 15;
    tiltSpeedDeg = 10;
    sliderSpeedMm = 30;
  } else {
    // normal
    angleStep = 5;
    sliderStep = 10;
    panSpeedDeg = 8;
    tiltSpeedDeg = 5;
    sliderSpeedMm = 10;
  }

  currentArmSpeed = normalized;

  sendPanSpeed((float)panSpeedDeg);
  sendTiltSpeed((float)tiltSpeedDeg);
  sendNanoFloat('X', (float)sliderSpeedMm);
}

void getArmJoystickMaxSpeed(float& maxPanSpeed, float& maxTiltSpeed) {
  if (currentArmSpeed == "slow") {
    maxPanSpeed = 4.0f;
    maxTiltSpeed = 3.0f;
  } else if (currentArmSpeed == "fast") {
    maxPanSpeed = 15.0f;
    maxTiltSpeed = 10.0f;
  } else {
    // normal
    maxPanSpeed = 8.0f;
    maxTiltSpeed = 5.0f;
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
    panDeg = 0;
    tiltDeg = -30;
    sendPanTilt();
  } else if (normalizedAction == "aim_cloth") {
    panDeg = 0;
    tiltDeg = 20;
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
