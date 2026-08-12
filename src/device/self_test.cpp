#include "device/self_test.h"
#include "device/arm_controller.h"
#include "network/http_reporter.h"
#include "network/ws_client.h"
#include "online_logger.h"
#include "diagnostics/diagnostic_logger.h"

static const unsigned long SELFTEST_NANO_HOMING_TIMEOUT_MS = 30000;
static const unsigned long SELFTEST_NANO_STATUS_TIMEOUT_MS = 2000;

static SelfTestState selfTestState = SELFTEST_IDLE;
static unsigned long selfTestStepStartedAt = 0;
static bool lastNanoCommOk = false;

String hallStateFromNanoStatus(const String& status, const char* key) {
  String needle = String(key) + "=";
  int start = status.indexOf(needle);
  if (start < 0) {
    return "unknown";
  }

  start += needle.length();
  if (start >= (int)status.length()) {
    return "unknown";
  }

  char value = status.charAt(start);
  if (value == '1') {
    return "triggered";
  }
  if (value == '0') {
    return "clear";
  }
  return "unknown";
}

static void enterSelfTestState(SelfTestState nextState) {
  selfTestState = nextState;
  selfTestStepStartedAt = millis();
}

static bool hasFreshNanoLine() {
  return nanoLineSeen && lastNanoRxAt >= selfTestStepStartedAt;
}

static bool selfTestStepTimedOut(unsigned long timeoutMs) {
  return millis() - selfTestStepStartedAt >= timeoutMs;
}

static void finishSelfTest() {
  selfTestCheckedAtMs = millis();
  selfTestNanoOk = lastNanoCommOk;
  if (selfTestNanoStatus.length() == 0) {
    selfTestNanoStatus = selfTestNanoOk ? lastNanoLine : "no_response";
  }
  selfTestDone = true;

  DEBUG_SERIAL.printf(
    "[SELFTEST] fs=%d wifi=%d ws=%d bh1750=%d tof=%d nano=%d nanoStatus=%s\n",
    selfTestFsOk,
    selfTestWifiOk,
    selfTestWsOk,
    selfTestBh1750Ok,
    selfTestTofOk,
    selfTestNanoOk,
    selfTestNanoStatus.c_str()
  );

  bool overall = selfTestFsOk && selfTestWifiOk && selfTestWsOk && selfTestBh1750Ok && selfTestTofOk && selfTestNanoOk;
  String selfTestMsg = "自检完成 overall=" + String(overall ? 1 : 0) +
                       " fs=" + String(selfTestFsOk ? 1 : 0) +
                       " bh1750=" + String(selfTestBh1750Ok ? 1 : 0) +
                       " tof=" + String(selfTestTofOk ? 1 : 0) +
                       " nano=" + String(selfTestNanoOk ? 1 : 0);
  if (overall) {
    LOG_INFO("TEST", selfTestMsg.c_str());
  } else {
    LOG_WARN("TEST", selfTestMsg.c_str());
    if (!selfTestNanoOk || !lastNanoHomingOk || !lastNanoHallStatusOk) {
      diagnosticLogNano((String("selftest failed status=") + selfTestNanoStatus).c_str(), true);
    }
  }

  if (!bootSelfTestReportDone) {
    requestDeviceStateReport("SELFTEST_DONE", true);
  }
  enterSelfTestState(SELFTEST_IDLE);
}

void startDeviceSelfTest() {
  if (selfTestState != SELFTEST_IDLE) {
    return;
  }

  selfTestDone = false;
  selfTestCheckedAtMs = millis();
  selfTestFsOk = false;
  selfTestWifiOk = false;
  selfTestWsOk = false;
  selfTestBh1750Ok = false;
  selfTestTofOk = false;
  selfTestNanoOk = false;
  lastNanoCommOk = false;
  lastNanoHomingOk = false;
  lastNanoHallStatusOk = false;
  selfTestNanoStatus = "running";

  enterSelfTestState(SELFTEST_START);
}

