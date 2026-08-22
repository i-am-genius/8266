#pragma once

#include <Arduino.h>

bool armTrackingUdpSession(
  const String& sessionId,
  const String& camChipId,
  const String& camIp,
  uint16_t udpPort
);
void stopTrackingUdpSession(const char* reason, bool reportStatus = true);
void handleTrackingUdp();
bool isTrackingUdpArmed();
bool trackingUdpSessionMatches(const String& sessionId);
