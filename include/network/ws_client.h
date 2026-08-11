#pragma once
#include "app_config.h"

void sendWsRegister();
void sendWsPing();
void handleWsHeartbeat();
void handleWsMessage(const String& text);
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length);
void beginWebSocketClient();
void sendLampClothState(const char* clothState, bool tracking, const char* lastTakenAt = "");
