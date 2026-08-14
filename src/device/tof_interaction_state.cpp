#include "device/tof_interaction_state.h"

static bool elapsedAtLeast(uint32_t nowMs, uint32_t startedAt, uint32_t durationMs) {
  return static_cast<uint32_t>(nowMs - startedAt) >= durationMs;
}

TofSample classifyTofSample(
  uint8_t rangeStatus,
  uint16_t distanceMm,
  uint16_t maximumDistanceMm
) {
  if (rangeStatus == 0 && distanceMm <= maximumDistanceMm) {
    return TofSample{TofSampleKind::ValidDistance, distanceMm};
  }
  if (rangeStatus == 4) {
    return TofSample{TofSampleKind::NoTarget, distanceMm};
  }
  return TofSample{TofSampleKind::Invalid, distanceMm};
}

TofInteractionUpdate updateTofInteraction(
  TofInteractionState& state,
  uint16_t distanceMm,
  uint32_t nowMs,
  const TofInteractionConfig& config
) {
  return updateTofInteraction(
    state,
    TofSample{TofSampleKind::ValidDistance, distanceMm},
    nowMs,
    config
  );
}

TofInteractionUpdate updateTofInteraction(
  TofInteractionState& state,
  const TofSample& sample,
  uint32_t nowMs,
  const TofInteractionConfig& config
) {
  TofInteractionUpdate update{};
  if (sample.kind == TofSampleKind::Invalid) {
    state.proximityExitCandidateActive = false;
    state.proximityExitStartedAt = 0;
    if (state.clothState == ClothTakeDetectionState::Candidate) {
      state.clothState = ClothTakeDetectionState::Armed;
      state.candidateStartedAt = 0;
    } else if (state.clothState == ClothTakeDetectionState::DisarmedUntilRelease) {
      state.releaseStartedAt = 0;
    }
    update.nearby = state.nearby;
    return update;
  }

  const bool hasValidDistance = sample.kind == TofSampleKind::ValidDistance;
  const bool nearby = hasValidDistance && sample.distanceMm < config.personNearThresholdMm;
  if (nearby) {
    state.proximityExitCandidateActive = false;
    state.proximityExitStartedAt = 0;
    if (!state.proximityInitialized || !state.nearby) {
      state.proximityInitialized = true;
      state.nearby = true;
      update.proximityChanged = true;
    }
  } else if (state.proximityInitialized && !state.nearby) {
    state.proximityExitCandidateActive = false;
    state.proximityExitStartedAt = 0;
  } else {
    if (!state.proximityExitCandidateActive) {
      state.proximityExitCandidateActive = true;
      state.proximityExitStartedAt = nowMs;
    } else if (elapsedAtLeast(
                 nowMs,
                 state.proximityExitStartedAt,
                 config.proximityExitConfirmMs)) {
      state.proximityExitCandidateActive = false;
      state.proximityExitStartedAt = 0;
      if (!state.proximityInitialized || state.nearby) {
        state.proximityInitialized = true;
        state.nearby = false;
        update.proximityChanged = true;
      }
    }
  }
  update.nearby = state.nearby;

  switch (state.clothState) {
    case ClothTakeDetectionState::Armed:
      if (hasValidDistance && sample.distanceMm < config.clothTakenThresholdMm) {
        state.clothState = ClothTakeDetectionState::Candidate;
        state.candidateStartedAt = nowMs;
      }
      break;

    case ClothTakeDetectionState::Candidate:
      if (!hasValidDistance || sample.distanceMm >= config.clothTakenThresholdMm) {
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
      if (hasValidDistance && sample.distanceMm < config.clothReleaseThresholdMm) {
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
