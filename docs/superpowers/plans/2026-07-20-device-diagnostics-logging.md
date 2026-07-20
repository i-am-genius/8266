# Device Diagnostics Logging Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add actionable ESP8266 diagnostic logs, loss-aware delivery, deduplicated light-control events, and one ARM summary per active 60-second window without changing the backend protocol.

**Architecture:** Keep `online_logger` as the fixed-size transport and add pure C++ policy helpers plus a centralized `diagnostic_logger`. Business modules emit semantic events; the diagnostic layer owns rate limiting, aggregation, health reporting, and planned-restart delivery.

**Tech Stack:** PlatformIO, Arduino ESP8266, C++17-compatible embedded code, PlatformIO Unity native tests, existing HTTP NDJSON logging endpoint.

## Global Constraints

- Keep the existing `module + msg + uptimeMs` NDJSON protocol unchanged.
- Keep `LogEntry::message` at 128 bytes and the ring buffer at 32 entries.
- Do not log credentials, secrets, complete signed OTA URLs, raw JSON commands, Nano TX/RX streams, sensor samples, heartbeats, joystick packets, or tracking coordinates.
- Light-control logs are emitted only when brightness, color temperature, or auto mode actually changes.
- The first ARM command opens a non-extending 60-second aggregation window; inactive windows emit nothing.
- Planned restart delivery attempts one newest-entry batch and must include the `REBOOT` record.
- Preserve unrelated changes already present in the dirty worktree.

---

## File Structure

- Create `include/diagnostics/diagnostic_policy.h`: Arduino-independent state machines and ring-index helpers.
- Create `src/diagnostics/diagnostic_policy.cpp`: light comparison, ARM window, failure gate, periodic gate, and ring-slot algorithms.
- Create `include/diagnostics/diagnostic_logger.h`: semantic logging interface used by firmware modules.
- Create `src/diagnostics/diagnostic_logger.cpp`: ESP/Wi-Fi-aware formatting and periodic diagnostics.
- Create `test/test_diagnostic_policy/test_main.cpp`: native Unity tests for all pure policy behavior.
- Modify `include/online_logger.h` and `src/online_logger.cpp`: transport statistics and restart-specific newest-batch upload.
- Modify `platformio.ini`: isolated native test environment.
- Modify existing firmware modules only at their event boundaries.

### Task 1: Pure Diagnostic Policies

**Files:**
- Create: `include/diagnostics/diagnostic_policy.h`
- Create: `src/diagnostics/diagnostic_policy.cpp`
- Create: `test/test_diagnostic_policy/test_main.cpp`
- Modify: `platformio.ini`

**Interfaces:**
- Produces: `LightState`, `ArmAggregate`, `ArmSummary`, `FailureGate`, `FailureDecision`, `periodicDue`, `selectLogWriteSlot`, and `newestLogBatchStart`.
- Consumes: standard fixed-width integers and fixed-size character arrays only.

- [ ] **Step 1: Add the isolated native test environment**

Append this exact environment to `platformio.ini`:

```ini
[env:native]
platform = native
test_framework = unity
test_build_src = yes
build_src_filter =
    -<*>
    +<diagnostics/diagnostic_policy.cpp>
build_flags =
    -std=gnu++17
```

- [ ] **Step 2: Write failing policy tests**

Create `test/test_diagnostic_policy/test_main.cpp` with Unity cases that call the intended API:

