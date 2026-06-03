#include "device/arm_controller.h"

// Current speed preset.
String currentArmSpeed = "normal";

// Continuous joystick motion state.
bool armJoystickActive = false;
float joystickX = 0.0f;
float joystickY = 0.0f;
float panVelocityDegPerSec = 0.0f;
float tiltVelocityDegPerSec = 0.0f;
unsigned long joystickExpireAt = 0;
unsigned long lastArmMotionUpdateAt = 0;
unsigned long lastNanoPositionSendAt = 0;
bool nanoLineSeen = false;
String lastNanoLine = "";
unsigned long lastNanoRxAt = 0;
bool lastNanoHomingOk = false;
bool lastNanoHallStatusOk = false;

static const unsigned long JOYSTICK_TARGET_SEND_INTERVAL_MS = 120;
static const float JOYSTICK_LEAD_SECONDS = 0.6f;
static const float JOYSTICK_DEADZONE_DEG_PER_SEC = 0.05f;
static const float JOYSTICK_TARGET_EPS_DEG = 0.8f;
static const float JOYSTICK_SPEED_EPS_DEG_PER_SEC = 0.25f;
static const float PAN_MIN_LEAD_DEG = 3.0f;
static const float TILT_MIN_LEAD_DEG = 2.0f;

static float panEstimateDeg = 0.0f;
static float tiltEstimateDeg = 0.0f;
static float lastSentPanTargetDeg = 10000.0f;
static float lastSentTiltTargetDeg = 10000.0f;
static float lastSentPanJoystickSpeed = -1.0f;
static float lastSentTiltJoystickSpeed = -1.0f;

String formatDeg(float value) {
  return String(value, 2);
}

float joystickLeadForVelocity(float velocityDegPerSec, float minLeadDeg) {
  if (fabs(velocityDegPerSec) <= JOYSTICK_DEADZONE_DEG_PER_SEC) {
    return 0.0f;
  }

  float lead = velocityDegPerSec * JOYSTICK_LEAD_SECONDS;
  if (fabs(lead) < minLeadDeg) {
    lead = velocityDegPerSec > 0.0f ? minLeadDeg : -minLeadDeg;
  }
  return lead;
}

void syncJoystickEstimate(unsigned long now) {
  if (lastArmMotionUpdateAt == 0) {
    lastArmMotionUpdateAt = now;
    return;
  }

  float dt = (now - lastArmMotionUpdateAt) / 1000.0f;
  lastArmMotionUpdateAt = now;

  if (dt <= 0.0f) {
    return;
  }
  if (dt > 0.2f) {
    dt = 0.2f;
  }

  panEstimateDeg += panVelocityDegPerSec * dt;
  tiltEstimateDeg += tiltVelocityDegPerSec * dt;

  panEstimateDeg = constrain(panEstimateDeg, (float)PAN_MIN, (float)PAN_MAX);
  tiltEstimateDeg = constrain(tiltEstimateDeg, (float)TILT_MIN, (float)TILT_MAX);

  panDeg = (int)round(panEstimateDeg);
  tiltDeg = (int)round(tiltEstimateDeg);
}

void sendJoystickSpeedIfNeeded(char cmd, float speedDegPerSec, float& lastSentSpeed) {
  float speed = fabs(speedDegPerSec);
  if (speed <= JOYSTICK_DEADZONE_DEG_PER_SEC) {
    return;
  }

  if (lastSentSpeed < 0.0f || fabs(speed - lastSentSpeed) >= JOYSTICK_SPEED_EPS_DEG_PER_SEC) {
    sendNano(cmd, String(speed, 2));
    lastSentSpeed = speed;
  }
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
        line = "";
      }
      continue;
    }

    if (line.length() < 120) {
      line += ch;
    } else {
      line = "";
      DEBUG_SERIAL.println("[NANO] RX line too long, dropped");
    }
  }
}

void sendPanTilt() {
  panDeg = constrain(panDeg, PAN_MIN, PAN_MAX);
  tiltDeg = constrain(tiltDeg, TILT_MIN, TILT_MAX);
  sendNano('p', formatDeg(panDeg));
  sendNano('t', formatDeg(tiltDeg));
}

