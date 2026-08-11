#include <unity.h>

#include "device/garment_aim.h"

void setUp() {}
void tearDown() {}

void test_center_uses_default_cloth_preset() {
  GarmentAimTarget target = calculateGarmentAimTarget(
    0.5f,
    0.5f,
    GarmentAimConfig{0.0f, 20.0f, 60.0f, 45.0f}
  );

  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, target.panDeg);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, target.tiltDeg);
}

void test_image_offsets_follow_pan_and_tilt_signs() {
  GarmentAimTarget target = calculateGarmentAimTarget(
    0.75f,
    0.75f,
    GarmentAimConfig{0.0f, 20.0f, 60.0f, 40.0f}
  );

  TEST_ASSERT_FLOAT_WITHIN(0.001f, 15.0f, target.panDeg);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.0f, target.tiltDeg);
}

void test_normalized_coordinates_are_clamped() {
  GarmentAimTarget target = calculateGarmentAimTarget(
    2.0f,
    -1.0f,
    GarmentAimConfig{0.0f, 20.0f, 60.0f, 40.0f}
  );

  TEST_ASSERT_FLOAT_WITHIN(0.001f, 30.0f, target.panDeg);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 40.0f, target.tiltDeg);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_center_uses_default_cloth_preset);
  RUN_TEST(test_image_offsets_follow_pan_and_tilt_signs);
  RUN_TEST(test_normalized_coordinates_are_clamped);
  return UNITY_END();
}