```cpp
#include <unity.h>
#include "diagnostics/diagnostic_policy.h"

void test_light_change_detects_only_real_changes() {
  LightState before{80, 4000, true};
  TEST_ASSERT_FALSE(lightStateChanged(before, before));
  TEST_ASSERT_TRUE(lightStateChanged(before, LightState{60, 4000, true}));
  TEST_ASSERT_TRUE(lightStateChanged(before, LightState{80, 5200, true}));
  TEST_ASSERT_TRUE(lightStateChanged(before, LightState{80, 4000, false}));
}

void test_arm_window_aggregates_without_extending_deadline() {
  ArmAggregate state{};
  armAggregateRecord(state, 1000, DIAG_SOURCE_WS, "joystick", 1, 2, 3);
  armAggregateRecord(state, 59000, DIAG_SOURCE_TRACK, "coord", 9, 8, 7);
  ArmSummary summary{};
  TEST_ASSERT_FALSE(armAggregateTakeIfDue(state, 60999, 60000, summary));
  TEST_ASSERT_TRUE(armAggregateTakeIfDue(state, 61000, 60000, summary));
  TEST_ASSERT_EQUAL_UINT32(2, summary.commandCount);
  TEST_ASSERT_EQUAL_UINT8(DIAG_SOURCE_WS | DIAG_SOURCE_TRACK, summary.sourceMask);
  TEST_ASSERT_EQUAL_STRING("coord", summary.lastAction);
  TEST_ASSERT_EQUAL_INT(9, summary.pan);
  TEST_ASSERT_FALSE(state.active);
}

void test_arm_window_handles_millis_wraparound() {
  ArmAggregate state{};
  armAggregateRecord(state, 0xFFFFFF00u, DIAG_SOURCE_WS, "right", 5, 0, 0);
  ArmSummary summary{};
  TEST_ASSERT_TRUE(armAggregateTakeIfDue(state, 0x0000E960u, 60000, summary));
}

void test_failure_gate_logs_first_tenth_and_recovery() {
  FailureGate gate{};
  TEST_ASSERT_EQUAL(DIAG_LOG_FAILURE, failureGateRecord(gate, false, 100, 10));
  for (int i = 2; i < 10; ++i) {
    TEST_ASSERT_EQUAL(DIAG_LOG_NONE, failureGateRecord(gate, false, 100 + i, 10));
  }
  TEST_ASSERT_EQUAL(DIAG_LOG_FAILURE, failureGateRecord(gate, false, 110, 10));
  TEST_ASSERT_EQUAL(DIAG_LOG_RECOVERY, failureGateRecord(gate, true, 200, 10));
  TEST_ASSERT_EQUAL(DIAG_LOG_NONE, failureGateRecord(gate, true, 201, 10));
}

void test_periodic_gate_and_ring_helpers() {
  uint32_t last = 100;
  TEST_ASSERT_FALSE(periodicTakeIfDue(200, 300000, last));
  TEST_ASSERT_TRUE(periodicTakeIfDue(300100, 300000, last));
  TEST_ASSERT_EQUAL_UINT32(300100, last);

  const uint8_t levels[4] = {1, 1, 2, 3};
  LogWriteSlot slot = selectLogWriteSlot(levels, 4, 4, 0, 2);
  TEST_ASSERT_TRUE(slot.accept);
  TEST_ASSERT_TRUE(slot.replacing);
  TEST_ASSERT_EQUAL_UINT32(0, slot.index);
  TEST_ASSERT_EQUAL_UINT32(3, newestLogBatchStart(1, 3, 4, 2));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_light_change_detects_only_real_changes);
  RUN_TEST(test_arm_window_aggregates_without_extending_deadline);
  RUN_TEST(test_arm_window_handles_millis_wraparound);
  RUN_TEST(test_failure_gate_logs_first_tenth_and_recovery);
  RUN_TEST(test_periodic_gate_and_ring_helpers);
  return UNITY_END();
}
```

- [ ] **Step 3: Run the tests and verify RED**

Run:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" test -e native
```

Expected: compilation fails because `diagnostics/diagnostic_policy.h` does not exist.

- [ ] **Step 4: Implement the minimal pure policy API**

Create the header with these exact public types and functions:

```cpp
#pragma once
#include <stddef.h>
#include <stdint.h>

enum DiagnosticSource : uint8_t {
  DIAG_SOURCE_WS = 1,
  DIAG_SOURCE_TRACK = 2,
  DIAG_SOURCE_LOCAL = 4,
  DIAG_SOURCE_EFFECT = 8,
};

