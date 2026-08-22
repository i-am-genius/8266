#include "network/wifi_boot_policy.h"

namespace {

constexpr uint32_t kNoSsidGraceMs = 5000;
constexpr uint32_t kConnectFailureGraceMs = 1500;

}  // namespace

bool shouldFinishWiFiAttempt(
  WiFiAttemptStatus status,
  uint32_t elapsedMs,
  uint32_t timeoutMs
) {
  if (status == WiFiAttemptStatus::Connected || elapsedMs >= timeoutMs) {
    return true;
  }

  if (status == WiFiAttemptStatus::NoSsid) {
    // Mobile hotspots can take a few seconds to become visible after waking.
    return elapsedMs >= kNoSsidGraceMs;
  }
  if (status == WiFiAttemptStatus::ConnectFailed) {
    return elapsedMs >= kConnectFailureGraceMs;
  }
  return false;
}
