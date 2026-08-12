#include <unity.h>

#include "device/lamp_aim_state.h"

void setUp() {}
void tearDown() {}

static LampAimState makeState() {
  return LampAimState{
    LampAimPose{12.0f, 8.0f},
    LampAimPose{5.0f, 25.0f},
    LampAimPose{-25.0f, -10.0f},
    LampAimPose{3.0f, -28.0f},
    true, true, false, false,
  };
}

void test_garment_runtime_pose_temporarily_overrides_configured_default() {
  LampAimSelection selected = selectLampAim(makeState());
  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(LampAimSource::Garment),
    static_cast<int>(selected.source)
  );
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.0f, selected.pose.panDeg);
}

void test_configured_garment_default_is_used_without_valid_target() {
  LampAimState state = makeState();
  state.garmentTargetValid = false;
  LampAimSelection selected = selectLampAim(state);
  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(LampAimSource::DefaultGarment),
    static_cast<int>(selected.source)
  );
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f, selected.pose.panDeg);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, selected.pose.tiltDeg);
}

void test_person_tracking_has_priority_and_starts_from_configured_default() {
  LampAimState state = makeState();
  state.personTrackingActive = true;
  LampAimSelection selected = selectLampAim(state);
  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(LampAimSource::DefaultPerson),
    static_cast<int>(selected.source)
  );
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.0f, selected.pose.panDeg);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -28.0f, selected.pose.tiltDeg);
  state.personTargetValid = true;
  selected = selectLampAim(state);
  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(LampAimSource::Person),
    static_cast<int>(selected.source)
  );
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -25.0f, selected.pose.panDeg);
}

void test_stopping_person_tracking_restores_runtime_garment_without_changing_defaults() {
  LampAimState state = makeState();
  const LampAimPose defaultGarmentBefore = state.defaultGarment;
  const LampAimPose defaultPersonBefore = state.defaultPerson;
  state.personTrackingActive = true;
  state.personTargetValid = true;
  state.personTrackingActive = false;
  state.personTargetValid = false;
  LampAimSelection selected = selectLampAim(state);
  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(LampAimSource::Garment),
    static_cast<int>(selected.source)
  );
  TEST_ASSERT_FLOAT_WITHIN(0.001f, defaultGarmentBefore.panDeg, state.defaultGarment.panDeg);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, defaultPersonBefore.tiltDeg, state.defaultPerson.tiltDeg);
}

void test_same_selection_compares_only_pan_and_tilt() {
  LampAimSelection left{LampAimSource::DefaultGarment, LampAimPose{5.0f, 25.0f}};
  LampAimSelection right{LampAimSource::DefaultGarment, LampAimPose{5.02f, 25.02f}};
  TEST_ASSERT_TRUE(sameLampAimSelection(left, right));
  right.pose.panDeg = 5.2f;
  TEST_ASSERT_FALSE(sameLampAimSelection(left, right));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_garment_runtime_pose_temporarily_overrides_configured_default);
  RUN_TEST(test_configured_garment_default_is_used_without_valid_target);
  RUN_TEST(test_person_tracking_has_priority_and_starts_from_configured_default);
  RUN_TEST(test_stopping_person_tracking_restores_runtime_garment_without_changing_defaults);
  RUN_TEST(test_same_selection_compares_only_pan_and_tilt);
  return UNITY_END();
}
