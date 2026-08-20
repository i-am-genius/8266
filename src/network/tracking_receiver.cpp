#include "network/tracking_receiver.h"

#include "app_config.h"
#include "device/arm_controller.h"
#include "network/ws_client.h"

namespace {

static const uint8_t TRACKING_PROTOCOL_VERSION = 1;
static const uint16_t TRACKING_PACKET_MAX_BYTES = 512;
static const unsigned long TRACKING_WATCHDOG_MS = 600;

struct TrackingUdpState {
  bool armed = false;
  bool trackingReported = false;
  bool hasSeq = false;
  String sessionId;
  String camChipId;
  IPAddress camIp;
  uint16_t port = 0;
  uint32_t lastSeq = 0;
  unsigned long armedAt = 0;
  unsigned long lastValidAt = 0;
};

WiFiUDP trackingUdp;
TrackingUdpState state;

bool sameChipId(const String& left, const String& right) {
  return left.equalsIgnoreCase(right);
}

void reportTrackingStatus(
  const char* status,
  const char* message,
  uint32_t sequence = 0,
  bool includeSequence = false
) {
  if (!wsConnected) {
    return;
  }

  StaticJsonDocument<384> doc;
  doc["type"] = "trackingStatus";
  doc["chipId"] = deviceId;
  doc["role"] = "lamp";
  doc["trackingStatus"] = status ? status : "unknown";
  doc["camChipId"] = state.camChipId;
  doc["lampChipId"] = deviceId;
  doc["sessionId"] = state.sessionId;
  if (includeSequence) {
    doc["sequence"] = sequence;
  }
  if (message && *message) {
    doc["message"] = message;
  }

  String payload;
  serializeJson(doc, payload);
  webSocket.sendTXT(payload);
  DEBUG_SERIAL.println("[TRACK_UDP] status: " + payload);
}

void clearState() {
  state = TrackingUdpState{};
}

void terminateSession(const char* status, const char* message) {
  if (!state.armed) {
    return;
  }

  stopPersonTrackingAim();
  reportTrackingStatus(status, message, state.lastSeq, state.hasSeq);
  trackingUdp.stop();
  clearState();
}

bool sequenceIsNewer(uint32_t incoming, uint32_t previous) {
  return static_cast<int32_t>(incoming - previous) > 0;
}

bool validatePose(float pan, float tilt) {
  if (!isfinite(pan) || !isfinite(tilt)) {
    return false;
  }
  return fabsf(clampPanTargetDeg(pan) - pan) <= 0.001f
      && fabsf(clampTiltTargetDeg(tilt) - tilt) <= 0.001f;
}

void discardPacket(int packetSize) {
  while (packetSize-- > 0 && trackingUdp.available()) {
    trackingUdp.read();
  }
}

}  // namespace

bool armTrackingUdpSession(
  const String& sessionId,
  const String& camChipId,
  const String& camIp,
  uint16_t udpPort
) {
  String normalizedSession = sessionId;
  String normalizedCam = camChipId;
  String normalizedIp = camIp;
  normalizedSession.trim();
  normalizedCam.trim();
  normalizedIp.trim();

  IPAddress parsedCamIp;
  if (normalizedSession.length() == 0 || normalizedSession.length() > 80
      || normalizedCam.length() == 0 || normalizedCam.length() > 64
      || !parsedCamIp.fromString(normalizedIp)
      || udpPort == 0) {
    DEBUG_SERIAL.println("[TRACK_UDP] reject invalid session parameters");
    return false;
  }

  if (state.armed) {
    stopTrackingUdpSession("tracking session replaced", false);
  }

  trackingUdp.stop();
  if (!trackingUdp.begin(udpPort)) {
    DEBUG_SERIAL.printf("[TRACK_UDP] failed to bind UDP port %u\n", udpPort);
    return false;
  }

  state.armed = true;
  state.sessionId = normalizedSession;
  state.camChipId = normalizedCam;
  state.camIp = parsedCamIp;
  state.port = udpPort;
  state.armedAt = millis();
  state.lastValidAt = 0;
  state.hasSeq = false;
  state.trackingReported = false;

  // Arm only. Do not move to the person preset until the first valid UDP pose.
  startPersonTrackingAim();
  reportTrackingStatus("armed", "tracking UDP receiver armed");
  DEBUG_SERIAL.printf(
    "[TRACK_UDP] armed session=%s cam=%s ip=%s port=%u\n",
    state.sessionId.c_str(),
    state.camChipId.c_str(),
    state.camIp.toString().c_str(),
    state.port
  );
  return true;
}

