#include <unity.h>
#include <cmath>

#include "device/garment_aim.h"

void setUp() {}
void tearDown() {}

void test_center_uses_configured_cloth_default() {
  GarmentAimTarget target = calculateGarmentAimTarget(
    0.5f, 0.5f, GarmentAimConfig{12.0f, 28.0f, 60.0f, 45.0f}
  );
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.0f, target.panDeg);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 28.0f, target.tiltDeg);
}

void test_image_offsets_are_relative_to_configured_default() {
  GarmentAimTarget target = calculateGarmentAimTarget(
    0.75f, 0.75f, GarmentAimConfig{10.0f, 30.0f, 60.0f, 40.0f}
  );
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, target.panDeg);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, target.tiltDeg);
}

void test_normalized_coordinates_are_clamped() {
  GarmentAimTarget target = calculateGarmentAimTarget(
    2.0f, -1.0f, GarmentAimConfig{0.0f, 20.0f, 60.0f, 40.0f}
  );
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 30.0f, target.panDeg);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 40.0f, target.tiltDeg);
}

void test_calibrated_pose_requires_only_finite_pan_and_tilt() {
  TEST_ASSERT_TRUE(isValidCalibratedGarmentAimPose(12.0f, -6.0f));
  TEST_ASSERT_FALSE(isValidCalibratedGarmentAimPose(NAN, -6.0f));
  TEST_ASSERT_FALSE(isValidCalibratedGarmentAimPose(12.0f, INFINITY));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_center_uses_configured_cloth_default);
  RUN_TEST(test_image_offsets_are_relative_to_configured_default);
  RUN_TEST(test_normalized_coordinates_are_clamped);
  RUN_TEST(test_calibrated_pose_requires_only_finite_pan_and_tilt);
  return UNITY_END();
}
