#include "device/sensor_manager.h"
#include "device/light_control.h"
#include "device/lux_auto_policy.h"
#include "device/tof_interaction_state.h"
#include "network/http_reporter.h"
#include "network/ws_client.h"
#include "diagnostics/diagnostic_logger.h"

static ThresholdGate tofReadGate{};
static ThresholdGate bh1750ReadGate{};
static const uint8_t BH1750_I2C_ADDRESS = 0x23;
static const unsigned long BH1750_CONTROL_INTERVAL_MS = 150;
static const LuxAutoConfig luxAutoConfig{
  0.35f,  // EMA alpha: react quickly while filtering short spikes
  20.0f,  // target deadband in lux
  75.0f,  // lux error represented by one brightness step
  1,      // maximum brightness change per sample
  15,     // BH1750 may only trim the configured base by +/-15
  5,
  100,
};
static LuxAutoState luxAutoState{0.0f, 50, false};
static const uint16_t TOF_PERSON_NEAR_THRESHOLD_MM = 2000;
static const unsigned long TOF_PROXIMITY_EXIT_CONFIRM_MS = 500;
static const uint16_t TOF_CLOTH_TAKEN_THRESHOLD_MM = 600;
static const unsigned long TOF_CLOTH_CONFIRM_MS = 500;
static const uint16_t TOF_CLOTH_RELEASE_THRESHOLD_MM = 800;
static const unsigned long TOF_CLOTH_REARM_MS = 1000;
static const uint32_t TOF_LONG_RANGE_TIMING_BUDGET_US = 33000;
static TofInteractionState tofInteractionState{};
static bool clothTakenReportPending = false;
static const TofInteractionConfig tofInteractionConfig{
  TOF_PERSON_NEAR_THRESHOLD_MM,
  TOF_PROXIMITY_EXIT_CONFIRM_MS,
  TOF_CLOTH_TAKEN_THRESHOLD_MM,
  TOF_CLOTH_CONFIRM_MS,
  TOF_CLOTH_RELEASE_THRESHOLD_MM,
  TOF_CLOTH_REARM_MS,
};

static bool configureTofLongRange() {
  // lox.begin() first applies Adafruit's DEFAULT preset, which enables the
  // range-ignore threshold. A clean LONG_RANGE initialization does not enable
  // that check, so explicitly disable it before applying long-range limits.
  const FixPoint1616_t signalRateLimit =
    static_cast<FixPoint1616_t>(0.1f * 65536.0f);
  const FixPoint1616_t sigmaLimit =
    static_cast<FixPoint1616_t>(60.0f * 65536.0f);

  bool ok = true;
  ok = lox.setLimitCheckEnable(
         VL53L0X_CHECKENABLE_SIGNAL_RATE_FINAL_RANGE,
         1
       ) && ok;
  ok = lox.setLimitCheckEnable(
         VL53L0X_CHECKENABLE_SIGMA_FINAL_RANGE,
         1
       ) && ok;
  ok = lox.setLimitCheckEnable(
         VL53L0X_CHECKENABLE_RANGE_IGNORE_THRESHOLD,
         0
       ) && ok;
  ok = lox.setLimitCheckValue(
         VL53L0X_CHECKENABLE_SIGNAL_RATE_FINAL_RANGE,
         signalRateLimit
       ) && ok;
  ok = lox.setLimitCheckValue(
         VL53L0X_CHECKENABLE_SIGMA_FINAL_RANGE,
         sigmaLimit
       ) && ok;
  ok = lox.setMeasurementTimingBudgetMicroSeconds(
         TOF_LONG_RANGE_TIMING_BUDGET_US
       ) && ok;
  ok = lox.setVcselPulsePeriod(VL53L0X_VCSEL_PERIOD_PRE_RANGE, 18) && ok;
  ok = lox.setVcselPulsePeriod(VL53L0X_VCSEL_PERIOD_FINAL_RANGE, 14) && ok;

  DEBUG_SERIAL.printf(
    "[TOF] long range config %s, budget=%luus\n",
    ok ? "ok" : "failed",
    static_cast<unsigned long>(TOF_LONG_RANGE_TIMING_BUDGET_US)
  );
  return ok;
}