void stopTrackingUdpSession(const char* reason, bool reportStatus) {
  if (!state.armed) {
    stopPersonTrackingAim();
    return;
  }

  stopPersonTrackingAim();
  if (reportStatus) {
    reportTrackingStatus("stopped", reason ? reason : "tracking stopped", state.lastSeq, state.hasSeq);
  }
  trackingUdp.stop();
  clearState();
}

bool isTrackingUdpArmed() {
  return state.armed;
}

bool trackingUdpSessionMatches(const String& sessionId) {
  if (!state.armed) {
    return false;
  }
  String normalized = sessionId;
  normalized.trim();
  return normalized.length() > 0 && normalized == state.sessionId;
}

void handleTrackingUdp() {
  if (!state.armed) {
    return;
  }

  bool haveLatestPose = false;
  float latestPan = 0.0f;
  float latestTilt = 0.0f;
  uint32_t latestSeq = 0;

  int packetSize = trackingUdp.parsePacket();
  while (packetSize > 0) {
    const IPAddress remoteIp = trackingUdp.remoteIP();

    if (packetSize > TRACKING_PACKET_MAX_BYTES) {
      discardPacket(packetSize);
      packetSize = trackingUdp.parsePacket();
      continue;
    }

    char buffer[TRACKING_PACKET_MAX_BYTES + 1];
    int readLength = trackingUdp.read(
      reinterpret_cast<uint8_t*>(buffer),
      min(packetSize, static_cast<int>(TRACKING_PACKET_MAX_BYTES))
    );
    if (readLength <= 0) {
      packetSize = trackingUdp.parsePacket();
      continue;
    }
    buffer[readLength] = '\0';

    if (remoteIp != state.camIp) {
      packetSize = trackingUdp.parsePacket();
      continue;
    }

    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, buffer) != DeserializationError::Ok) {
      packetSize = trackingUdp.parsePacket();
      continue;
    }

    const int protocolVersion = doc["protocolVersion"] | 0;
    const String type = doc["type"] | "";
    const String sessionId = doc["sessionId"] | "";
    const String sourceChipId = doc["sourceChipId"] | "";
    const String targetChipId = doc["targetChipId"] | "";

    if (protocolVersion != TRACKING_PROTOCOL_VERSION
        || type != "lampTrackingUpdate"
        || sessionId != state.sessionId
        || !sameChipId(sourceChipId, state.camChipId)
        || !sameChipId(targetChipId, deviceId)
        || !doc.containsKey("seq")
        || !doc.containsKey("valid")) {
      packetSize = trackingUdp.parsePacket();
      continue;
    }

    const uint32_t seq = doc["seq"].as<uint32_t>();
    if (state.hasSeq && !sequenceIsNewer(seq, state.lastSeq)) {
      packetSize = trackingUdp.parsePacket();
      continue;
    }

    if (!doc["valid"].as<bool>()) {
      state.lastSeq = seq;
      state.hasSeq = true;
      terminateSession("lost", "camera reported invalid tracking geometry");
      return;
    }

    if (!doc.containsKey("pan") || !doc.containsKey("tilt")) {
      packetSize = trackingUdp.parsePacket();
      continue;
    }

    const float pan = doc["pan"].as<float>();
    const float tilt = doc["tilt"].as<float>();
    if (!validatePose(pan, tilt)) {
      packetSize = trackingUdp.parsePacket();
      continue;
    }

    state.lastSeq = seq;
    state.hasSeq = true;
    state.lastValidAt = millis();
    latestPan = pan;
    latestTilt = tilt;
    latestSeq = seq;
    haveLatestPose = true;

    packetSize = trackingUdp.parsePacket();
  }

  if (haveLatestPose) {
    if (updatePersonTrackingAim(true, latestPan, true, latestTilt)) {
      if (!state.trackingReported) {
        state.trackingReported = true;
        reportTrackingStatus("tracking", "first valid UDP pose applied", latestSeq, true);
      }
    }
  }

  const unsigned long now = millis();
  const unsigned long watchdogBase = state.lastValidAt > 0 ? state.lastValidAt : state.armedAt;
  if (static_cast<unsigned long>(now - watchdogBase) >= TRACKING_WATCHDOG_MS) {
    terminateSession("timeout", "tracking UDP watchdog timeout");
  }
}
