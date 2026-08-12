#include <unity.h>

#include "device/lamp_aim_state.h"

void setUp() {}
void tearDown() {}

static LampAimState makeState() {
  return LampAimState{
    LampAimPose{12.0f, 8.0f, 180.0f},
    LampAimPose{0.0f, 20.0f, 0.0f},
    LampAimPose{-25.0f, -10.0f, 420.0f},
    LampAimPose{0.0f, -30.0f, 0.0f},
    true,
    true,
    false,
    false,
  };
}

void test_garment_runtime_pose_temporarily_overrides_default_garment_pose() {
  LampAimSelection selected = selectLampAim(makeState());

  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(LampAimSource::Garment),
    static_cast<int>(selected.source)
  );
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.0f, selected.pose.panDeg);
}

void test_default_garment_pose_is_used_without_valid_garment_target() {
  LampAimState state = makeState();
  state.garmentTargetValid = false;

  LampAimSelection selected = selectLampAim(state);

  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(LampAimSource::DefaultGarment),
    static_cast<int>(selected.source)
  );
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, selected.pose.tiltDeg);
}

void test_person_tracking_has_priority_and_starts_from_default_person_pose() {
  LampAimState state = makeState();
  state.personTrackingActive = true;

  LampAimSelection selected = selectLampAim(state);

  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(LampAimSource::DefaultPerson),
    static_cast<int>(selected.source)
  );
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -30.0f, selected.pose.tiltDeg);

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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_garment_runtime_pose_temporarily_overrides_default_garment_pose);
  RUN_TEST(test_default_garment_pose_is_used_without_valid_garment_target);
  RUN_TEST(test_person_tracking_has_priority_and_starts_from_default_person_pose);
  RUN_TEST(test_stopping_person_tracking_restores_runtime_garment_without_changing_defaults);
  return UNITY_END();
}
