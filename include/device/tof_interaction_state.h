#pragma once

#include <stdint.h>

enum class ClothTakeDetectionState : uint8_t {
  Armed,
  Candidate,
  TakenLatched,
  DisarmedUntilRelease,
};

enum class TofSampleKind : uint8_t {
  ValidDistance,
  NoTarget,
  Invalid,
};

struct TofSample {
  TofSampleKind kind = TofSampleKind::Invalid;
  uint16_t distanceMm = 0;

  TofSample() = default;
  TofSample(TofSampleKind sampleKind, uint16_t sampleDistanceMm)
    : kind(sampleKind), distanceMm(sampleDistanceMm) {}
};

struct TofInteractionConfig {
  uint16_t personNearThresholdMm;
  uint32_t proximityExitConfirmMs;
  uint16_t clothTakenThresholdMm;
  uint32_t clothConfirmMs;
  uint16_t clothReleaseThresholdMm;
  uint32_t clothRearmMs;
};

struct TofInteractionState {
  bool proximityInitialized = false;
  bool nearby = false;
  bool proximityExitCandidateActive = false;
  uint32_t proximityExitStartedAt = 0;
  ClothTakeDetectionState clothState = ClothTakeDetectionState::Armed;
  uint32_t candidateStartedAt = 0;
  uint32_t releaseStartedAt = 0;
};

struct TofInteractionUpdate {
  bool proximityChanged = false;
  bool nearby = false;
  bool clothTaken = false;
};

TofSample classifyTofSample(
  uint8_t rangeStatus,
  uint16_t distanceMm,
  uint16_t maximumDistanceMm
);

TofInteractionUpdate updateTofInteraction(
  TofInteractionState& state,
  uint16_t distanceMm,
  uint32_t nowMs,
  const TofInteractionConfig& config
);

TofInteractionUpdate updateTofInteraction(
  TofInteractionState& state,
  const TofSample& sample,
  uint32_t nowMs,
  const TofInteractionConfig& config
);

void clearClothTakenDetection(TofInteractionState& state);
