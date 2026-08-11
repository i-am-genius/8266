#pragma once

#include <stddef.h>
#include <stdint.h>

enum DiagnosticSource : uint8_t {
  DIAG_SOURCE_WS = 1,
  DIAG_SOURCE_TRACK = 2,
  DIAG_SOURCE_LOCAL = 4,
  DIAG_SOURCE_EFFECT = 8,
};

struct LightState {
  int brightness;
  int temperature;
  bool autoMode;
};

bool lightStateChanged(const LightState& before, const LightState& after);
bool formatLightChange(
  const char* source,
  const LightState& before,
  const LightState& after,
  char* output,
  size_t outputSize
);

struct ArmAggregate {
  bool active;
  uint32_t startedAt;
  uint32_t commandCount;
  uint8_t sourceMask;
  char lastAction[16];
  int pan;
  int tilt;
  int slider;
};

struct ArmSummary {
  uint32_t commandCount;
  uint8_t sourceMask;
  char lastAction[16];
  int pan;
  int tilt;
  int slider;
};

void armAggregateRecord(
  ArmAggregate& state,
  uint32_t now,
  uint8_t source,
  const char* action,
  int pan,
  int tilt,
  int slider
);
bool armAggregateTakeIfDue(
  ArmAggregate& state,
  uint32_t now,
  uint32_t windowMs,
  ArmSummary& summary
);
void formatDiagnosticSources(uint8_t mask, char* output, size_t outputSize);

enum FailureDecision : uint8_t {
  DIAG_LOG_NONE,
  DIAG_LOG_FAILURE,
  DIAG_LOG_RECOVERY,
};

struct FailureGate {
  uint32_t consecutive;
  uint32_t firstFailureAt;
  bool failing;
};

FailureDecision failureGateRecord(
  FailureGate& gate,
  bool success,
  uint32_t now,
  uint32_t cadence
);

struct ThresholdGate {
  uint32_t consecutive;
  bool latched;
};

FailureDecision thresholdGateRecord(
  ThresholdGate& gate,
  bool valid,
  uint32_t threshold
);

bool periodicTakeIfDue(uint32_t now, uint32_t interval, uint32_t& lastAt);

struct LogWriteSlot {
  bool accept;
  bool replacing;
  size_t index;
  uint8_t droppedLevel;
};

LogWriteSlot selectLogWriteSlot(
  const uint8_t* levels,
  size_t count,
  size_t capacity,
  size_t head,
  uint8_t newLevel
);
size_t newestLogBatchStart(size_t head, size_t count, size_t capacity, size_t batchSize);

struct LogTransportPolicy {
  uint32_t dropped[4];
  uint32_t totalUploadFailures;
  uint32_t consecutiveUploadFailures;
  int lastUploadCode;
  bool recoveryPending;
  uint32_t recoveredFailureCount;
  int recoveredLastCode;
};

void recordLogDrop(LogTransportPolicy& state, uint8_t level);
void recordLogUploadResult(LogTransportPolicy& state, bool success, int code);
bool consumeLogRecovery(LogTransportPolicy& state, uint32_t& failureCount, int& lastCode);

enum DiagnosticHttpEndpoint : uint8_t {
  DIAG_HTTP_STATE_REPORT,
  DIAG_HTTP_ANNOUNCE,
  DIAG_HTTP_LUX,
  DIAG_HTTP_DURATION,
  DIAG_HTTP_ENDPOINT_COUNT,
  DIAG_HTTP_UNKNOWN = 255,
};

DiagnosticHttpEndpoint classifyHttpEndpoint(const char* path);
