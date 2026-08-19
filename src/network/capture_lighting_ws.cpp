#include "network/capture_lighting_ws.h"
#include "network/ws_client.h"
#include "device/light_control.h"
#include "diagnostics/diagnostic_logger.h"

static bool handleCaptureLightingMessage(const String& text) {
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, text);
  if (err) {
    return false;
  }

  JsonObject root = doc.as<JsonObject>();
  JsonObject payload = root;
  if (root["payload"].is<JsonObject>()) {
    payload = root["payload"].as<JsonObject>();
  } else if (root["data"].is<JsonObject>()) {
    payload = root["data"].as<JsonObject>();
  }

  String messageType = root["type"] | "";
  if (messageType.length() == 0) {
    messageType = payload["type"] | "";
  }
  if (messageType != "captureLighting") {
    return false;
  }

  String targetId = payload["id"] | "";
  String chipId = payload["chipId"] | "";
  if (targetId != deviceId && chipId != deviceId) {
    DEBUG_SERIAL.println("[CAPTURE_LIGHT] command is for another device, ignored");
    return true;
  }

  const bool active = payload["active"] | false;
  if (!active) {
    stopCaptureLightingOverride();
    return true;
  }

  const int br = payload["brightness"] | 80;
  const int tp = payload["temp"] | 4000;
  const unsigned long ttlMs = payload["ttlMs"] | 15000UL;
  startCaptureLightingOverride(br, tp, ttlMs);
  return true;
}

static void captureAwareWebSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_TEXT && payload != nullptr) {
    String text((char*)payload);
    if (handleCaptureLightingMessage(text)) {
      return;
    }
  }
  webSocketEvent(type, payload, length);
}

void installCaptureLightingWsInterceptor() {
  webSocket.onEvent(captureAwareWebSocketEvent);
}
