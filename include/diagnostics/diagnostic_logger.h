#pragma once

#include "diagnostics/diagnostic_policy.h"

void diagnosticInit();
void diagnosticHandlePeriodic();

LightState diagnosticCurrentLightState();
void diagnosticLogLightChange(
  const char* source,
  const LightState& before,
  const LightState& after
);
void diagnosticRecordArm(
  uint8_t source,
  const char* action,
  int pan,
  int tilt,
  int slider
);

void diagnosticRecordHttpResult(const char* endpoint, int code, unsigned long costMs);
void diagnosticLogWifi(const char* message, bool warning = false);
void diagnosticLogWs(const char* message, bool warning = false);
void diagnosticNoteWsConnected();
void diagnosticLogSensor(const char* message, bool warning = false);
void diagnosticLogNano(const char* message, bool warning = false);
void diagnosticLogConfig(const char* message, bool error = false);
void diagnosticLogTracking(const char* message, bool warning = false);
void diagnosticLogEffect(const char* message, bool warning = false);

[[noreturn]] void diagnosticRestart(const char* reason);