static bool i2cDeviceResponds(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

void setupHardwareAndSensors() {
  pinMode(LED_COLD_PIN, OUTPUT);
  pinMode(LED_WARM_PIN, OUTPUT);
  pinMode(BLUR, OUTPUT);
  digitalWrite(BLUR, LOW);
  analogWriteRange(1024);

  Wire.begin(TOF_SDA_PIN, TOF_SCL_PIN);
  Wire.setClock(400000);

  if (!lox.begin()) {
    DEBUG_SERIAL.println("VL53L0X 初始化失败");
    diagnosticLogSensor("VL53L0X init failed", true);
  } else {
    DEBUG_SERIAL.println("VL53L0X 初始化成功");
    const bool longRangeOk = configureTofLongRange();
    if (!longRangeOk) {
      diagnosticLogSensor("VL53L0X long range config failed", true);
    }
    tofReady = true;
  }

  // BH1750 库会把 NACK 错误直接打印到 Serial；Serial 是 Nano 协议口。
  // 先用 Wire 静默探测，设备缺失时不进入会污染串口的库调用。
  if (!i2cDeviceResponds(BH1750_I2C_ADDRESS)) {
    DEBUG_SERIAL.println("BH1750 初始化失败");
    diagnosticLogSensor("BH1750 not found", true);
  } else if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    DEBUG_SERIAL.println("BH1750 初始化成功");
    bh1750Ready = true;
  } else {
    DEBUG_SERIAL.println("BH1750 初始化失败");
    diagnosticLogSensor("BH1750 init failed", true);
  }

  udp.begin(udpPort);
}

void clearClothTakenState() {
  clearClothTakenDetection(tofInteractionState);
  clothTakenReportPending = false;
}

void sendCurrentLampProximityState() {
  if (tofInteractionState.proximityInitialized) {
    sendLampProximityState(tofInteractionState.nearby);
  }
  if (clothTakenReportPending && wsConnected) {
    sendLampClothState("taken", false);
    clothTakenReportPending = false;
  }
}