struct LightState { int brightness; int temperature; bool autoMode; };
bool lightStateChanged(const LightState& before, const LightState& after);

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
void armAggregateRecord(ArmAggregate&, uint32_t, uint8_t, const char*, int, int, int);
bool armAggregateTakeIfDue(ArmAggregate&, uint32_t, uint32_t, ArmSummary&);

enum FailureDecision : uint8_t { DIAG_LOG_NONE, DIAG_LOG_FAILURE, DIAG_LOG_RECOVERY };
struct FailureGate { uint32_t consecutive; uint32_t firstFailureAt; bool failing; };
FailureDecision failureGateRecord(FailureGate&, bool, uint32_t, uint32_t);
bool periodicTakeIfDue(uint32_t now, uint32_t interval, uint32_t& lastAt);

struct LogWriteSlot { bool accept; bool replacing; size_t index; uint8_t droppedLevel; };
LogWriteSlot selectLogWriteSlot(const uint8_t*, size_t, size_t, size_t, uint8_t);
size_t newestLogBatchStart(size_t head, size_t count, size_t capacity, size_t batchSize);
```

Implement with unsigned subtraction for time checks, `strncpy` plus explicit termination, first/each-N failure decisions, and the current WARN/ERROR replacement policy.

- [ ] **Step 5: Run the native tests and verify GREEN**

Run the same `pio test -e native` command. Expected: all five tests pass.

- [ ] **Step 6: Commit the policy slice**

```powershell
git add platformio.ini include/diagnostics/diagnostic_policy.h src/diagnostics/diagnostic_policy.cpp test/test_diagnostic_policy/test_main.cpp
git commit -m "test: define diagnostic logging policies"
```

### Task 2: Loss-Aware Online Logger Transport

**Files:**
- Modify: `include/diagnostics/diagnostic_policy.h`
- Modify: `src/diagnostics/diagnostic_policy.cpp`
- Modify: `include/online_logger.h`
- Modify: `src/online_logger.cpp`
- Modify: `test/test_diagnostic_policy/test_main.cpp`

**Interfaces:**
- Consumes: `selectLogWriteSlot` and `newestLogBatchStart` from Task 1.
- Produces: `LogTransportStats`, `getLogTransportStats()`, `consumeLogUploadRecovery(...)`, and `uploadLogsBeforeRestart()`.

- [ ] **Step 1: Extend tests for buffer decisions and newest restart batches**

Add these exact cases to `test/test_diagnostic_policy/test_main.cpp` and register them in `main`:

```cpp
void test_full_critical_buffer_drops_new_info() {
  const uint8_t levels[4] = {2, 3, 2, 3};
  LogWriteSlot slot = selectLogWriteSlot(levels, 4, 4, 0, 1);
  TEST_ASSERT_FALSE(slot.accept);
  TEST_ASSERT_EQUAL_UINT8(1, slot.droppedLevel);
}

void test_warn_replaces_oldest_low_priority_entry() {
  const uint8_t levels[4] = {3, 1, 0, 2};
  LogWriteSlot slot = selectLogWriteSlot(levels, 4, 4, 1, 2);
  TEST_ASSERT_TRUE(slot.accept);
  TEST_ASSERT_TRUE(slot.replacing);
  TEST_ASSERT_EQUAL_UINT32(1, slot.index);
  TEST_ASSERT_EQUAL_UINT8(1, slot.droppedLevel);
}

void test_newest_batch_start_handles_wrapped_ring() {
  TEST_ASSERT_EQUAL_UINT32(3, newestLogBatchStart(1, 3, 4, 2));
  TEST_ASSERT_EQUAL_UINT32(2, newestLogBatchStart(1, 3, 4, 3));
}

