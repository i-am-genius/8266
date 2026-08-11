#include "diagnostics/diagnostic_logger.h"

#include "app_config.h"
#include "online_logger.h"

static const uint32_t ARM_LOG_WINDOW_MS = 60000;
static const uint32_t HEALTH_LOG_INTERVAL_MS = 300000;

static ArmAggregate armAggregate{};
static FailureGate httpGates[DIAG_HTTP_ENDPOINT_COUNT]{};
static uint32_t lastHealthLogAt = 0;
static uint32_t wsReconnectCount = 0;
static bool wsHasConnected = false;
static uint32_t totalHttpFailures = 0;

static void copySanitized(char* output, size_t outputSize, const String& value) {
  if (!output || outputSize == 0) return;
  size_t length = min(value.length(), outputSize - 1);
  for (size_t i = 0; i < length; ++i) {
    char ch = value.charAt(i);
    output[i] = (ch == '\r' || ch == '\n') ? ' ' : ch;
  }
  output[length] = '\0';
}

static const char* endpointName(DiagnosticHttpEndpoint endpoint) {
  switch (endpoint) {
    case DIAG_HTTP_STATE_REPORT: return "state-report";
    case DIAG_HTTP_ANNOUNCE: return "announce";
    case DIAG_HTTP_LUX: return "lux";
    case DIAG_HTTP_DURATION: return "duration";
    default: return "unknown";
  }
}

void diagnosticInit() {
  armAggregate = ArmAggregate{};
  for (uint8_t i = 0; i < DIAG_HTTP_ENDPOINT_COUNT; ++i) {
    httpGates[i] = FailureGate{};
  }
  lastHealthLogAt = millis();
  wsReconnectCount = 0;
  wsHasConnected = false;
  totalHttpFailures = 0;

  char reason[40];
  char info[48];
  copySanitized(reason, sizeof(reason), ESP.getResetReason());
  copySanitized(info, sizeof(info), ESP.getResetInfo());

  char message[128];
  snprintf(
    message,
    sizeof(message),
    "reset=%s info=%s heap=%u max=%u frag=%u",
    reason,
    info,
    ESP.getFreeHeap(),
    ESP.getMaxFreeBlockSize(),
    ESP.getHeapFragmentation()
  );
  LOG_INFO("BOOT", message);
}

LightState diagnosticCurrentLightState() {
  return LightState{brightness, temp, autoMode};
}

void diagnosticLogLightChange(
  const char* source,
  const LightState& before,
  const LightState& after
) {
  char message[128];
  if (formatLightChange(source, before, after, message, sizeof(message))) {
    LOG_INFO("CONTROL", message);
  }
}

void diagnosticRecordArm(
  uint8_t source,
  const char* action,
  int pan,
  int tilt,
  int slider
) {
  armAggregateRecord(armAggregate, millis(), source, action, pan, tilt, slider);
}

void diagnosticRecordHttpResult(const char* endpoint, int code, unsigned long costMs) {
  DiagnosticHttpEndpoint endpointId = classifyHttpEndpoint(endpoint);
  if (endpointId == DIAG_HTTP_UNKNOWN) return;

  bool success = code >= 200 && code < 300;
  FailureGate& gate = httpGates[endpointId];
  uint32_t previousFailures = gate.consecutive;
  uint32_t firstFailureAt = gate.firstFailureAt;
  FailureDecision decision = failureGateRecord(gate, success, millis(), 10);

  char message[128];
  if (decision == DIAG_LOG_FAILURE) {
    totalHttpFailures++;
    snprintf(
      message,
      sizeof(message),
      "ep=%s code=%d cost=%lums fail=%lu",
      endpointName(endpointId),
      code,
      costMs,
      (unsigned long)gate.consecutive
    );
    LOG_WARN("HTTP", message);
  } else if (!success) {
    totalHttpFailures++;
  } else if (decision == DIAG_LOG_RECOVERY) {
    snprintf(
      message,
      sizeof(message),
      "ep=%s recovered fail=%lu after=%lums cost=%lums",
      endpointName(endpointId),
      (unsigned long)previousFailures,
      (unsigned long)(millis() - firstFailureAt),
      costMs
    );
    LOG_INFO("HTTP", message);
  }
}

