#pragma once

#include <cstdint>

enum class NanoBootAction {
  None,
  SendBoot,
  SendFinish,
  SendCancel,
};

enum class NanoBootReply {
  NotBootReply,
  Opening,
  AlreadyActive,
  RejectedReference,
  RejectedOther,
};

enum class NanoProbeResult {
  Idle,
  Pending,
  Ok,
  TimedOut,
};

struct NanoStatusSnapshot {
  bool hasEnabled;
  bool enabled;
  bool hasMoving;
  bool moving;
  bool hasBoot;
  char boot[16];
  bool hasPan;
  float pan;
  bool hasTilt;
  float tilt;
  bool hasTimeout;
  bool timedOut;
};

class NanoBootSession {
 public:
  bool noteInitializationLine(const char* line);
  bool onReadyLine(const char* line);
  NanoBootReply onBootReply(const char* line);
  void noteNanoBootState(const char* state);
  bool recoverReadyFromStatus(
    const NanoStatusSnapshot& status,
    float referenceToleranceDeg
  );
  void markStartupAimReady();
  void beginOtaCancel();
  void resumeAfterOtaFailure();

  NanoBootAction pendingAction() const;
  void markActionSent(NanoBootAction action);

  bool readySeen() const;
  bool bootRequested() const;
  bool bootAccepted() const;
  bool bootRejected() const;
  bool finishSent() const;
  bool startupAimReady() const;

 private:
  void resetNanoSession();

  bool readySeen_ = false;
  bool bootRequested_ = false;
  bool bootAccepted_ = false;
  bool bootRejected_ = false;
  bool finishSent_ = false;
  bool startupAimReady_ = false;
  bool otaCancelled_ = false;
  bool cancelRequested_ = false;
  bool cancelSent_ = false;
};

class NanoStatusProbe {
 public:
  bool begin(uint32_t nowMs);
  bool takeSendRequest(
    uint32_t nowMs,
    uint32_t retryIntervalMs,
    uint8_t maxAttempts
  );
  void succeed();
  void reset();
  bool updateTimeout(uint32_t nowMs, uint32_t timeoutMs);
  NanoProbeResult result() const;
  uint8_t sendAttempts() const;

 private:
  NanoProbeResult result_ = NanoProbeResult::Idle;
  uint32_t startedAtMs_ = 0;
  uint32_t lastSentAtMs_ = 0;
  uint8_t sendAttempts_ = 0;
};

float nanoBootPanCommand(float garmentPanDeg);
float nanoBootTiltCommand(float garmentTiltDeg);
bool parseNanoStatusLine(const char* line, NanoStatusSnapshot& status);
