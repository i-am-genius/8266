#include "device/tof_interaction_state.h"

static bool elapsedAtLeast(uint32_t nowMs, uint32_t startedAt, uint32_t durationMs) {
  return static_cast<uint32_t>(nowMs - startedAt) >= durationMs;
}

TofInteractionUpdate updateTofInteraction(
  TofInteractionState& state,
  uint16_t distanceMm,
  uint32_t nowMs,
  const TofInteractionConfig& config
) {
  TofInteractionUpdate update{};
  const bool nearby = distanceMm < config.personNearThresholdMm;
  if (!state.proximityInitialized || nearby != state.nearby) {
    state.proximityInitialized = true;
    state.nearby = nearby;
    update.proximityChanged = true;
  }
  update.nearby = state.nearby;

  switch (state.clothState) {
    case ClothTakeDetectionState::Armed:
      if (distanceMm < config.clothTakenThresholdMm) {
        state.clothState = ClothTakeDetectionState::Candidate;
        state.candidateStartedAt = nowMs;
      }
      break;

    case ClothTakeDetectionState::Candidate:
      if (distanceMm >= config.clothTakenThresholdMm) {
        state.clothState = ClothTakeDetectionState::Armed;
        state.candidateStartedAt = 0;
      } else if (elapsedAtLeast(nowMs, state.candidateStartedAt, config.clothConfirmMs)) {
        state.clothState = ClothTakeDetectionState::TakenLatched;
        update.clothTaken = true;
      }
      break;

    case ClothTakeDetectionState::TakenLatched:
      break;

    case ClothTakeDetectionState::DisarmedUntilRelease:
      if (distanceMm < config.clothReleaseThresholdMm) {
        state.releaseStartedAt = 0;
      } else if (state.releaseStartedAt == 0) {
        state.releaseStartedAt = nowMs;
      } else if (elapsedAtLeast(nowMs, state.releaseStartedAt, config.clothRearmMs)) {
        state.clothState = ClothTakeDetectionState::Armed;
        state.releaseStartedAt = 0;
      }
      break;
  }

  return update;
}

void clearClothTakenDetection(TofInteractionState& state) {
  state.clothState = ClothTakeDetectionState::DisarmedUntilRelease;
  state.candidateStartedAt = 0;
  state.releaseStartedAt = 0;
}