void test_transport_policy_tracks_failure_and_recovery() {
  LogTransportPolicy state{};
  recordLogDrop(state, 1);
  recordLogDrop(state, 1);
  TEST_ASSERT_EQUAL_UINT32(2, state.dropped[1]);

  recordLogUploadResult(state, false, -11);
  recordLogUploadResult(state, false, 500);
  TEST_ASSERT_EQUAL_UINT32(2, state.totalUploadFailures);
  TEST_ASSERT_EQUAL_UINT32(2, state.consecutiveUploadFailures);
  TEST_ASSERT_EQUAL_INT(500, state.lastUploadCode);

  recordLogUploadResult(state, true, 200);
  uint32_t recoveredFailures = 0;
  int recoveredCode = 0;
  TEST_ASSERT_TRUE(consumeLogRecovery(state, recoveredFailures, recoveredCode));
  TEST_ASSERT_EQUAL_UINT32(2, recoveredFailures);
  TEST_ASSERT_EQUAL_INT(500, recoveredCode);
  TEST_ASSERT_FALSE(consumeLogRecovery(state, recoveredFailures, recoveredCode));
}
```

Add the expected `LogTransportPolicy` declarations to the test include contract, run native tests, and verify RED because the transport policy functions do not exist.

Add this API to `diagnostic_policy.h` before implementing it:

```cpp
struct LogTransportPolicy {
  uint32_t dropped[4];
  uint32_t totalUploadFailures;
  uint32_t consecutiveUploadFailures;
  int lastUploadCode;
  bool recoveryPending;
  uint32_t recoveredFailureCount;
  int recoveredLastCode;
};
void recordLogDrop(LogTransportPolicy&, uint8_t level);
void recordLogUploadResult(LogTransportPolicy&, bool success, int code);
bool consumeLogRecovery(LogTransportPolicy&, uint32_t& failureCount, int& lastCode);
```

Implement the model in `diagnostic_policy.cpp`, then make `online_logger.cpp` use one `LogTransportPolicy` instance.

- [ ] **Step 2: Add the transport API**

Add to `include/online_logger.h`:

```cpp
struct LogTransportStats {
  uint32_t dropped[4];
  uint32_t totalUploadFailures;
  uint32_t consecutiveUploadFailures;
  int lastUploadCode;
};

LogTransportStats getLogTransportStats();
bool consumeLogUploadRecovery(uint32_t& failureCount, int& lastCode);
bool uploadLogsBeforeRestart();
```

Refactor `online_logger.cpp` so normal `uploadLogs()` sends the oldest batch, while `uploadLogsBeforeRestart()` sends the newest `min(logCount, 20)` entries in chronological order and ignores `lastUploadMs`. Do not write `LOG_*` from inside either upload path.

- [ ] **Step 3: Record drops and upload recovery**

Use `selectLogWriteSlot` for writes. Increment the correct `dropped[level]` counter whenever a new log or replaced log is discarded. On failed POST, update failure counters; on the first success after failures, store a one-shot recovery snapshot returned by `consumeLogUploadRecovery`.

- [ ] **Step 4: Verify native tests and ESP compilation**

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" test -e native
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp12e
```

Expected: tests pass and firmware links successfully.

- [ ] **Step 5: Commit the transport slice**

```powershell
git add include/diagnostics/diagnostic_policy.h src/diagnostics/diagnostic_policy.cpp include/online_logger.h src/online_logger.cpp test/test_diagnostic_policy/test_main.cpp
git commit -m "feat: make device log delivery observable"
```

### Task 3: Central Diagnostic Logger and Periodic Health

**Files:**
- Modify: `include/diagnostics/diagnostic_policy.h`
- Modify: `src/diagnostics/diagnostic_policy.cpp`
- Create: `include/diagnostics/diagnostic_logger.h`
- Create: `src/diagnostics/diagnostic_logger.cpp`
- Modify: `src/main.cpp`
- Modify: `test/test_diagnostic_policy/test_main.cpp`

**Interfaces:**
- Produces: semantic event functions used by Tasks 4-6.
- Consumes: policy types from Task 1 and transport statistics from Task 2.

- [ ] **Step 1: Add failing formatting and scheduling tests**

Add the intended formatter declaration to the test via the public header and add these cases:

