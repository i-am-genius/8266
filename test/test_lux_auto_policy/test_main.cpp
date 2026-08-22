#include <unity.h>

#include <limits>

#include "device/lux_auto_policy.h"

static LuxAutoConfig config() {
  return LuxAutoConfig{
    0.35f,
    20.0f,
    75.0f,
    1,
    15,
    5,
    100,
  };
}

void setUp() {}
void tearDown() {}

void test_first_sample_starts_from_configured_base_not_stale_auto_value() {
  LuxAutoState state{0.0f, 50, false};

  state = updateLuxAutoControl(state, 0.0f, 300, 80, config());

  TEST_ASSERT_TRUE(state.initialized);
  TEST_ASSERT_EQUAL_INT(81, state.brightness);
}

void test_low_lux_cannot_raise_brightness_more_than_15_from_base() {
  LuxAutoState state{0.0f, 50, false};

  for (int i = 0; i < 20; ++i) {
    state = updateLuxAutoControl(state, 0.0f, 300, 80, config());
  }

  TEST_ASSERT_EQUAL_INT(95, state.brightness);
}

void test_high_lux_cannot_lower_brightness_more_than_15_from_base() {
  LuxAutoState state{0.0f, 50, false};

  for (int i = 0; i < 20; ++i) {
    state = updateLuxAutoControl(state, 1000.0f, 300, 80, config());
  }

  TEST_ASSERT_EQUAL_INT(65, state.brightness);
}

void test_lux_inside_deadband_does_not_change_base_brightness() {
  LuxAutoState state{0.0f, 50, false};

  state = updateLuxAutoControl(state, 310.0f, 300, 80, config());

  TEST_ASSERT_EQUAL_INT(80, state.brightness);
}

void test_filter_reduces_one_sample_spike() {
  LuxAutoState state{0.0f, 50, false};
  state = updateLuxAutoControl(state, 300.0f, 300, 80, config());

  state = updateLuxAutoControl(state, 700.0f, 300, 80, config());

  TEST_ASSERT_FLOAT_WITHIN(0.01f, 440.0f, state.filteredLux);
  TEST_ASSERT_EQUAL_INT(79, state.brightness);
}

void test_invalid_sample_preserves_previous_state() {
  LuxAutoState state{280.0f, 84, true};

  LuxAutoState next = updateLuxAutoControl(
    state,
    std::numeric_limits<float>::quiet_NaN(),
    300,
    80,
    config()
  );

  TEST_ASSERT_FLOAT_WITHIN(0.01f, 280.0f, next.filteredLux);
  TEST_ASSERT_EQUAL_INT(84, next.brightness);
  TEST_ASSERT_TRUE(next.initialized);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_first_sample_starts_from_configured_base_not_stale_auto_value);
  RUN_TEST(test_low_lux_cannot_raise_brightness_more_than_15_from_base);
  RUN_TEST(test_high_lux_cannot_lower_brightness_more_than_15_from_base);
  RUN_TEST(test_lux_inside_deadband_does_not_change_base_brightness);
  RUN_TEST(test_filter_reduces_one_sample_spike);
  RUN_TEST(test_invalid_sample_preserves_previous_state);
  return UNITY_END();
}
