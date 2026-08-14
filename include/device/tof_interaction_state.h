#pragma once

#include <stdint.h>

enum class ClothTakeDetectionState : uint8_t {
  Armed,
  Candidate,
  TakenLatched,
  DisarmedUntilRelease,
};

struct TofInteractionConfig {
  uint16_t personNearThresholdMm;
  uint16_t clothTakenThresholdMm;
  uint32_t clothConfirmMs;
  uint16_t clothReleaseThresholdMm;
  uint32_t clothRearmMs;
};

struct TofInteractionState {
  bool proximityInitialized = false;
  bool nearby = false;
  ClothTakeDetectionState clothState = ClothTakeDetectionState::Armed;
  uint32_t candidateStartedAt = 0;
  uint32_t releaseStartedAt = 0;
};

struct TofInteractionUpdate {
  bool proximityChanged = false;
  bool nearby = false;
  bool clothTaken = false;
};

TofInteractionUpdate updateTofInteraction(
  TofInteractionState& state,
  uint16_t distanceMm,
  uint32_t nowMs,
  const TofInteractionConfig& config
);

void clearClothTakenDetection(TofInteractionState& state);