void sendSlider() {
  sliderMm = constrain(sliderMm, SLIDER_MIN, SLIDER_MAX);
  sendNano('x', String((float)sliderMm, 2));
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

  sendNano('s', String((float)panSpeedDeg, 2));
  sendNano('S', String((float)tiltSpeedDeg, 2));
  sendNano('X', String((float)sliderSpeedMm, 2));
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

void setArmJoystickMotion(float x, float y, int durationMs) {
  x = constrain(x, -1.0f, 1.0f);
  y = constrain(y, -1.0f, 1.0f);
  durationMs = constrain(durationMs, 100, 1000);
  unsigned long now = millis();

  float maxPanSpeed = 8.0f;
  float maxTiltSpeed = 5.0f;
  getArmJoystickMaxSpeed(maxPanSpeed, maxTiltSpeed);

  if (!armJoystickActive) {
    panEstimateDeg = panDeg;
    tiltEstimateDeg = tiltDeg;
    lastSentPanTargetDeg = panEstimateDeg;
    lastSentTiltTargetDeg = tiltEstimateDeg;
    lastSentPanJoystickSpeed = -1.0f;
    lastSentTiltJoystickSpeed = -1.0f;
    lastArmMotionUpdateAt = now;
    lastNanoPositionSendAt = 0;
  } else {
    syncJoystickEstimate(now);
  }

  joystickX = x;
  joystickY = y;

  panVelocityDegPerSec = joystickX * maxPanSpeed;
  tiltVelocityDegPerSec = joystickY * maxTiltSpeed;
  sendJoystickSpeedIfNeeded('s', panVelocityDegPerSec, lastSentPanJoystickSpeed);
  sendJoystickSpeedIfNeeded('S', tiltVelocityDegPerSec, lastSentTiltJoystickSpeed);

  armJoystickActive = true;
  joystickExpireAt = now + durationMs;

  DEBUG_SERIAL.printf(
    "[ARM] joystick x=%.2f y=%.2f panVel=%.2f tiltVel=%.2f duration=%d\n",
    x,
    y,
    panVelocityDegPerSec,
    tiltVelocityDegPerSec,
    durationMs
  );
}

void stopArmJoystickMotion() {
  bool wasActive = armJoystickActive;

  if (wasActive) {
    syncJoystickEstimate(millis());
    sendNano('p', formatDeg(panEstimateDeg));
    sendNano('t', formatDeg(tiltEstimateDeg));
    sendNano('s', String((float)panSpeedDeg, 2));
    sendNano('S', String((float)tiltSpeedDeg, 2));
  }

  armJoystickActive = false;
  joystickX = 0.0f;
  joystickY = 0.0f;
  panVelocityDegPerSec = 0.0f;
  tiltVelocityDegPerSec = 0.0f;
  lastArmMotionUpdateAt = 0;
  lastSentPanTargetDeg = panEstimateDeg;
  lastSentTiltTargetDeg = tiltEstimateDeg;
  lastSentPanJoystickSpeed = -1.0f;
  lastSentTiltJoystickSpeed = -1.0f;

  DEBUG_SERIAL.println("[ARM] joystick stopped");
}

void updateArmJoystickMotion() {
  if (!armJoystickActive) {
    return;
  }

  unsigned long now = millis();
  syncJoystickEstimate(now);

  // Joystick packets act as a heartbeat; stop if updates stop arriving.
  if ((long)(now - joystickExpireAt) >= 0) {
    stopArmJoystickMotion();
    DEBUG_SERIAL.println("[ARM] joystick expired, auto stop");
    return;
  }

  // Send a lead target, not the estimated current position. Nano keeps chasing
  // the target ahead of the current estimate, which reduces small stop/start
  // steps without requiring a Nano protocol change.
  if (now - lastNanoPositionSendAt >= JOYSTICK_TARGET_SEND_INTERVAL_MS) {
    bool sent = false;

    float panTargetDeg = panEstimateDeg + joystickLeadForVelocity(panVelocityDegPerSec, PAN_MIN_LEAD_DEG);
    panTargetDeg = constrain(panTargetDeg, (float)PAN_MIN, (float)PAN_MAX);
    if (fabs(panTargetDeg - lastSentPanTargetDeg) >= JOYSTICK_TARGET_EPS_DEG) {
      sendNano('p', formatDeg(panTargetDeg));
      lastSentPanTargetDeg = panTargetDeg;
      sent = true;
    }

    float tiltTargetDeg = tiltEstimateDeg + joystickLeadForVelocity(tiltVelocityDegPerSec, TILT_MIN_LEAD_DEG);
    tiltTargetDeg = constrain(tiltTargetDeg, (float)TILT_MIN, (float)TILT_MAX);
    if (fabs(tiltTargetDeg - lastSentTiltTargetDeg) >= JOYSTICK_TARGET_EPS_DEG) {
      sendNano('t', formatDeg(tiltTargetDeg));
      lastSentTiltTargetDeg = tiltTargetDeg;
      sent = true;
    }

    if (sent) {
      lastNanoPositionSendAt = now;
    }
  }
}

void handleArmAction(const String& action) {
  String normalizedAction = action;
  normalizedAction.trim();
  normalizedAction.toLowerCase();

  if (normalizedAction == "slider_position") {
    DEBUG_SERIAL.println("[ARM] slider_position ignored by lamp firmware");
    return;
  }

  if (normalizedAction == "up") {
    tiltDeg += angleStep;
    tiltDeg = constrain(tiltDeg, TILT_MIN, TILT_MAX);
    sendNano('t', formatDeg(tiltDeg));
  } else if (normalizedAction == "down") {
    tiltDeg -= angleStep;
    tiltDeg = constrain(tiltDeg, TILT_MIN, TILT_MAX);
    sendNano('t', formatDeg(tiltDeg));
  } else if (normalizedAction == "left") {
    panDeg -= angleStep;
    panDeg = constrain(panDeg, PAN_MIN, PAN_MAX);
    sendNano('p', formatDeg(panDeg));
  } else if (normalizedAction == "right") {
    panDeg += angleStep;
    panDeg = constrain(panDeg, PAN_MIN, PAN_MAX);
    sendNano('p', formatDeg(panDeg));
  } else if (normalizedAction == "center") {
    panDeg = 0;
    tiltDeg = 0;
    sendPanTilt();
  } else if (normalizedAction == "home") {
    sendNano('A');
  } else if (normalizedAction == "stop") {
    DEBUG_SERIAL.println("[ARM] stop: keep current pan/tilt");
    sendPanTilt();
  } else if (normalizedAction == "aim_person") {
    panDeg = 0;
    tiltDeg = -10;
    sendPanTilt();
  } else if (normalizedAction == "aim_cloth") {
    panDeg = 0;
    tiltDeg = 20;
    sendPanTilt();
  } else {
    DEBUG_SERIAL.println("[ARM] unsupported lamp action: " + normalizedAction);
    return;
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
}
