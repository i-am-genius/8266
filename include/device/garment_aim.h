#pragma once

struct GarmentAimConfig {
  float defaultPanDeg;
  float defaultTiltDeg;
  float horizontalFovDeg;
  float verticalFovDeg;
};

struct GarmentAimTarget {
  float panDeg;
  float tiltDeg;
};

GarmentAimTarget calculateGarmentAimTarget(
  float normalizedCenterX,
  float normalizedCenterY,
  const GarmentAimConfig& config
);

bool isValidCalibratedGarmentAimPose(float panDeg, float tiltDeg, float sliderMm);
