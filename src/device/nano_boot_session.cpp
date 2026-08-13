#include "device/nano_boot_session.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

constexpr float kTiltCommandRatio = 30.0f / 48.0f;

float clampFloat(float value, float minimum, float maximum) {
  if (value < minimum) return minimum;
  if (value > maximum) return maximum;
  return value;
}

bool matches(const char* value, const char* expected) {
  return value && std::strcmp(value, expected) == 0;
}

bool tokenValue(
  const char* line,
  const char* key,
  const char*& valueStart,
  size_t& valueLength
) {
  const size_t keyLength = std::strlen(key);
  const char* cursor = line;
  while (*cursor != '\0') {
    while (*cursor == ' ') ++cursor;
    if (*cursor == '\0') break;

    const char* tokenEnd = std::strchr(cursor, ' ');
    if (!tokenEnd) tokenEnd = cursor + std::strlen(cursor);
    const size_t tokenLength = static_cast<size_t>(tokenEnd - cursor);
    if (tokenLength > keyLength + 1
        && std::strncmp(cursor, key, keyLength) == 0
        && cursor[keyLength] == '=') {
      valueStart = cursor + keyLength + 1;
      valueLength = tokenLength - keyLength - 1;
      return valueLength > 0;
    }
    cursor = tokenEnd;
  }
  return false;
}

bool parseBooleanField(const char* line, const char* key, bool& value) {
  const char* start = nullptr;
  size_t length = 0;
  if (!tokenValue(line, key, start, length) || length != 1) return false;
  if (*start == '0') {
    value = false;
    return true;
  }
  if (*start == '1') {
    value = true;
    return true;
  }
  return false;
}

bool parseFloatField(const char* line, const char* key, float& value) {
  const char* start = nullptr;
  size_t length = 0;
  if (!tokenValue(line, key, start, length) || length >= 24) return false;

  char buffer[24];
  std::memcpy(buffer, start, length);
  buffer[length] = '\0';
  char* end = nullptr;
  const float parsed = std::strtof(buffer, &end);
  if (!end || *end != '\0' || !std::isfinite(parsed)) return false;
  value = parsed;
  return true;
}

}  // namespace

void NanoBootSession::resetNanoSession() {
  readySeen_ = false;
  bootRequested_ = false;
  bootAccepted_ = false;
  bootRejected_ = false;
  finishSent_ = false;
}

bool NanoBootSession::noteInitializationLine(const char* line) {
  if (!matches(line, "OK step mode=16")) {
    return false;
  }
  resetNanoSession();
  return true;
}

bool NanoBootSession::onReadyLine(const char* line) {
  if (!matches(line, "READY zero=0")) {
    return false;
  }
  readySeen_ = true;
  return true;
}

NanoBootReply NanoBootSession::onBootReply(const char* line) {
  if (matches(line, "OK B state=OPENING")) {
    bootRequested_ = true;
    bootAccepted_ = true;
    bootRejected_ = false;
    return NanoBootReply::Opening;
  }
  if (matches(line, "OK B active")) {
    bootRequested_ = true;
    bootAccepted_ = true;
    bootRejected_ = false;
    return NanoBootReply::AlreadyActive;
  }
  if (matches(line, "ERR B not at power-on reference")) {
    bootRequested_ = true;
    bootAccepted_ = false;
    bootRejected_ = true;
    return NanoBootReply::RejectedReference;
  }
  if (line && std::strncmp(line, "ERR B ", 6) == 0) {
    bootRequested_ = true;
    bootAccepted_ = false;
    bootRejected_ = true;
    return NanoBootReply::RejectedOther;
  }
  return NanoBootReply::NotBootReply;
}

void NanoBootSession::noteNanoBootState(const char* state) {
  if (matches(state, "OPENING") || matches(state, "WAIT_LOOP")) {
    bootRequested_ = true;
    bootAccepted_ = true;
    bootRejected_ = false;
    return;
  }
  if (matches(state, "FINISHING") || matches(state, "DONE")) {
    bootRequested_ = true;
    bootAccepted_ = true;
    bootRejected_ = false;
    finishSent_ = true;
  }
}