void handleSelfTestTask() {
  switch (selfTestState) {
    case SELFTEST_IDLE:
      return;

    case SELFTEST_START:
      enterSelfTestState(SELFTEST_CHECK_FS);
      return;

    case SELFTEST_CHECK_FS:
      selfTestFsOk = fsReady;
      enterSelfTestState(SELFTEST_CHECK_WIFI);
      return;

    case SELFTEST_CHECK_WIFI:
      selfTestWifiOk = WiFi.status() == WL_CONNECTED;
      enterSelfTestState(SELFTEST_CHECK_WS);
      return;

    case SELFTEST_CHECK_WS:
      selfTestWsOk = wsConnected;
      enterSelfTestState(SELFTEST_CHECK_BH1750);
      return;

    case SELFTEST_CHECK_BH1750:
      selfTestBh1750Ok = bh1750Ready;
      enterSelfTestState(SELFTEST_CHECK_TOF);
      return;

    case SELFTEST_CHECK_TOF:
      selfTestTofOk = tofReady;
      if (selfTestNanoHomingEnabled) {
        enterSelfTestState(SELFTEST_NANO_SEND_HOMING);
      } else {
        lastNanoHomingOk = true;
        selfTestNanoStatus = "homing_disabled";
        enterSelfTestState(SELFTEST_NANO_SEND_STATUS);
      }
      return;

    case SELFTEST_NANO_SEND_HOMING:
      lastNanoHomingOk = false;
      lastNanoHallStatusOk = false;
      selfTestNanoStatus = "homing";
      enterSelfTestState(SELFTEST_NANO_WAIT_HOMING);
      sendNano('H', "2");
      sendNano('A');
      return;

    case SELFTEST_NANO_WAIT_HOMING:
      if (hasFreshNanoLine()) {
        lastNanoCommOk = true;

        if (lastNanoLine.indexOf("Complete") >= 0) {
          lastNanoHomingOk = true;
          panDeg = 0;
          tiltDeg = 0;
          enterSelfTestState(SELFTEST_NANO_SEND_STATUS);
          return;
        }

        if (lastNanoLine.indexOf("Error homing") >= 0) {
          lastNanoHomingOk = false;
          selfTestNanoStatus = lastNanoLine;
          diagnosticLogNano(lastNanoLine.c_str(), true);
          enterSelfTestState(SELFTEST_DONE);
          return;
        }
      }

      if (selfTestStepTimedOut(SELFTEST_NANO_HOMING_TIMEOUT_MS)) {
        lastNanoHomingOk = false;
        selfTestNanoStatus = "no_response";
        diagnosticLogNano("homing timeout", true);
        enterSelfTestState(SELFTEST_DONE);
      }
      return;

    case SELFTEST_NANO_SEND_STATUS:
      selfTestNanoStatus = selfTestNanoHallReadEnabled ? "checking_hall" : "checking_status";
      enterSelfTestState(SELFTEST_NANO_WAIT_STATUS);
      if (selfTestNanoHallReadEnabled) {
        sendNano('h');
      } else {
        sendNano('Q');
      }
      return;

    case SELFTEST_NANO_WAIT_STATUS:
      if (hasFreshNanoLine()) {
        if (selfTestNanoHallReadEnabled && lastNanoLine.indexOf("Hall pan=") >= 0) {
          lastNanoCommOk = true;
          lastNanoHallStatusOk = true;
          selfTestNanoStatus = lastNanoLine;
          enterSelfTestState(SELFTEST_DONE);
          return;
        }

        if (
          !selfTestNanoHallReadEnabled &&
          lastNanoLine.startsWith("NANO_OK ")
        ) {
          lastNanoCommOk = true;
          lastNanoHallStatusOk = true;
          selfTestNanoStatus = lastNanoLine;
          enterSelfTestState(SELFTEST_DONE);
          return;
        }
      }

      if (selfTestStepTimedOut(SELFTEST_NANO_STATUS_TIMEOUT_MS)) {
        lastNanoHallStatusOk = false;
        selfTestNanoStatus = selfTestNanoHallReadEnabled
            ? "hall_no_response"
            : "nano_no_response";
        diagnosticLogNano(
          selfTestNanoHallReadEnabled
              ? "hall status timeout"
              : "nano status timeout",
          true
        );
        enterSelfTestState(SELFTEST_DONE);
      }
      return;

    case SELFTEST_DONE:
      finishSelfTest();
      return;
  }
}

SelfTestState getSelfTestState() {
  return selfTestState;
}

void appendSelfTestJson(JsonObject root) {
  selfTestWifiOk = WiFi.status() == WL_CONNECTED;
  selfTestWsOk = wsConnected;

  bool nanoHomingRequiredOk = !selfTestNanoHomingEnabled || lastNanoHomingOk;
  bool nanoHallRequiredOk = !selfTestNanoHallReadEnabled || lastNanoHallStatusOk;
  bool overall = selfTestFsOk
      && selfTestWifiOk
      && selfTestWsOk
      && selfTestBh1750Ok
      && selfTestTofOk
      && selfTestNanoOk
      && nanoHomingRequiredOk
      && nanoHallRequiredOk;

  JsonObject selfTest = root["selfTest"].to<JsonObject>();
  selfTest["done"] = selfTestDone;
  selfTest["overall"] = overall;
  selfTest["checkedAtMs"] = selfTestCheckedAtMs;
  selfTest["fs"] = selfTestFsOk;
  selfTest["wifi"] = selfTestWifiOk;
  selfTest["ws"] = selfTestWsOk;
  selfTest["bh1750"] = selfTestBh1750Ok;
  selfTest["tof"] = selfTestTofOk;
  selfTest["nano"] = selfTestNanoOk;
  selfTest["nanoHomingEnabled"] = selfTestNanoHomingEnabled;
  selfTest["nanoHallReadEnabled"] = selfTestNanoHallReadEnabled;
  if (selfTestNanoHomingEnabled) {
    selfTest["nanoHoming"] = lastNanoHomingOk;
  } else {
    selfTest["nanoHoming"] = "disabled";
  }
  if (selfTestNanoHallReadEnabled) {
    selfTest["nanoHallStatus"] = lastNanoHallStatusOk;
  } else {
    selfTest["nanoHallStatus"] = "disabled";
  }
  selfTest["nanoStatus"] = selfTestNanoStatus;

  JsonObject hall = selfTest["hall"].to<JsonObject>();
  if (selfTestNanoHallReadEnabled) {
    hall["pan"] = hallStateFromNanoStatus(selfTestNanoStatus, "pan");
    hall["tilt"] = hallStateFromNanoStatus(selfTestNanoStatus, "tilt");
    hall["slider"] = hallStateFromNanoStatus(selfTestNanoStatus, "slider");
  } else {
    hall["pan"] = "disabled";
    hall["tilt"] = "disabled";
    hall["slider"] = "disabled";
  }
}