```cpp
void test_source_mask_formats_in_stable_order() {
  char value[24] = {};
  formatDiagnosticSources(DIAG_SOURCE_LOCAL | DIAG_SOURCE_WS | DIAG_SOURCE_TRACK,
                          value, sizeof(value));
  TEST_ASSERT_EQUAL_STRING("ws|track|local", value);
}

void test_arm_action_is_bounded_and_terminated() {
  ArmAggregate state{};
  armAggregateRecord(state, 1, DIAG_SOURCE_WS,
                     "12345678901234567890", 0, 0, 0);
  TEST_ASSERT_EQUAL_UINT32(15, strlen(state.lastAction));
  TEST_ASSERT_EQUAL_CHAR('\0', state.lastAction[15]);
}

void test_health_gate_handles_wraparound() {
  uint32_t last = 0xFFFF0000u;
  TEST_ASSERT_TRUE(periodicTakeIfDue(0x000393E0u, 300000u, last));
}
```

Add `void formatDiagnosticSources(uint8_t mask, char* out, size_t outSize);` to the expected policy API, run native tests, and verify RED because the function is undefined.

- [ ] **Step 2: Create the semantic logger header**

Declare these interfaces:

```cpp
#pragma once
#include "diagnostics/diagnostic_policy.h"

void diagnosticInit();
void diagnosticHandlePeriodic();
LightState diagnosticCurrentLightState();
void diagnosticLogLightChange(const char* source, const LightState& before, const LightState& after);
void diagnosticRecordArm(uint8_t source, const char* action, int pan, int tilt, int slider);
void diagnosticRecordHttpResult(const char* endpoint, int code, unsigned long costMs);
void diagnosticLogWifi(const char* message, bool warning = false);
void diagnosticLogWs(const char* message, bool warning = false);
void diagnosticLogSensor(const char* message, bool warning = false);
void diagnosticLogNano(const char* message, bool warning = false);
void diagnosticLogConfig(const char* message, bool error = false);
void diagnosticLogTracking(const char* message, bool warning = false);
void diagnosticRestart(const char* reason);
```

- [ ] **Step 3: Implement boot, health, light, ARM, and recovery logs**

`diagnosticInit()` writes reset reason/info and heap metrics after logger server/device configuration is set. `diagnosticHandlePeriodic()` emits ARM summaries, five-minute HEALTH records, and one upload-recovery summary. Use fixed 128-byte stack buffers and abbreviated fields.

- [ ] **Step 4: Wire setup and loop**

In `setup()`, call `diagnosticInit()` immediately after `logSetDeviceId`. In `loop()`, call `diagnosticHandlePeriodic()` immediately before `uploadLogs()` so newly due records can enter the same upload cycle.