void updateLightingByToF() {
  if (!tofReady) {
    int br = autoMode ? recommendedBrightness : brightness;
    int tp = autoMode ? recommendedTemp : temp;
    unsigned long now = millis();

    // ToF 不可用时无法检测有人/无人，BLUR 一律 LOW
    digitalWrite(BLUR, LOW);

    if (now - lastLightUpdate > lightUpdateInterval) {
      applyLightSettings(br, tp);
      lastLightUpdate = now;
    }
    return;
  }

  unsigned long now = millis();
  VL53L0X_RangingMeasurementData_t measure;

  if (now - lastToFRead >= TOF_READ_INTERVAL_MS) {
    lox.rangingTest(&measure, false);
    lastToFRead = now;
  } else {
    return;
  }

  TofSample tofSample = classifyTofSample(
    measure.RangeStatus,
    measure.RangeMilliMeter,
    TOF_MAX_RANGE_MM
  );
  bool tofSampleUsable = tofSample.kind != TofSampleKind::Invalid;
  FailureDecision tofDecision = thresholdGateRecord(tofReadGate, tofSampleUsable, 20);
  if (tofDecision == DIAG_LOG_FAILURE) {
    char message[64];
    snprintf(message, sizeof(message), "ToF invalid status=%u range=%u",
             measure.RangeStatus, measure.RangeMilliMeter);
    diagnosticLogSensor(message, true);
  } else if (tofDecision == DIAG_LOG_RECOVERY) {
    diagnosticLogSensor("ToF reads recovered");
  }

  TofInteractionUpdate interaction = updateTofInteraction(
    tofInteractionState,
    tofSample,
    now,
    tofInteractionConfig
  );
  if (!tofSampleUsable) return;

  static bool wasNearby = false;
  static unsigned long transitionStart = 0;
  static unsigned long detectedStart = 0;
  static unsigned long leftStart = 0;

  bool currentNearby = interaction.nearby;

  if (tofSample.kind == TofSampleKind::ValidDistance) {
    DEBUG_SERIAL.printf("测距: %d mm\n", measure.RangeMilliMeter);
  }

  if (interaction.proximityChanged) {
    sendLampProximityState(interaction.nearby);
  }
  if (interaction.clothTaken) {
    clothTakenReportPending = true;
  }
  if (clothTakenReportPending && wsConnected) {
    sendLampClothState("taken", false);
    clothTakenReportPending = false;
  }

  if (autoMode) {
    if (currentNearby && !wasNearby) {
      if (detectedStart == 0) {
        detectedStart = now;
      } else if (now - detectedStart >= TOF_DEBOUNCE_MS) {
        transitionStart = now;
        wasNearby = true;
        leftStart = 0;
      }
    } else if (!currentNearby && wasNearby) {
      if (leftStart == 0) {
        leftStart = now;
      } else if (now - leftStart >= TOF_DEBOUNCE_MS) {
        transitionStart = now;
        wasNearby = false;
        unsigned long stayDurationSeconds = (now - detectedStart) / 1000;
        sendStayRecordToServer(stayDurationSeconds);
        detectedStart = 0;
      }
    } else {
      if (currentNearby) leftStart = 0;
      else detectedStart = 0;
    }
  }

  // ---- BH1750 ambient trim: fast sampling with bounded influence ----
  // Only learn the idle-light correction while nobody is nearby. Otherwise the
  // brighter garment lighting would contaminate the next idle brightness.
  static unsigned long lastLuxAutoRead = 0;
  if (autoMode
      && !wasNearby
      && bh1750Ready
      && (now - lastLuxAutoRead >= BH1750_CONTROL_INTERVAL_MS)) {
    float lux = lightMeter.readLightLevel();
    bool luxValid = isfinite(lux) && lux >= 0.0f;
    FailureDecision luxDecision = thresholdGateRecord(bh1750ReadGate, luxValid, 3);
    if (luxDecision == DIAG_LOG_FAILURE) {
      diagnosticLogSensor("BH1750 invalid reads", true);
    } else if (luxDecision == DIAG_LOG_RECOVERY) {
      diagnosticLogSensor("BH1750 reads recovered");
    }
    if (luxValid) {
      const int previousBrightness = luxAutoBrightness;
      luxAutoState = updateLuxAutoControl(
        luxAutoState,
        lux,
        luxAutoTarget,
        brightness,
        luxAutoConfig
      );
      luxAutoBrightness = luxAutoState.brightness;
      if (luxAutoBrightness != previousBrightness) {
        DEBUG_SERIAL.printf(
          "[BH1750] lux=%.1f filtered=%.1f base=%d auto=%d\n",
          lux,
          luxAutoState.filteredLux,
          brightness,
          luxAutoBrightness
        );
      }
    }
    lastLuxAutoRead = now;
  }

  float ratio = float(now - transitionStart) / TOF_TRANSITION_MS;
  if (ratio > 1.0f) ratio = 1.0f;

  int br = brightness;
  int tp = temp;

  if (autoMode) {
    int idleBrightness = bh1750Ready ? luxAutoBrightness : brightness;

    if (wasNearby) {
      br = brightness + int((recommendedBrightness - brightness) * ratio);
      tp = temp + int((recommendedTemp - temp) * ratio);
    } else {
      br = recommendedBrightness + int((idleBrightness - recommendedBrightness) * ratio);
      tp = recommendedTemp + int((temp - recommendedTemp) * ratio);
    }
  }

  // BLUR HIGH 仅当: 自动模式 + 有人靠近 + 聚酯纤维
  bool blurHigh = (autoMode && wasNearby && strcmp(fabric, "polyester") == 0);
  digitalWrite(BLUR, blurHigh ? HIGH : LOW);

  if (now - lastLightUpdate > lightUpdateInterval) {
    applyLightSettings(br, tp);
    lastLightUpdate = now;
  }
}
