#include <unity.h>

#include "network/arm_position_policy.h"

void setUp() {}

void tearDown() {}

void test_capture_and_batch_return_require_arrival_task_id() {
  TEST_ASSERT_TRUE(armPositionSourceRequiresArrivalTaskId("camera_capture"));
  TEST_ASSERT_TRUE(armPositionSourceRequiresArrivalTaskId("camera_batch_return"));
}

void test_unrelated_motion_does_not_require_arrival_task_id() {
  TEST_ASSERT_FALSE(armPositionSourceRequiresArrivalTaskId("camera_tracking"));
  TEST_ASSERT_FALSE(armPositionSourceRequiresArrivalTaskId(""));
  TEST_ASSERT_FALSE(armPositionSourceRequiresArrivalTaskId(nullptr));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_capture_and_batch_return_require_arrival_task_id);
  RUN_TEST(test_unrelated_motion_does_not_require_arrival_task_id);
  return UNITY_END();
}
