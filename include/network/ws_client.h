#pragma once
#include "app_config.h"

void sendWsRegister();
void sendWsPing();
void handleWsHeartbeat();
void handleWsMessage(const String& text);
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length);
void beginWebSocketClient();
void sendLampClothState(const char* clothState, bool tracking, const char* lastTakenAt = "");
void sendLampProximityState(bool nearby);
// Reports the speed command that this ESP8266 has applied and forwarded.
// This is not a Nano feedback/measurement.
void sendArmSpeedStatus(const char* reason = "applied");
bool isCollisionParkActive();

// Runtime aim arbitration. Camera HTTP updates are treated as live person
// targets only while the backend has opened a person-tracking session.
void startPersonTrackingAim();
bool updatePersonTrackingAim(
  bool hasPan,
  float pan,
  bool hasTilt,
  float tilt
);
void stopPersonTrackingAim();
bool isPersonTrackingAimActive();
void getDefaultGarmentAim(float& pan, float& tilt);
void getDefaultPersonAim(float& pan, float& tilt);