- [ ] **Step 5: Verify tests and ESP build, then commit**

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" test -e native
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp12e
git add include/diagnostics src/diagnostics src/main.cpp test/test_diagnostic_policy/test_main.cpp
git commit -m "feat: add centralized device diagnostics"
```

### Task 4: Light and ARM Control Events

**Files:**
- Modify: `include/diagnostics/diagnostic_policy.h`
- Modify: `src/diagnostics/diagnostic_policy.cpp`
- Modify: `test/test_diagnostic_policy/test_main.cpp`
- Modify: `src/network/ws_client.cpp`
- Modify: `src/server/local_server.cpp`
- Modify: `src/network/tracking_receiver.cpp`
- Modify: `src/device/light_control.cpp`

**Interfaces:**
- Consumes: `diagnosticCurrentLightState`, `diagnosticLogLightChange`, and `diagnosticRecordArm`.

- [ ] **Step 1: Add a failing test for one merged light change**

Add this formatter test and expected declaration to the policy header:

```cpp
void test_light_change_formats_one_merged_message() {
  char message[128] = {};
  bool written = formatLightChange("ws", LightState{80, 4000, true},
                                  LightState{60, 5200, false},
                                  message, sizeof(message));
  TEST_ASSERT_TRUE(written);
  TEST_ASSERT_EQUAL_STRING("src=ws bri=80>60 temp=4000>5200 auto=1>0", message);

  TEST_ASSERT_FALSE(formatLightChange("ws", LightState{60, 5200, false},
                                     LightState{60, 5200, false},
                                     message, sizeof(message)));
}
```

Declare `bool formatLightChange(const char*, const LightState&, const LightState&, char*, size_t);`, run native tests, and verify RED because the formatter is undefined. Implement it in `diagnostic_policy.cpp` with `snprintf`, returning false for an unchanged state.

- [ ] **Step 2: Instrument WebSocket light changes**

For `state/control` and effect start, capture `LightState before` before mutation and `after` after all fields are assigned, then call exactly once:

```cpp
diagnosticLogLightChange("ws", before, after);
```

Use source `effect` for the one-time wave start transition. Do not log effect frames.

- [ ] **Step 3: Instrument local light changes**

Apply the same before/after pattern to `/setLight` and `/lamp/control` with source `local`. A request with unchanged effective values must create no log.

- [ ] **Step 4: Feed ARM aggregation**

Record recognized WebSocket joystick, position, speed, and action commands with `DIAG_SOURCE_WS`; local pan/tilt control with `DIAG_SOURCE_LOCAL`; accepted tracking coordinates with `DIAG_SOURCE_TRACK`. Pass final global `panDeg`, `tiltDeg`, and `sliderMm`. Do not record ignored or malformed commands as ARM activity.

- [ ] **Step 5: Build and commit**

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" test -e native
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp12e
git add include/diagnostics/diagnostic_policy.h src/diagnostics/diagnostic_policy.cpp test/test_diagnostic_policy/test_main.cpp src/network/ws_client.cpp src/server/local_server.cpp src/network/tracking_receiver.cpp src/device/light_control.cpp
git commit -m "feat: log light changes and aggregate arm control"
```

### Task 5: Network, HTTP, and Restart Diagnostics

**Files:**
- Modify: `include/diagnostics/diagnostic_policy.h`
- Modify: `src/diagnostics/diagnostic_policy.cpp`
- Modify: `test/test_diagnostic_policy/test_main.cpp`
- Modify: `src/network/wifi_manager.cpp`
- Modify: `src/network/ws_client.cpp`
- Modify: `src/network/http_reporter.cpp`
- Modify: `src/device/ota_manager.cpp`
- Modify: `src/server/local_server.cpp`

**Interfaces:**
- Consumes: HTTP failure gates and semantic network/restart APIs.

- [ ] **Step 1: Add endpoint failure-gate tests**

Add a pure classifier and independence test:

```cpp
void test_http_endpoint_gates_are_independent() {
  TEST_ASSERT_EQUAL(DIAG_HTTP_STATE_REPORT, classifyHttpEndpoint("/admin/device/state-report"));
  TEST_ASSERT_EQUAL(DIAG_HTTP_ANNOUNCE, classifyHttpEndpoint("/admin/device/announce"));
  TEST_ASSERT_EQUAL(DIAG_HTTP_LUX, classifyHttpEndpoint("/admin/lux/create"));
  TEST_ASSERT_EQUAL(DIAG_HTTP_DURATION, classifyHttpEndpoint("/admin/duration/create"));

  FailureGate gates[DIAG_HTTP_ENDPOINT_COUNT]{};
  TEST_ASSERT_EQUAL(DIAG_LOG_FAILURE,
                    failureGateRecord(gates[DIAG_HTTP_STATE_REPORT], false, 10, 10));
  TEST_ASSERT_EQUAL(DIAG_LOG_NONE,
                    failureGateRecord(gates[DIAG_HTTP_ANNOUNCE], true, 20, 10));
  TEST_ASSERT_TRUE(gates[DIAG_HTTP_STATE_REPORT].failing);
}
```

Add this API to `diagnostic_policy.h`:

