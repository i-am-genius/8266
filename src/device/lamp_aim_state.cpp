#include "device/lamp_aim_state.h"

#include <cmath>

LampAimSelection selectLampAim(const LampAimState& state) {
  if (state.personTrackingActive) {
    if (state.personTargetValid) {
      return LampAimSelection{LampAimSource::Person, state.person};
    }
    return LampAimSelection{LampAimSource::DefaultPerson, state.defaultPerson};
  }

  if (state.garmentTrackingEnabled && state.garmentTargetValid) {
    return LampAimSelection{LampAimSource::Garment, state.garment};
  }
  return LampAimSelection{LampAimSource::DefaultGarment, state.defaultGarment};
}

const char* lampAimSourceName(LampAimSource source) {
  switch (source) {
    case LampAimSource::Garment:
      return "garment";
    case LampAimSource::DefaultPerson:
      return "person_default";
    case LampAimSource::Person:
      return "person";
    case LampAimSource::DefaultGarment:
    default:
      return "garment_default";
  }
}

bool sameLampAimSelection(
  const LampAimSelection& left,
  const LampAimSelection& right,
  float angleToleranceDeg
) {
  return left.source == right.source
    && std::fabs(left.pose.panDeg - right.pose.panDeg) <= angleToleranceDeg
    && std::fabs(left.pose.tiltDeg - right.pose.tiltDeg) <= angleToleranceDeg;
}
