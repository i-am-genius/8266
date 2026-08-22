#include <unity.h>

#include "network/wifi_boot_policy.h"

void setUp() {}

void tearDown() {}

void test_connected_attempt_finishes_immediately() {
  TEST_ASSERT_TRUE(shouldFinishWiFiAttempt(
    WiFiAttemptStatus::Connected,
    50,
    15000
  ));
}

void test_missing_ssid_waits_for_short_grace_period() {
  TEST_ASSERT_FALSE(shouldFinishWiFiAttempt(
    WiFiAttemptStatus::NoSsid,
    4999,
    15000
  ));
  TEST_ASSERT_TRUE(shouldFinishWiFiAttempt(
    WiFiAttemptStatus::NoSsid,
    5000,
    15000
  ));
}

void test_auth_failure_waits_for_short_grace_period() {
  TEST_ASSERT_FALSE(shouldFinishWiFiAttempt(
    WiFiAttemptStatus::ConnectFailed,
    1000,
    15000
  ));
  TEST_ASSERT_TRUE(shouldFinishWiFiAttempt(
    WiFiAttemptStatus::ConnectFailed,
    1500,
    15000
  ));
}

void test_disconnected_state_is_not_treated_as_terminal_before_timeout() {
  TEST_ASSERT_FALSE(shouldFinishWiFiAttempt(
    WiFiAttemptStatus::Disconnected,
    14000,
    15000
  ));
  TEST_ASSERT_TRUE(shouldFinishWiFiAttempt(
    WiFiAttemptStatus::Disconnected,
    15000,
    15000
  ));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_connected_attempt_finishes_immediately);
  RUN_TEST(test_missing_ssid_waits_for_short_grace_period);
  RUN_TEST(test_auth_failure_waits_for_short_grace_period);
  RUN_TEST(test_disconnected_state_is_not_treated_as_terminal_before_timeout);
  return UNITY_END();
}
