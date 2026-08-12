#pragma once

constexpr float LAMP_DEFAULT_PERSON_PAN_DEG = 0.0f;
constexpr float LAMP_DEFAULT_PERSON_TILT_DEG = -30.0f;

enum class LampAimSource {
  DefaultGarment,
  Garment,
  DefaultPerson,
  Person,
};

struct LampAimPose {
  float panDeg;
  float tiltDeg;
};

// Two configurable/default targets and two runtime targets. Runtime tracking
// updates never overwrite either default pose. Slider motion is independent.
struct LampAimState {
  LampAimPose garment;
  LampAimPose defaultGarment;
  LampAimPose person;
  LampAimPose defaultPerson;
  bool garmentTrackingEnabled;
  bool garmentTargetValid;
  bool personTrackingActive;
  bool personTargetValid;
};

struct LampAimSelection {
  LampAimSource source;
  LampAimPose pose;
};

LampAimSelection selectLampAim(const LampAimState& state);
const char* lampAimSourceName(LampAimSource source);
bool sameLampAimSelection(
  const LampAimSelection& left,
  const LampAimSelection& right,
  float angleToleranceDeg = 0.05f
);
