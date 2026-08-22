#pragma once

struct LuxAutoConfig {
  float filterAlpha;
  float deadbandLux;
  float luxPerBrightnessStep;
  int maxStepPerUpdate;
  int maxOffsetFromBase;
  int minBrightness;
  int maxBrightness;
};

struct LuxAutoState {
  float filteredLux;
  int brightness;
  bool initialized;
};

LuxAutoState updateLuxAutoControl(
  const LuxAutoState& current,
  float sampleLux,
  int targetLux,
  int baseBrightness,
  const LuxAutoConfig& config
);
