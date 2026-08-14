#include <unity.h>

#include "device/tof_interaction_state.h"

static TofInteractionConfig config() {
  return TofInteractionConfig{
    2000,
    500,
    600,
    500,
    800,
    1000,
  };
}

void setUp() {}
void tearDown() {}

void test_initial_far_sample_waits_before_publishing_proximity_snapshot() {
  TofInteractionState state{};

  TofInteractionUpdate started = updateTofInteraction(state, 2400, 10, config());
  TofInteractionUpdate pending = updateTofInteraction(state, 2400, 509, config());
  TofInteractionUpdate confirmed = updateTofInteraction(state, 2400, 510, config());

  TEST_ASSERT_FALSE(started.proximityChanged);
  TEST_ASSERT_FALSE(pending.proximityChanged);
  TEST_ASSERT_TRUE(confirmed.proximityChanged);
  TEST_ASSERT_FALSE(confirmed.nearby);
  TEST_ASSERT_FALSE(confirmed.clothTaken);
}

void test_valid_far_distance_requires_500ms_before_exit() {
  TofInteractionState state{};
  updateTofInteraction(state, 1999, 0, config());

  TofInteractionUpdate started = updateTofInteraction(state, 2000, 100, config());
  TofInteractionUpdate pending = updateTofInteraction(state, 2400, 599, config());
  TofInteractionUpdate confirmed = updateTofInteraction(state, 2400, 600, config());

  TEST_ASSERT_FALSE(started.proximityChanged);
  TEST_ASSERT_TRUE(started.nearby);
  TEST_ASSERT_FALSE(pending.proximityChanged);
  TEST_ASSERT_TRUE(pending.nearby);
  TEST_ASSERT_TRUE(confirmed.proximityChanged);
  TEST_ASSERT_FALSE(confirmed.nearby);
}

void test_range_status_4_is_no_target_and_exits_after_500ms() {
  TofInteractionState state{};
  updateTofInteraction(state, 1000, 0, config());
  TofSample noTarget = classifyTofSample(4, 8190, 8200);

  TEST_ASSERT_EQUAL_INT((int)TofSampleKind::NoTarget, (int)noTarget.kind);
  TEST_ASSERT_FALSE(updateTofInteraction(state, noTarget, 100, config()).proximityChanged);
  TEST_ASSERT_TRUE(updateTofInteraction(state, noTarget, 599, config()).nearby);

  TofInteractionUpdate confirmed = updateTofInteraction(state, noTarget, 600, config());
  TEST_ASSERT_TRUE(confirmed.proximityChanged);
  TEST_ASSERT_FALSE(confirmed.nearby);
}

void test_initial_no_target_publishes_not_near_after_500ms() {
  TofInteractionState state{};
  TofSample noTarget = classifyTofSample(4, 8190, 8200);

  TEST_ASSERT_FALSE(updateTofInteraction(state, noTarget, 100, config()).proximityChanged);
  TEST_ASSERT_FALSE(updateTofInteraction(state, noTarget, 599, config()).proximityChanged);
  TofInteractionUpdate confirmed = updateTofInteraction(state, noTarget, 600, config());

  TEST_ASSERT_TRUE(confirmed.proximityChanged);
  TEST_ASSERT_FALSE(confirmed.nearby);
  TEST_ASSERT_FALSE(confirmed.clothTaken);
}

void test_sensor_failure_is_not_classified_as_no_target() {
  TofSample failure = classifyTofSample(2, 8190, 8200);

  TEST_ASSERT_EQUAL_INT((int)TofSampleKind::Invalid, (int)failure.kind);
}

void test_sensor_failure_breaks_continuous_far_confirmation() {
  TofInteractionState state{};
  TofSample noTarget = classifyTofSample(4, 8190, 8200);
  TofSample failure = classifyTofSample(2, 8190, 8200);
  updateTofInteraction(state, 1000, 0, config());

  updateTofInteraction(state, noTarget, 100, config());
  updateTofInteraction(state, failure, 400, config());
  updateTofInteraction(state, noTarget, 600, config());

  TEST_ASSERT_TRUE(updateTofInteraction(state, noTarget, 1099, config()).nearby);
  TofInteractionUpdate confirmed = updateTofInteraction(state, noTarget, 1100, config());
  TEST_ASSERT_TRUE(confirmed.proximityChanged);
  TEST_ASSERT_FALSE(confirmed.nearby);
}

