#include <unity.h>

#include "runtime/loop_policy.h"

void setUp() {}
void tearDown() {}

void test_allows_non_critical_http_when_wave_is_idle() {
  TEST_ASSERT_TRUE(shouldRunNonCriticalHttp(false));
}

void test_defers_non_critical_http_while_wave_is_running() {
  TEST_ASSERT_FALSE(shouldRunNonCriticalHttp(true));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_allows_non_critical_http_when_wave_is_idle);
  RUN_TEST(test_defers_non_critical_http_while_wave_is_running);
  return UNITY_END();
}
