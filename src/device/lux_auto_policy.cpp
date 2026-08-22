#include "device/lux_auto_policy.h"

#include <math.h>

static int clampInt(int value, int minimum, int maximum) {
  if (value < minimum) return minimum;
  if (value > maximum) return maximum;
  return value;
}

static float clampFloat(float value, float minimum, float maximum) {
  if (value < minimum) return minimum;
  if (value > maximum) return maximum;
  return value;
}

LuxAutoState updateLuxAutoControl(
  const LuxAutoState& current,
  float sampleLux,
  int targetLux,
  int baseBrightness,
  const LuxAutoConfig& config
) {
  if (!isfinite(sampleLux) || sampleLux < 0.0f) {
    return current;
  }

  const int globalMin = config.minBrightness < config.maxBrightness
    ? config.minBrightness
    : config.maxBrightness;
  const int globalMax = config.minBrightness < config.maxBrightness
    ? config.maxBrightness
    : config.minBrightness;
  const int offset = config.maxOffsetFromBase > 0
    ? config.maxOffsetFromBase
    : 0;
  const int lowerBound = clampInt(baseBrightness - offset, globalMin, globalMax);
  const int upperBound = clampInt(baseBrightness + offset, globalMin, globalMax);
  const float alpha = clampFloat(config.filterAlpha, 0.01f, 1.0f);

  LuxAutoState next = current;
  next.filteredLux = current.initialized
    ? current.filteredLux + alpha * (sampleLux - current.filteredLux)
    : sampleLux;
  next.brightness = current.initialized
    ? clampInt(current.brightness, lowerBound, upperBound)
    : clampInt(baseBrightness, lowerBound, upperBound);
  next.initialized = true;

  const float safeTarget = targetLux > 0 ? static_cast<float>(targetLux) : 0.0f;
  const float error = safeTarget - next.filteredLux;
  const float deadband = config.deadbandLux > 0.0f
    ? config.deadbandLux
    : 0.0f;
  const float absoluteError = fabsf(error);
  if (absoluteError <= deadband) {
    return next;
  }

  const float luxPerStep = config.luxPerBrightnessStep > 1.0f
    ? config.luxPerBrightnessStep
    : 1.0f;
  int step = static_cast<int>(ceilf((absoluteError - deadband) / luxPerStep));
  if (step < 1) step = 1;
  const int maxStep = config.maxStepPerUpdate > 0
    ? config.maxStepPerUpdate
    : 1;
  if (step > maxStep) step = maxStep;

  next.brightness += error > 0.0f ? step : -step;
  next.brightness = clampInt(next.brightness, lowerBound, upperBound);
  return next;
}