void test_take_requires_600mm_for_full_500ms() {
  TofInteractionState state{};

  TEST_ASSERT_FALSE(updateTofInteraction(state, 599, 100, config()).clothTaken);
  TEST_ASSERT_FALSE(updateTofInteraction(state, 599, 599, config()).clothTaken);
  TEST_ASSERT_TRUE(updateTofInteraction(state, 599, 600, config()).clothTaken);
  TEST_ASSERT_FALSE(updateTofInteraction(state, 599, 1200, config()).clothTaken);
}

void test_take_candidate_is_cancelled_when_distance_returns_to_600mm() {
  TofInteractionState state{};

  updateTofInteraction(state, 599, 100, config());
  updateTofInteraction(state, 600, 400, config());

  TEST_ASSERT_FALSE(updateTofInteraction(state, 599, 500, config()).clothTaken);
  TEST_ASSERT_FALSE(updateTofInteraction(state, 599, 999, config()).clothTaken);
  TEST_ASSERT_TRUE(updateTofInteraction(state, 599, 1000, config()).clothTaken);
}

void test_no_target_cancels_take_candidate() {
  TofInteractionState state{};
  TofSample noTarget = classifyTofSample(4, 8190, 8200);

  updateTofInteraction(state, 599, 100, config());
  updateTofInteraction(state, noTarget, 400, config());

  TEST_ASSERT_FALSE(updateTofInteraction(state, 599, 500, config()).clothTaken);
  TEST_ASSERT_FALSE(updateTofInteraction(state, 599, 999, config()).clothTaken);
  TEST_ASSERT_TRUE(updateTofInteraction(state, 599, 1000, config()).clothTaken);
}

void test_clear_disarms_until_800mm_has_remained_clear_for_1000ms() {
  TofInteractionState state{};
  updateTofInteraction(state, 500, 0, config());
  TEST_ASSERT_TRUE(updateTofInteraction(state, 500, 500, config()).clothTaken);

  clearClothTakenDetection(state);
  TEST_ASSERT_FALSE(updateTofInteraction(state, 500, 1200, config()).clothTaken);
  updateTofInteraction(state, 800, 1300, config());
  updateTofInteraction(state, 799, 1800, config());
  updateTofInteraction(state, 800, 2000, config());
  updateTofInteraction(state, 800, 2999, config());
  updateTofInteraction(state, 800, 3000, config());

  TEST_ASSERT_FALSE(updateTofInteraction(state, 500, 3100, config()).clothTaken);
  TEST_ASSERT_TRUE(updateTofInteraction(state, 500, 3600, config()).clothTaken);
}

void test_no_target_rearms_taken_detection_after_clear() {
  TofInteractionState state{};
  TofSample noTarget = classifyTofSample(4, 8190, 8200);
  updateTofInteraction(state, 500, 0, config());
  TEST_ASSERT_TRUE(updateTofInteraction(state, 500, 500, config()).clothTaken);

  clearClothTakenDetection(state);
  updateTofInteraction(state, noTarget, 1200, config());
  updateTofInteraction(state, noTarget, 2199, config());
  updateTofInteraction(state, noTarget, 2200, config());

  TEST_ASSERT_FALSE(updateTofInteraction(state, 500, 2300, config()).clothTaken);
  TEST_ASSERT_TRUE(updateTofInteraction(state, 500, 2800, config()).clothTaken);
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_initial_far_sample_waits_before_publishing_proximity_snapshot);
  RUN_TEST(test_valid_far_distance_requires_500ms_before_exit);
  RUN_TEST(test_range_status_4_is_no_target_and_exits_after_500ms);
  RUN_TEST(test_initial_no_target_publishes_not_near_after_500ms);
  RUN_TEST(test_sensor_failure_is_not_classified_as_no_target);
  RUN_TEST(test_sensor_failure_breaks_continuous_far_confirmation);
  RUN_TEST(test_take_requires_600mm_for_full_500ms);
  RUN_TEST(test_take_candidate_is_cancelled_when_distance_returns_to_600mm);
  RUN_TEST(test_no_target_cancels_take_candidate);
  RUN_TEST(test_clear_disarms_until_800mm_has_remained_clear_for_1000ms);
  RUN_TEST(test_no_target_rearms_taken_detection_after_clear);
  return UNITY_END();
}
