#include <unity.h>

#include "device/tof_interaction_state.h"

static TofInteractionConfig config() {
  return TofInteractionConfig{
    2000,
    600,
    500,
    800,
    1000,
  };
}

void setUp() {}
void tearDown() {}

void test_initial_sample_publishes_proximity_snapshot() {
  TofInteractionState state{};

  TofInteractionUpdate update = updateTofInteraction(state, 2400, 10, config());

  TEST_ASSERT_TRUE(update.proximityChanged);
  TEST_ASSERT_FALSE(update.nearby);
  TEST_ASSERT_FALSE(update.clothTaken);
}

void test_proximity_only_changes_at_2000mm_boundary() {
  TofInteractionState state{};
  updateTofInteraction(state, 2400, 0, config());

  TofInteractionUpdate near = updateTofInteraction(state, 1999, 50, config());
  TofInteractionUpdate same = updateTofInteraction(state, 1200, 100, config());
  TofInteractionUpdate far = updateTofInteraction(state, 2000, 150, config());

  TEST_ASSERT_TRUE(near.proximityChanged);
  TEST_ASSERT_TRUE(near.nearby);
  TEST_ASSERT_FALSE(same.proximityChanged);
  TEST_ASSERT_TRUE(far.proximityChanged);
  TEST_ASSERT_FALSE(far.nearby);
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

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_initial_sample_publishes_proximity_snapshot);
  RUN_TEST(test_proximity_only_changes_at_2000mm_boundary);
  RUN_TEST(test_take_requires_600mm_for_full_500ms);
  RUN_TEST(test_take_candidate_is_cancelled_when_distance_returns_to_600mm);
  RUN_TEST(test_clear_disarms_until_800mm_has_remained_clear_for_1000ms);
  return UNITY_END();
}
