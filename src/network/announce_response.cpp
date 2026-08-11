#include "network/announce_response.h"

#include <ArduinoJson.h>

AnnounceResponseStatus parseAnnounceAdded(
  const char* payload,
  size_t payloadLength,
  bool& added
) {
  added = false;
  if (payload == nullptr || payloadLength == 0) {
    return AnnounceResponseStatus::InvalidJson;
  }

  JsonDocument responseDoc;
  DeserializationError error = deserializeJson(responseDoc, payload, payloadLength);
  if (error) {
    return AnnounceResponseStatus::InvalidJson;
  }

  JsonVariantConst addedValue = responseDoc["data"]["added"];
  if (!addedValue.is<bool>()) {
    return AnnounceResponseStatus::MissingAdded;
  }

  added = addedValue.as<bool>();
  return AnnounceResponseStatus::Parsed;
}
