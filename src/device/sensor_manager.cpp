#include "device/sensor_manager.h"
#include "device/light_control.h"
#include "network/http_reporter.h"
#include "network/ws_client.h"
#include "diagnostics/diagnostic_logger.h"

static ThresholdGate tofReadGate{};
static ThresholdGate bh1750ReadGate{};
static const uint8_t BH1750_I2C_ADDRESS = 0x23;

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

void updateLightingByToF() {
  static String lastClothState = "";
  static unsigned long lastClothStateReportAt = 0;
  static int lastClothDistanceMm = -1;

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
    if (lastClothState != "unknown" || now - lastClothStateReportAt > 30000) {
      lastClothState = "unknown";
      lastClothDistanceMm = -1;
      lastClothStateReportAt = now;
      sendLampClothState("unknown", false);
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

  bool tofSampleValid = measure.RangeStatus == 0 && measure.RangeMilliMeter <= TOF_MAX_RANGE_MM;
  FailureDecision tofDecision = thresholdGateRecord(tofReadGate, tofSampleValid, 20);
  if (tofDecision == DIAG_LOG_FAILURE) {
    char message[64];
    snprintf(message, sizeof(message), "ToF invalid status=%u range=%u",
             measure.RangeStatus, measure.RangeMilliMeter);
    diagnosticLogSensor(message, true);
  } else if (tofDecision == DIAG_LOG_RECOVERY) {
    diagnosticLogSensor("ToF reads recovered");
  }

  if (measure.RangeMilliMeter > TOF_MAX_RANGE_MM) return;

  static bool wasNearby = false;
  static unsigned long transitionStart = 0;
  static unsigned long detectedStart = 0;
  static unsigned long leftStart = 0;

  bool currentNearby = (measure.RangeMilliMeter < 2000);
  const char* clothState = currentNearby ? "taken" : "on_rack";

  DEBUG_SERIAL.printf("测距: %d mm\n", measure.RangeMilliMeter);

  // 状态变化立即上报；距离跳变 >80mm 需至少间隔 3s 防抖；10s 心跳保活
  if (
    lastClothState != clothState ||
    (now - lastClothStateReportAt > 3000 && abs((int)measure.RangeMilliMeter - lastClothDistanceMm) > 80) ||
    now - lastClothStateReportAt > 10000
  ) {
    lastClothState = clothState;
    lastClothDistanceMm = measure.RangeMilliMeter;
    lastClothStateReportAt = now;
    sendLampClothState(clothState, false);
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

  // ---- Lux P 控制器：无人时根据环境光自动调节亮度 ----
  static unsigned long lastLuxAutoRead = 0;
  if (autoMode && bh1750Ready && (now - lastLuxAutoRead > 2000)) {
    float lux = lightMeter.readLightLevel();
    bool luxValid = isfinite(lux) && lux >= 0.0f;
    FailureDecision luxDecision = thresholdGateRecord(bh1750ReadGate, luxValid, 3);
    if (luxDecision == DIAG_LOG_FAILURE) {
      diagnosticLogSensor("BH1750 invalid reads", true);
    } else if (luxDecision == DIAG_LOG_RECOVERY) {
      diagnosticLogSensor("BH1750 reads recovered");
    }
    if (luxValid) {
      int error = luxAutoTarget - (int)lux;
      luxAutoBrightness += (int)(error * 0.1f);
      luxAutoBrightness = constrain(luxAutoBrightness, 5, 100);
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
