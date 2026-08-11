#include "device/garment_aim.h"

#include <cmath>

static float clampNormalized(float value) {
  if (value < 0.0f) return 0.0f;
  if (value > 1.0f) return 1.0f;
  return value;
}

bool isValidCalibratedGarmentAimPose(float panDeg, float tiltDeg, float sliderMm) {
  return std::isfinite(panDeg) && std::isfinite(tiltDeg) && std::isfinite(sliderMm);
}

GarmentAimTarget calculateGarmentAimTarget(
  float normalizedCenterX,
  float normalizedCenterY,
  const GarmentAimConfig& config
) {
  const float centerX = clampNormalized(normalizedCenterX);
  const float centerY = clampNormalized(normalizedCenterY);

  return GarmentAimTarget{
    config.defaultPanDeg + (centerX - 0.5f) * config.horizontalFovDeg,
    config.defaultTiltDeg - (centerY - 0.5f) * config.verticalFovDeg,
  };
}
