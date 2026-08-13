#include <unity.h>

#include "runtime/loop_policy.h"

void setUp() {}
void tearDown() {}

void test_allows_non_critical_http_when_wave_and_tracking_are_idle() {
  TEST_ASSERT_TRUE(shouldRunNonCriticalHttp(false, false));
}

void test_defers_non_critical_http_while_wave_is_running() {
  TEST_ASSERT_FALSE(shouldRunNonCriticalHttp(true, false));
}

void test_defers_non_critical_http_while_person_tracking_is_active() {
  TEST_ASSERT_FALSE(shouldRunNonCriticalHttp(false, true));
}

void test_defers_non_critical_http_while_wave_and_tracking_are_active() {
  TEST_ASSERT_FALSE(shouldRunNonCriticalHttp(true, true));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_allows_non_critical_http_when_wave_and_tracking_are_idle);
  RUN_TEST(test_defers_non_critical_http_while_wave_is_running);
  RUN_TEST(test_defers_non_critical_http_while_person_tracking_is_active);
  RUN_TEST(test_defers_non_critical_http_while_wave_and_tracking_are_active);
  return UNITY_END();
}
