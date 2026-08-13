#include <unity.h>

#include "device/nano_boot_session.h"

void setUp() {}
void tearDown() {}

void test_ready_line_never_requests_boot_from_8266() {
  NanoBootSession session;

  TEST_ASSERT_FALSE(session.onReadyLine("OK step mode=16"));
  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(NanoBootAction::None),
    static_cast<int>(session.pendingAction())
  );
  TEST_ASSERT_FALSE(session.onReadyLine("READY zero=1"));
  TEST_ASSERT_TRUE(session.onReadyLine("READY zero=0"));
  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(NanoBootAction::None),
    static_cast<int>(session.pendingAction())
  );
}

void test_nano_initialization_rearms_finish_for_autonomous_animation() {
  NanoBootSession session;
  session.markStartupAimReady();
  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(NanoBootAction::SendFinish),
    static_cast<int>(session.pendingAction())
  );
  session.markActionSent(NanoBootAction::SendFinish);

  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(NanoBootAction::None),
    static_cast<int>(session.pendingAction())
  );

  session.noteInitializationLine("OK step mode=16");
  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(NanoBootAction::SendFinish),
    static_cast<int>(session.pendingAction())
  );
}

void test_finish_only_waits_for_server_default_aim() {
  NanoBootSession session;
  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(NanoBootAction::None),
    static_cast<int>(session.pendingAction())
  );
  session.markStartupAimReady();
  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(NanoBootAction::SendFinish),
    static_cast<int>(session.pendingAction())
  );
  session.markActionSent(NanoBootAction::SendFinish);
  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(NanoBootAction::None),
    static_cast<int>(session.pendingAction())
  );
}

void test_stale_boot_error_does_not_block_finish() {
  NanoBootSession session;
  session.markStartupAimReady();

  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(NanoBootReply::RejectedReference),
    static_cast<int>(session.onBootReply("ERR B not at power-on reference"))
  );
  TEST_ASSERT_TRUE(session.bootRejected());
  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(NanoBootAction::SendFinish),
    static_cast<int>(session.pendingAction())
  );
}

void test_finish_uses_existing_tilt_calibration_and_nano_bounds() {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, nanoBootPanCommand(0.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.5f, nanoBootTiltCommand(20.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 30.0f, nanoBootPanCommand(60.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -30.0f, nanoBootTiltCommand(-90.0f));
}

void test_ota_cancel_is_once_only_and_suppresses_late_finish() {
  NanoBootSession session;
  session.markStartupAimReady();

  session.beginOtaCancel();
  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(NanoBootAction::SendCancel),
    static_cast<int>(session.pendingAction())
  );
  session.markActionSent(NanoBootAction::SendCancel);
  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(NanoBootAction::None),
    static_cast<int>(session.pendingAction())
  );

  session.markStartupAimReady();
  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(NanoBootAction::None),
    static_cast<int>(session.pendingAction())
  );

  session.resumeAfterOtaFailure();
  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(NanoBootAction::SendFinish),
    static_cast<int>(session.pendingAction())
  );
}

void test_q_boot_state_recovers_warm_esp_restart() {
  NanoBootSession session;
  session.markStartupAimReady();
  session.noteNanoBootState("OPENING");

  TEST_ASSERT_TRUE(session.bootAccepted());
  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(NanoBootAction::SendFinish),
    static_cast<int>(session.pendingAction())
  );
}

void test_q_idle_reference_recovers_missed_ready() {
  NanoBootSession session;
  NanoStatusSnapshot status{};

  TEST_ASSERT_TRUE(parseNanoStatusLine(
    "NANO_OK v=2 en=1 moving=0 slider_steps=0 boot=IDLE "
    "pan=0.10 tilt=-0.20 timeout=0",
    status
  ));
  TEST_ASSERT_TRUE(session.recoverReadyFromStatus(status, 0.25f));
  TEST_ASSERT_TRUE(session.readySeen());
  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(NanoBootAction::None),
    static_cast<int>(session.pendingAction())
  );
}

void test_q_idle_recovery_rejects_motion_or_non_reference_pose() {
  NanoBootSession movingSession;
  NanoStatusSnapshot moving{};
  TEST_ASSERT_TRUE(parseNanoStatusLine(
    "NANO_OK v=2 en=1 moving=1 slider_steps=0 boot=IDLE "
    "pan=0.00 tilt=0.00 timeout=0",
    moving
  ));
  TEST_ASSERT_FALSE(movingSession.recoverReadyFromStatus(moving, 0.25f));

  NanoBootSession displacedSession;
  NanoStatusSnapshot displaced{};
  TEST_ASSERT_TRUE(parseNanoStatusLine(
    "NANO_OK v=2 en=1 moving=0 slider_steps=0 boot=IDLE "
    "pan=0.26 tilt=0.00 timeout=0",
    displaced
  ));
  TEST_ASSERT_FALSE(displacedSession.recoverReadyFromStatus(displaced, 0.25f));
}