static void logMessage(const char* module, const char* message, bool warning) {
  if (warning) LOG_WARN(module, message ? message : "unknown");
  else LOG_INFO(module, message ? message : "unknown");
}

void diagnosticLogWifi(const char* message, bool warning) {
  logMessage("WIFI", message, warning);
}

void diagnosticLogWs(const char* message, bool warning) {
  logMessage("WS", message, warning);
}

void diagnosticNoteWsConnected() {
  if (wsHasConnected) wsReconnectCount++;
  wsHasConnected = true;
}

void diagnosticLogSensor(const char* message, bool warning) {
  logMessage("SENSOR", message, warning);
}

void diagnosticLogNano(const char* message, bool warning) {
  logMessage("NANO", message, warning);
}

void diagnosticLogConfig(const char* message, bool error) {
  if (error) LOG_ERROR("CONFIG", message ? message : "unknown");
  else LOG_INFO("CONFIG", message ? message : "unknown");
}

void diagnosticLogTracking(const char* message, bool warning) {
  logMessage("TRACK", message, warning);
}

void diagnosticLogEffect(const char* message, bool warning) {
  logMessage("EFFECT", message, warning);
}

void diagnosticHandlePeriodic() {
  unsigned long now = millis();

  ArmSummary armSummary{};
  if (armAggregateTakeIfDue(armAggregate, now, ARM_LOG_WINDOW_MS, armSummary)) {
    char sources[24];
    formatDiagnosticSources(armSummary.sourceMask, sources, sizeof(sources));
    char message[128];
    snprintf(
      message,
      sizeof(message),
      "win=60s n=%lu src=%s last=%s pan=%d tilt=%d slider=%d",
      (unsigned long)armSummary.commandCount,
      sources,
      armSummary.lastAction,
      armSummary.pan,
      armSummary.tilt,
      armSummary.slider
    );
    LOG_INFO("ARM", message);
  }

  uint32_t recoveredFailures = 0;
  int recoveredCode = 0;
  if (consumeLogUploadRecovery(recoveredFailures, recoveredCode)) {
    char message[96];
    snprintf(
      message,
      sizeof(message),
      "upload recovered fail=%lu lastCode=%d",
      (unsigned long)recoveredFailures,
      recoveredCode
    );
    LOG_INFO("LOG", message);
  }

  if (!periodicTakeIfDue(now, HEALTH_LOG_INTERVAL_MS, lastHealthLogAt)) return;

  LogTransportStats stats = getLogTransportStats();
  uint32_t dropped = stats.dropped[0] + stats.dropped[1] + stats.dropped[2] + stats.dropped[3];
  int rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  char message[128];
  snprintf(
    message,
    sizeof(message),
    "up=%lu heap=%u max=%u frag=%u rssi=%d bri=%d temp=%d auto=%d wsRe=%lu httpFail=%lu drop=%lu",
    now,
    ESP.getFreeHeap(),
    ESP.getMaxFreeBlockSize(),
    ESP.getHeapFragmentation(),
    rssi,
    brightness,
    temp,
    autoMode ? 1 : 0,
    (unsigned long)wsReconnectCount,
    (unsigned long)totalHttpFailures,
    (unsigned long)dropped
  );
  LOG_INFO("HEALTH", message);
}

[[noreturn]] void diagnosticRestart(const char* reason) {
  char message[96];
  snprintf(message, sizeof(message), "reason=%s", reason ? reason : "unknown");
  LOG_WARN("REBOOT", message);
  uploadLogsBeforeRestart();
  yield();
  ESP.restart();
  while (true) yield();
}
