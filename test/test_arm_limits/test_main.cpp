#include <unity.h>

#include "device/arm_limits.h"

void setUp() {}
void tearDown() {}

void test_pan_supports_ninety_degree_boundaries() {
  TEST_ASSERT_EQUAL_INT(-90, PAN_MIN);
  TEST_ASSERT_EQUAL_INT(90, PAN_MAX);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_pan_supports_ninety_degree_boundaries);
  return UNITY_END();
}