void test_q_status_fields_are_parsed_by_name_in_any_order() {
  NanoStatusSnapshot status{};

  TEST_ASSERT_TRUE(parseNanoStatusLine(
    "NANO_OK timeout=0 tilt=8.40 boot=OPENING slider_steps=0 "
    "moving=1 en=1 pan=1.25 v=2",
    status
  ));
  TEST_ASSERT_TRUE(status.hasEnabled);
  TEST_ASSERT_TRUE(status.enabled);
  TEST_ASSERT_TRUE(status.hasMoving);
  TEST_ASSERT_TRUE(status.moving);
  TEST_ASSERT_TRUE(status.hasBoot);
  TEST_ASSERT_EQUAL_STRING("OPENING", status.boot);
  TEST_ASSERT_TRUE(status.hasPan);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.25f, status.pan);
  TEST_ASSERT_TRUE(status.hasTilt);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 8.40f, status.tilt);
  TEST_ASSERT_TRUE(status.hasTimeout);
  TEST_ASSERT_FALSE(status.timedOut);
}

void test_legacy_q_status_remains_valid_without_new_fields() {
  NanoStatusSnapshot status{};

  TEST_ASSERT_TRUE(parseNanoStatusLine(
    "NANO_OK v=1 en=1 moving=0 slider_steps=42",
    status
  ));
  TEST_ASSERT_TRUE(status.hasEnabled);
  TEST_ASSERT_FALSE(status.hasBoot);
  TEST_ASSERT_FALSE(status.hasPan);
  TEST_ASSERT_FALSE(status.hasTilt);
  TEST_ASSERT_FALSE(status.hasTimeout);
  TEST_ASSERT_FALSE(parseNanoStatusLine("OK B state=OPENING", status));
}

void test_shared_status_probe_can_only_timeout_once() {
  NanoStatusProbe probe;

  TEST_ASSERT_TRUE(probe.begin(1000));
  TEST_ASSERT_FALSE(probe.begin(1100));
  TEST_ASSERT_FALSE(probe.updateTimeout(2999, 2000));
  TEST_ASSERT_TRUE(probe.updateTimeout(3000, 2000));
  TEST_ASSERT_FALSE(probe.updateTimeout(5000, 2000));
  TEST_ASSERT_FALSE(probe.begin(5001));
  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(NanoProbeResult::TimedOut),
    static_cast<int>(probe.result())
  );
}

void test_shared_status_probe_retries_q_without_duplicate_timeout() {
  NanoStatusProbe probe;

  TEST_ASSERT_TRUE(probe.begin(1000));
  TEST_ASSERT_TRUE(probe.takeSendRequest(1000, 300, 3));
  TEST_ASSERT_FALSE(probe.takeSendRequest(1299, 300, 3));
  TEST_ASSERT_TRUE(probe.takeSendRequest(1300, 300, 3));
  TEST_ASSERT_TRUE(probe.takeSendRequest(1600, 300, 3));
  TEST_ASSERT_FALSE(probe.takeSendRequest(1900, 300, 3));
  TEST_ASSERT_FALSE(probe.updateTimeout(2999, 2000));
  TEST_ASSERT_TRUE(probe.updateTimeout(3000, 2000));
  TEST_ASSERT_FALSE(probe.updateTimeout(5000, 2000));
  TEST_ASSERT_FALSE(probe.takeSendRequest(5000, 300, 3));
  TEST_ASSERT_EQUAL_UINT8(3, probe.sendAttempts());
}

void test_fresh_ready_invalidates_stale_probe_timeout() {
  NanoBootSession session;
  NanoStatusProbe probe;

  TEST_ASSERT_TRUE(probe.begin(1000));
  TEST_ASSERT_TRUE(probe.takeSendRequest(1000, 300, 3));
  TEST_ASSERT_TRUE(probe.updateTimeout(3000, 2000));

  TEST_ASSERT_FALSE(acceptNanoReadyLine(session, probe, "READY zero=1"));
  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(NanoProbeResult::TimedOut),
    static_cast<int>(probe.result())
  );

  TEST_ASSERT_TRUE(acceptNanoReadyLine(session, probe, "READY zero=0"));
  TEST_ASSERT_EQUAL_INT(
    static_cast<int>(NanoProbeResult::Idle),
    static_cast<int>(probe.result())
  );
  TEST_ASSERT_EQUAL_UINT8(0, probe.sendAttempts());
  TEST_ASSERT_TRUE(probe.begin(4000));
  TEST_ASSERT_TRUE(probe.takeSendRequest(4000, 300, 3));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ready_line_never_requests_boot_from_8266);
  RUN_TEST(test_nano_initialization_rearms_finish_for_autonomous_animation);
  RUN_TEST(test_finish_only_waits_for_server_default_aim);
  RUN_TEST(test_stale_boot_error_does_not_block_finish);
  RUN_TEST(test_finish_uses_existing_tilt_calibration_and_nano_bounds);
  RUN_TEST(test_ota_cancel_is_once_only_and_suppresses_late_finish);
  RUN_TEST(test_q_boot_state_recovers_warm_esp_restart);
  RUN_TEST(test_q_idle_reference_recovers_missed_ready);
  RUN_TEST(test_q_idle_recovery_rejects_motion_or_non_reference_pose);
  RUN_TEST(test_q_status_fields_are_parsed_by_name_in_any_order);
  RUN_TEST(test_legacy_q_status_remains_valid_without_new_fields);
  RUN_TEST(test_shared_status_probe_can_only_timeout_once);
  RUN_TEST(test_shared_status_probe_retries_q_without_duplicate_timeout);
  RUN_TEST(test_fresh_ready_invalidates_stale_probe_timeout);
  return UNITY_END();
}
