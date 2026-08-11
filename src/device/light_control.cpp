#include "device/light_control.h"
#include "diagnostics/diagnostic_logger.h"

enum LocateBreathState {
  LOCATE_IDLE,
  LOCATE_RUNNING,
  LOCATE_RESTORE
};

static const int LOCATE_TEMP = 4500;
static const unsigned long LOCATE_UPDATE_INTERVAL_MS = 30;

static LocateBreathState locateState = LOCATE_IDLE;
static int locateTimes = 0;
static int locateCycleMs = 0;
static int locateRestoreBrightness = 0;
static int locateRestoreTemp = 0;
static unsigned long locateStartedAt = 0;
static unsigned long lastLocateUpdateAt = 0;

void applyLightSettings(int br, int tp) {
  tp = constrain(tp, 2700, 6500);

  int tempVal = map(tp, 2700, 6500, 0, 1024);
  int briVal  = map(br, 0, 100, 0, 1024);

  int pwmCold = (long)tempVal * briVal / 1024;
  int pwmWarm = (long)(1024 - tempVal) * briVal / 1024;

  analogWrite(LED_COLD_PIN, 1024 - pwmCold);
  analogWrite(LED_WARM_PIN, 1024 - pwmWarm);
}

void stopEffectWaveForManualControl() {
  if (!effectWaveEnabled) return;
    effectWaveEnabled = false;
    DEBUG_SERIAL.println("[EFFECT] wave stopped by manual control");
    diagnosticLogEffect("stopped by manual control");
}

void startLocateBreath(int times, int cycleMs) {
  times = constrain(times, 1, 8);
  cycleMs = constrain(cycleMs, 800, 3000);

  if (locateState == LOCATE_IDLE) {
    locateRestoreBrightness = autoMode ? recommendedBrightness : brightness;
    locateRestoreTemp = autoMode ? recommendedTemp : temp;
  }

  locateTimes = times;
  locateCycleMs = cycleMs;
  locateStartedAt = millis();
  lastLocateUpdateAt = 0;
  locateState = LOCATE_RUNNING;

  DEBUG_SERIAL.printf(
    "[LOCATE] 呼吸灯开始 times=%d cycleMs=%d restoreB=%d restoreT=%d\n",
    locateTimes, locateCycleMs, locateRestoreBrightness, locateRestoreTemp
  );
}

void stopLocateBreath(bool restoreLight) {
  if (locateState == LOCATE_IDLE) {
    return;
  }

  if (restoreLight) {
    applyLightSettings(locateRestoreBrightness, locateRestoreTemp);
    lastLightUpdate = millis();
  }
  locateState = LOCATE_IDLE;
}

bool isLocateBreathActive() {
  return locateState != LOCATE_IDLE;
}

void handleLocateBreathTask() {
  if (locateState == LOCATE_IDLE) {
    return;
  }

  if (locateState == LOCATE_RESTORE) {
    applyLightSettings(locateRestoreBrightness, locateRestoreTemp);
    lastLightUpdate = millis();
    locateState = LOCATE_IDLE;
    DEBUG_SERIAL.println("[LOCATE] 呼吸定位结束，已恢复原灯光");
    return;
  }

  unsigned long now = millis();
  unsigned long totalMs = (unsigned long)locateTimes * (unsigned long)locateCycleMs;
  unsigned long elapsed = now - locateStartedAt;

  if (elapsed >= totalMs) {
    locateState = LOCATE_RESTORE;
    return;
  }

  if (lastLocateUpdateAt != 0 && now - lastLocateUpdateAt < LOCATE_UPDATE_INTERVAL_MS) {
    return;
  }
  lastLocateUpdateAt = now;

  unsigned long cycleElapsed = elapsed % (unsigned long)locateCycleMs;
  float phase = (float)cycleElapsed / (float)locateCycleMs * PI;
  int br = LOCATE_MIN_BRIGHTNESS + int(sin(phase) * (LOCATE_MAX_BRIGHTNESS - LOCATE_MIN_BRIGHTNESS));
  applyLightSettings(br, LOCATE_TEMP);
  lastLightUpdate = now;
}

void updateEffectLoop() {
  if (!effectWaveEnabled) return;

  unsigned long now = millis();
  if (now - lastEffectUpdateMs < WAVE_UPDATE_INTERVAL_MS) return;
  lastEffectUpdateMs = now;

  float elapsedSec = (now - effectStartMs) / 1000.0f;
  float phase = elapsedSec * WAVE_FREQ_FACTOR * effectSpeed + effectPhaseOffset;
  int targetTemp = (int)round(effectBaseTemp + sin(phase) * effectRange);
  targetTemp = constrain(targetTemp, 2700, 6500);
  int targetBrightness = constrain(effectBrightness, 0, 100);

  brightness = targetBrightness;
  temp = targetTemp;
  recommendedBrightness = targetBrightness;
  recommendedTemp = targetTemp;
  applyLightSettings(targetBrightness, targetTemp);
}

void safeCopyFabric(const char* src) {
  if (!src || strlen(src) == 0) return;
  snprintf(fabric, sizeof(fabric), "%s", src);
}