```cpp
enum DiagnosticHttpEndpoint : uint8_t {
  DIAG_HTTP_STATE_REPORT,
  DIAG_HTTP_ANNOUNCE,
  DIAG_HTTP_LUX,
  DIAG_HTTP_DURATION,
  DIAG_HTTP_ENDPOINT_COUNT,
  DIAG_HTTP_UNKNOWN = 255,
};
DiagnosticHttpEndpoint classifyHttpEndpoint(const char* path);
```

Run native tests and verify RED because the classifier is undefined, then implement exact string matching and use an enum-indexed gate array in `diagnostic_logger.cpp`.

- [ ] **Step 2: Instrument HTTP outcomes**

Call `diagnosticRecordHttpResult(path, httpCode, costMs)` after every non-log HTTP request. Emit WARN on failure 1 and each 10th consecutive failure, INFO once on recovery, and nothing for ordinary success.

- [ ] **Step 3: Instrument Wi-Fi and WebSocket edges**

Record runtime Wi-Fi loss with status/RSSI, reconnect attempt and recovery duration, SmartConfig entry reason, WebSocket disconnect payload summary, connection duration, reconnect count, JSON parse failures, unknown types, and invalid OTA messages. Replace existing generic online WS logs to avoid duplicate records.

- [ ] **Step 4: Route all planned restarts through one API**

Replace every direct `ESP.restart()` in the scoped firmware with:

```cpp
diagnosticRestart("wifi_config_saved");
diagnosticRestart("wifi_config_cleared");
diagnosticRestart("smartconfig_start_failed");
diagnosticRestart("smartconfig_connect_failed");
diagnosticRestart("ota_success");
```

`diagnosticRestart` writes `REBOOT`, calls `uploadLogsBeforeRestart()`, yields, and restarts even if upload fails.

- [ ] **Step 5: Build and commit**

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" test -e native
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp12e
git add include/diagnostics/diagnostic_policy.h src/diagnostics/diagnostic_policy.cpp test/test_diagnostic_policy/test_main.cpp src/network/wifi_manager.cpp src/network/ws_client.cpp src/network/http_reporter.cpp src/device/ota_manager.cpp src/server/local_server.cpp
git commit -m "feat: add network and reboot diagnostics"
```

### Task 6: Sensor, Nano, Config, and Tracking Diagnostics

**Files:**
- Modify: `include/diagnostics/diagnostic_policy.h`
- Modify: `src/diagnostics/diagnostic_policy.cpp`
- Modify: `test/test_diagnostic_policy/test_main.cpp`
- Modify: `src/device/sensor_manager.cpp`
- Modify: `src/device/arm_controller.cpp`
- Modify: `src/device/self_test.cpp`
- Modify: `src/config/config_manager.cpp`
- Modify: `src/network/tracking_receiver.cpp`

**Interfaces:**
- Consumes: semantic SENSOR, NANO, CONFIG, and TRACK APIs plus `FailureGate`.

- [ ] **Step 1: Add threshold and recovery tests**

Add this API and tests:

```cpp
struct ThresholdGate { uint32_t consecutive; bool latched; };
FailureDecision thresholdGateRecord(ThresholdGate& gate, bool valid, uint32_t threshold);

