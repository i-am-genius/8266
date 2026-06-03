#pragma once
#include "app_config.h"

enum SelfTestState {
  SELFTEST_IDLE,
  SELFTEST_START,
  SELFTEST_CHECK_FS,
  SELFTEST_CHECK_WIFI,
  SELFTEST_CHECK_WS,
  SELFTEST_CHECK_BH1750,
  SELFTEST_CHECK_TOF,
  SELFTEST_NANO_SEND_HOMING,
  SELFTEST_NANO_WAIT_HOMING,
  SELFTEST_NANO_SEND_STATUS,
  SELFTEST_NANO_WAIT_STATUS,
  SELFTEST_DONE
};

void startDeviceSelfTest();
void handleSelfTestTask();
SelfTestState getSelfTestState();
void appendSelfTestJson(JsonObject root);
