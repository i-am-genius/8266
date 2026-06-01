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

// Preserve fractional motion so small joystick deltas are not lost.
static float panRemainder = 0.0f;
static float tiltRemainder = 0.0f;
static int lastJoystickSentPanDeg = 10000;
static int lastJoystickSentTiltDeg = 10000;

String formatDeg(int value) {
  return String((float)value, 2);
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

  float maxPanSpeed = 8.0f;
  float maxTiltSpeed = 5.0f;
  getArmJoystickMaxSpeed(maxPanSpeed, maxTiltSpeed);

  joystickX = x;
  joystickY = y;

  panVelocityDegPerSec = joystickX * maxPanSpeed;
  tiltVelocityDegPerSec = joystickY * maxTiltSpeed;
  lastJoystickSentPanDeg = panDeg;
  lastJoystickSentTiltDeg = tiltDeg;

  armJoystickActive = true;
  joystickExpireAt = millis() + durationMs;
  lastArmMotionUpdateAt = millis();

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
  armJoystickActive = false;
  joystickX = 0.0f;
  joystickY = 0.0f;
  panVelocityDegPerSec = 0.0f;
  tiltVelocityDegPerSec = 0.0f;
  panRemainder = 0.0f;
  tiltRemainder = 0.0f;
  lastJoystickSentPanDeg = panDeg;
  lastJoystickSentTiltDeg = tiltDeg;

  DEBUG_SERIAL.println("[ARM] joystick stopped");
}

void updateArmJoystickMotion() {
  if (!armJoystickActive) {
    return;
  }

  unsigned long now = millis();

  // Joystick packets act as a heartbeat; stop if updates stop arriving.
  if ((long)(now - joystickExpireAt) >= 0) {
    stopArmJoystickMotion();
    DEBUG_SERIAL.println("[ARM] joystick expired, auto stop");
    return;
  }

  if (lastArmMotionUpdateAt == 0) {
    lastArmMotionUpdateAt = now;
    return;
  }

  float dt = (now - lastArmMotionUpdateAt) / 1000.0f;
  lastArmMotionUpdateAt = now;

  // Ignore abnormal frame gaps to avoid a sudden position jump.
  if (dt <= 0.0f || dt > 0.2f) {
    return;
  }

  // Preserve fractional motion so small deltas eventually become whole steps.
  float panDelta = panVelocityDegPerSec * dt + panRemainder;
  float tiltDelta = tiltVelocityDegPerSec * dt + tiltRemainder;

  int panStep = (int)panDelta;
  int tiltStep = (int)tiltDelta;

  panRemainder = panDelta - panStep;
  tiltRemainder = tiltDelta - tiltStep;

  panDeg += panStep;
  tiltDeg += tiltStep;

  panDeg = constrain(panDeg, PAN_MIN, PAN_MAX);
  tiltDeg = constrain(tiltDeg, TILT_MIN, TILT_MAX);

  // Send the current position to Nano at a fixed cadence.
  if (now - lastNanoPositionSendAt >= 80) {
    bool sent = false;

    if (panDeg != lastJoystickSentPanDeg) {
      sendNano('p', formatDeg(panDeg));
      lastJoystickSentPanDeg = panDeg;
      sent = true;
    }

    if (tiltDeg != lastJoystickSentTiltDeg) {
      sendNano('t', formatDeg(tiltDeg));
      lastJoystickSentTiltDeg = tiltDeg;
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