void test_sensor_threshold_logs_once_and_recovers_once() {
  ThresholdGate tof{};
  for (uint32_t i = 1; i < 20; ++i) {
    TEST_ASSERT_EQUAL(DIAG_LOG_NONE, thresholdGateRecord(tof, false, 20));
  }
  TEST_ASSERT_EQUAL(DIAG_LOG_FAILURE, thresholdGateRecord(tof, false, 20));
  TEST_ASSERT_EQUAL(DIAG_LOG_NONE, thresholdGateRecord(tof, false, 20));
  TEST_ASSERT_EQUAL(DIAG_LOG_RECOVERY, thresholdGateRecord(tof, true, 20));
  TEST_ASSERT_EQUAL(DIAG_LOG_NONE, thresholdGateRecord(tof, true, 20));

  ThresholdGate bh{};
  TEST_ASSERT_EQUAL(DIAG_LOG_NONE, thresholdGateRecord(bh, false, 3));
  TEST_ASSERT_EQUAL(DIAG_LOG_NONE, thresholdGateRecord(bh, false, 3));
  TEST_ASSERT_EQUAL(DIAG_LOG_FAILURE, thresholdGateRecord(bh, false, 3));
}
```

Tracking parse failures continue to use `FailureGate` with cadence 10. Run native tests and verify RED because `thresholdGateRecord` is undefined; implement it so the threshold latches until one valid sample produces recovery.

- [ ] **Step 2: Instrument sensor initialization and reads**

Log BH1750/VL53L0X initialization failures. Treat ToF status/range failures as invalid reads, logging after 20 consecutive failures and once on recovery. Treat non-finite or negative BH1750 results as invalid, logging after 3 and once on recovery. Preserve existing lighting behavior.

- [ ] **Step 3: Instrument Nano failures without high-frequency noise**

Log startup status timeout, homing failure/timeout, hall failure/timeout, oversized serial line, disabled-driver recovery, and the exact final self-test Nano status. Keep normal TX/RX serial-only.

- [ ] **Step 4: Instrument configuration and tracking lifecycle**

Log LittleFS config open/parse/serialize/remove failures without logging SSID passwords or upload secrets. Log tracking prepare, stop, timeout, UDP parse failure 1/10, and parse recovery; do not log coordinates.

- [ ] **Step 5: Build and commit**

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" test -e native
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp12e
git add include/diagnostics/diagnostic_policy.h src/diagnostics/diagnostic_policy.cpp test/test_diagnostic_policy/test_main.cpp src/device/sensor_manager.cpp src/device/arm_controller.cpp src/device/self_test.cpp src/config/config_manager.cpp src/network/tracking_receiver.cpp
git commit -m "feat: add peripheral diagnostics"
```

### Task 7: Final Verification and Documentation

**Files:**
- Modify: `docs/firmware_modules.md`

**Interfaces:**
- Consumes: all completed diagnostic interfaces.
- Produces: verified firmware and current module documentation.

- [ ] **Step 1: Run the complete native suite**

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" test -e native -v
```

Expected: all policy tests pass with no failed cases.

- [ ] **Step 2: Build the ESP8266 firmware from clean objects**

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp12e -t clean
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp12e
```

Expected: firmware builds successfully; record RAM and flash percentages from PlatformIO output.

- [ ] **Step 3: Audit logging volume and secrets**

Run:

```powershell
rg -n "LOG_|diagnostic" src include
rg -n -i "password|uploadSecret|signed.*url|X-Upload-Secret" src/diagnostics src/online_logger.cpp
```

Confirm no high-frequency loop emits online logs and no secret is formatted into a message.

- [ ] **Step 4: Update module documentation**

Add this section to `docs/firmware_modules.md`, adapting only heading level to the surrounding document:

```markdown
## 设备诊断日志

- `BOOT`: reset reason、reset info 和启动堆指标。
- `HEALTH`: 每 5 分钟记录 uptime、堆、RSSI、重连/失败/丢弃计数和灯光状态。
- `CONTROL`: 仅记录亮度、色温或自动模式的真实变化。
- `ARM`: 首个命令开启 60 秒窗口，每个活跃窗口输出一条聚合摘要。
- `HTTP`: 首次和每第 10 次连续失败告警，恢复时记录一次摘要。
- `REBOOT`: 所有计划重启先上传包含重启原因的最新日志批次。

高频心跳、传感器采样、Nano TX/RX、摇杆包和 tracking 坐标不上传。
```

- [ ] **Step 5: Review the complete diff**

```powershell
git diff --check HEAD~6..HEAD
git status --short
```

Confirm only scoped files were changed and pre-existing unrelated worktree changes remain intact.

- [ ] **Step 6: Commit documentation**

```powershell
git add docs/firmware_modules.md
git commit -m "docs: document device diagnostic logs"
```
