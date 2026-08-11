#include "diagnostics/diagnostic_policy.h"

#include <stdio.h>
#include <string.h>

bool lightStateChanged(const LightState& before, const LightState& after) {
  return before.brightness != after.brightness ||
         before.temperature != after.temperature ||
         before.autoMode != after.autoMode;
}

bool formatLightChange(
  const char* source,
  const LightState& before,
  const LightState& after,
  char* output,
  size_t outputSize
) {
  if (!output || outputSize == 0 || !lightStateChanged(before, after)) {
    return false;
  }

  snprintf(
    output,
    outputSize,
    "src=%s bri=%d>%d temp=%d>%d auto=%d>%d",
    source ? source : "unknown",
    before.brightness,
    after.brightness,
    before.temperature,
    after.temperature,
    before.autoMode ? 1 : 0,
    after.autoMode ? 1 : 0
  );
  output[outputSize - 1] = '\0';
  return true;
}

void armAggregateRecord(
  ArmAggregate& state,
  uint32_t now,
  uint8_t source,
  const char* action,
  int pan,
  int tilt,
  int slider
) {
  if (!state.active) {
    state = ArmAggregate{};
    state.active = true;
    state.startedAt = now;
  }

  state.commandCount++;
  state.sourceMask |= source;
  strncpy(state.lastAction, action ? action : "unknown", sizeof(state.lastAction) - 1);
  state.lastAction[sizeof(state.lastAction) - 1] = '\0';
  state.pan = pan;
  state.tilt = tilt;
  state.slider = slider;
}

bool armAggregateTakeIfDue(
  ArmAggregate& state,
  uint32_t now,
  uint32_t windowMs,
  ArmSummary& summary
) {
  if (!state.active || static_cast<uint32_t>(now - state.startedAt) < windowMs) {
    return false;
  }

  summary.commandCount = state.commandCount;
  summary.sourceMask = state.sourceMask;
  strncpy(summary.lastAction, state.lastAction, sizeof(summary.lastAction) - 1);
  summary.lastAction[sizeof(summary.lastAction) - 1] = '\0';
  summary.pan = state.pan;
  summary.tilt = state.tilt;
  summary.slider = state.slider;
  state = ArmAggregate{};
  return true;
}

static void appendSource(char* output, size_t outputSize, const char* value) {
  if (!output || outputSize == 0 || !value) return;
  size_t used = strlen(output);
  if (used > 0 && used < outputSize - 1) {
    strncat(output, "|", outputSize - used - 1);
    used = strlen(output);
  }
  if (used < outputSize - 1) {
    strncat(output, value, outputSize - used - 1);
  }
}

void formatDiagnosticSources(uint8_t mask, char* output, size_t outputSize) {
  if (!output || outputSize == 0) return;
  output[0] = '\0';
  if (mask & DIAG_SOURCE_WS) appendSource(output, outputSize, "ws");
  if (mask & DIAG_SOURCE_TRACK) appendSource(output, outputSize, "track");
  if (mask & DIAG_SOURCE_LOCAL) appendSource(output, outputSize, "local");
  if (mask & DIAG_SOURCE_EFFECT) appendSource(output, outputSize, "effect");
  if (output[0] == '\0') appendSource(output, outputSize, "unknown");
}

FailureDecision failureGateRecord(
  FailureGate& gate,
  bool success,
  uint32_t now,
  uint32_t cadence
) {
  if (success) {
    if (!gate.failing) return DIAG_LOG_NONE;
    gate = FailureGate{};
    return DIAG_LOG_RECOVERY;
  }

  if (!gate.failing) {
    gate.failing = true;
    gate.firstFailureAt = now;
  }
  gate.consecutive++;
  if (gate.consecutive == 1 || (cadence > 0 && gate.consecutive % cadence == 0)) {
    return DIAG_LOG_FAILURE;
  }
  return DIAG_LOG_NONE;
}

FailureDecision thresholdGateRecord(
  ThresholdGate& gate,
  bool valid,
  uint32_t threshold
) {
  if (valid) {
    bool recovered = gate.latched;
    gate = ThresholdGate{};
    return recovered ? DIAG_LOG_RECOVERY : DIAG_LOG_NONE;
  }

  gate.consecutive++;
  if (!gate.latched && threshold > 0 && gate.consecutive >= threshold) {
    gate.latched = true;
    return DIAG_LOG_FAILURE;
  }
  return DIAG_LOG_NONE;
}

bool periodicTakeIfDue(uint32_t now, uint32_t interval, uint32_t& lastAt) {
  if (static_cast<uint32_t>(now - lastAt) < interval) return false;
  lastAt = now;
  return true;
}

LogWriteSlot selectLogWriteSlot(
  const uint8_t* levels,
  size_t count,
  size_t capacity,
  size_t head,
  uint8_t newLevel
) {
  if (capacity == 0) return LogWriteSlot{false, false, 0, newLevel};
  if (count < capacity) return LogWriteSlot{true, false, head, 255};
  if (newLevel < 2 || !levels) return LogWriteSlot{false, false, 0, newLevel};

  for (size_t i = 0; i < capacity; ++i) {
    size_t index = (head + i) % capacity;
    if (levels[index] < 2) {
      return LogWriteSlot{true, true, index, levels[index]};
    }
  }
  return LogWriteSlot{false, false, 0, newLevel};
}

size_t newestLogBatchStart(size_t head, size_t count, size_t capacity, size_t batchSize) {
  if (capacity == 0) return 0;
  size_t selected = count < batchSize ? count : batchSize;
  return (head + capacity - (selected % capacity)) % capacity;
}

void recordLogDrop(LogTransportPolicy& state, uint8_t level) {
  if (level < 4) state.dropped[level]++;
}

void recordLogUploadResult(LogTransportPolicy& state, bool success, int code) {
  if (!success) {
    state.totalUploadFailures++;
    state.consecutiveUploadFailures++;
    state.lastUploadCode = code;
    return;
  }

  if (state.consecutiveUploadFailures > 0) {
    state.recoveryPending = true;
    state.recoveredFailureCount = state.consecutiveUploadFailures;
    state.recoveredLastCode = state.lastUploadCode;
    state.consecutiveUploadFailures = 0;
  }
}

bool consumeLogRecovery(LogTransportPolicy& state, uint32_t& failureCount, int& lastCode) {
  if (!state.recoveryPending) return false;
  failureCount = state.recoveredFailureCount;
  lastCode = state.recoveredLastCode;
  state.recoveryPending = false;
  state.recoveredFailureCount = 0;
  return true;
}

DiagnosticHttpEndpoint classifyHttpEndpoint(const char* path) {
  if (!path) return DIAG_HTTP_UNKNOWN;
  if (strcmp(path, "/admin/device/state-report") == 0) return DIAG_HTTP_STATE_REPORT;
  if (strcmp(path, "/admin/device/announce") == 0) return DIAG_HTTP_ANNOUNCE;
  if (strcmp(path, "/admin/lux/create") == 0) return DIAG_HTTP_LUX;
  if (strcmp(path, "/admin/duration/create") == 0) return DIAG_HTTP_DURATION;
  return DIAG_HTTP_UNKNOWN;
}
