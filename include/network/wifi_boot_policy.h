#pragma once

#include <cstdint>

enum class WiFiAttemptStatus : uint8_t {
  Connecting,
  Connected,
  NoSsid,
  ConnectFailed,
  Disconnected,
};

bool shouldFinishWiFiAttempt(
  WiFiAttemptStatus status,
  uint32_t elapsedMs,
  uint32_t timeoutMs
);