void NanoBootSession::markStartupAimReady() {
  startupAimReady_ = true;
}

void NanoBootSession::beginOtaCancel() {
  otaCancelled_ = true;
  finishSent_ = true;
  cancelRequested_ = true;
  cancelSent_ = false;
}

void NanoBootSession::resumeAfterOtaFailure() {
  otaCancelled_ = false;
  cancelRequested_ = false;
  cancelSent_ = false;
  finishSent_ = false;
}

NanoBootAction NanoBootSession::pendingAction() const {
  if (cancelRequested_ && !cancelSent_) {
    return NanoBootAction::SendCancel;
  }
  if (otaCancelled_) {
    return NanoBootAction::None;
  }
  if (readySeen_ && !bootRequested_ && !bootRejected_) {
    return NanoBootAction::SendBoot;
  }
  if (startupAimReady_ && bootAccepted_ && !finishSent_) {
    return NanoBootAction::SendFinish;
  }
  return NanoBootAction::None;
}

void NanoBootSession::markActionSent(NanoBootAction action) {
  switch (action) {
    case NanoBootAction::SendBoot:
      bootRequested_ = true;
      break;
    case NanoBootAction::SendFinish:
      finishSent_ = true;
      break;
    case NanoBootAction::SendCancel:
      cancelSent_ = true;
      break;
    case NanoBootAction::None:
      break;
  }
}

bool NanoBootSession::readySeen() const {
  return readySeen_;
}

bool NanoBootSession::bootRequested() const {
  return bootRequested_;
}

bool NanoBootSession::bootAccepted() const {
  return bootAccepted_;
}

bool NanoBootSession::bootRejected() const {
  return bootRejected_;
}

bool NanoBootSession::finishSent() const {
  return finishSent_;
}

bool NanoBootSession::startupAimReady() const {
  return startupAimReady_;
}

bool NanoStatusProbe::begin(uint32_t nowMs) {
  if (result_ != NanoProbeResult::Idle) {
    return false;
  }
  result_ = NanoProbeResult::Pending;
  startedAtMs_ = nowMs;
  return true;
}

void NanoStatusProbe::succeed() {
  if (result_ == NanoProbeResult::Pending) {
    result_ = NanoProbeResult::Ok;
  }
}

void NanoStatusProbe::reset() {
  result_ = NanoProbeResult::Idle;
  startedAtMs_ = 0;
}

bool NanoStatusProbe::updateTimeout(uint32_t nowMs, uint32_t timeoutMs) {
  if (result_ != NanoProbeResult::Pending) {
    return false;
  }
  if (nowMs - startedAtMs_ < timeoutMs) {
    return false;
  }
  result_ = NanoProbeResult::TimedOut;
  return true;
}

NanoProbeResult NanoStatusProbe::result() const {
  return result_;
}

float nanoBootPanCommand(float garmentPanDeg) {
  return clampFloat(garmentPanDeg, -30.0f, 30.0f);
}

float nanoBootTiltCommand(float garmentTiltDeg) {
  const float calibrated = clampFloat(garmentTiltDeg, -90.0f, 90.0f)
    * kTiltCommandRatio;
  return clampFloat(calibrated, -30.0f, 30.0f);
}

bool parseNanoStatusLine(const char* line, NanoStatusSnapshot& status) {
  status = NanoStatusSnapshot{};
  if (!line || std::strncmp(line, "NANO_OK", 7) != 0
      || (line[7] != '\0' && line[7] != ' ')) {
    return false;
  }

  status.hasEnabled = parseBooleanField(line, "en", status.enabled);
  status.hasMoving = parseBooleanField(line, "moving", status.moving);
  status.hasPan = parseFloatField(line, "pan", status.pan);
  status.hasTilt = parseFloatField(line, "tilt", status.tilt);
  status.hasTimeout = parseBooleanField(line, "timeout", status.timedOut);

  const char* bootStart = nullptr;
  size_t bootLength = 0;
  if (tokenValue(line, "boot", bootStart, bootLength)
      && bootLength < sizeof(status.boot)) {
    std::memcpy(status.boot, bootStart, bootLength);
    status.boot[bootLength] = '\0';
    status.hasBoot = true;
  }
  return true;
}
