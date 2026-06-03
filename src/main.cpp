#include "app_config.h"
#include "config/config_manager.h"
#include "network/wifi_manager.h"
#include "network/http_reporter.h"
#include "network/ws_client.h"
#include "device/light_control.h"
#include "device/sensor_manager.h"
#include "device/ota_manager.h"
#include "device/arm_controller.h"
#include "device/self_test.h"
#include "server/local_server.h"
#include "network/udp_discovery.h"

// ==================== HTTP 烟雾测试 ====================
// 启用后固件只做 WiFi + HTTP announce，排除其他所有模块干扰


// ===================== 传感器 / 外设 实例 =====================
BH1750 lightMeter;
Adafruit_VL53L0X lox = Adafruit_VL53L0X();
WebSocketsClient webSocket;
ESP8266WebServer server(80);
WiFiUDP udp;

// ===================== 运行状态 =====================
bool bh1750Ready = false;
bool tofReady = false;
bool fsReady = false;
bool wsConnected = false;
bool selfTestDone = false;
bool selfTestFsOk = false;
bool selfTestWifiOk = false;
bool selfTestWsOk = false;
bool selfTestBh1750Ok = false;
bool selfTestTofOk = false;
bool selfTestNanoOk = false;
unsigned long selfTestCheckedAtMs = 0;
String selfTestNanoStatus = "not_run";
bool enableBroadcast = true;
bool enableAnnounce = true;
bool provisioningMode = false;
bool smartConfigActive = false;
bool smartConfigDoneHandled = false;
unsigned long smartConfigStartMs = 0;
bool otaInProgress = false;
String firmwareChannel = FW_CHANNEL;
String otaStatus = "idle";
int otaProgress = 0;
int lastOtaProgressLog = -1;
int lastOtaProgressReport = -1;
unsigned long lastOtaProgressReportMs = 0;

unsigned long lastLightSend = 0;
unsigned long lastLightUpdate = 0;
unsigned long lastAnnounce = 0;
unsigned long lastBroadcast = 0;
unsigned long lastPing = 0;
unsigned long lastToFRead = 0;

bool pendingStateReport = false;
bool pendingStateReportWithSelfTest = false;
unsigned long lastStateReportFailedAt = 0;
unsigned long lastWsConnectedMs = 0;
bool bootOnlineReportRequested = false;
bool bootOnlineReportDone = false;
bool bootSelfTestStarted = false;
bool bootSelfTestReportDone = false;

IPAddress cachedBroadcastIP;
bool broadcastIPCached = false;

// ===================== 灯光控制参数 =====================
int brightness = 80;
int temp = 4000;
bool autoMode = true;
int recommendedBrightness = 80;
int recommendedTemp = 4000;
int luxAutoTarget = 300;
int luxAutoBrightness = 50;
char fabric[16] = "unknown";

bool effectWaveEnabled = false;
int effectBaseTemp = 3800;
int effectRange = 500;
float effectSpeed = 1.0f;
int effectBrightness = 80;
float effectPhaseOffset = 0.0f;
int effectRestoreBrightness = 80;
int effectRestoreTemp = 4000;
unsigned long effectStartMs = 0;
unsigned long lastEffectUpdateMs = 0;
const unsigned long WAVE_UPDATE_INTERVAL_MS = 80;

// ===================== Nano 云台 / 滑轨控制参数 =====================
int panDeg = 0;
int tiltDeg = 0;
int sliderMm = 0;
int angleStep = 5;
int sliderStep = 50;
int panSpeedDeg = 60;
int tiltSpeedDeg = 60;
int sliderSpeedMm = 100;

// ===================== 设备配置 =====================
DeviceConfig cfg;
String deviceId;

// ===================== setup / loop =====================
void setup() {
  Serial.begin(NANO_BAUD);
  DEBUG_SERIAL.begin(115200);
  delay(200);

  deviceId = makeDeviceId();

  DEBUG_SERIAL.println("\n========================");
  DEBUG_SERIAL.println("设备启动");
  DEBUG_SERIAL.println("ID = " + deviceId);
  DEBUG_SERIAL.println("FW = " + String(FW_VERSION));
  DEBUG_SERIAL.println("========================");

  if (!LittleFS.begin()) {
    DEBUG_SERIAL.println("[FS] LittleFS 挂载失败");
  } else {
    fsReady = true;
  }

  setupHardwareAndSensors();

  // 初始化 Nano 云台：细分配置 + 默认速度
  sendNano('m', "16");
  applyArmSpeed("normal");
  bool hasConfig = loadConfig();
  bool wifiOk = false;

  if (hasConfig) {
    DEBUG_SERIAL.println("[BOOT] Saved config found, trying to connect...");
    wifiOk = connectSavedWiFi();
  } else {
    DEBUG_SERIAL.println("[BOOT] No saved config, entering parallel provisioning...");
  }

  if (!wifiOk) {
    startParallelProvision();
    return;
  }

  broadcastIPCached = false;
  setupDeviceHttpServer();
  beginWebSocketClient();
  sendAnnounce();
}

void loop() {
  server.handleClient();
  pollNano();

  if (provisioningMode) {
    handleProvisioningLoop();
    return;
  }

  if (!ensureWiFiReady()) return;

  webSocket.loop();
  handleWsHeartbeat();

  if (otaInProgress) return;

  // 摇杆连续运动更新 (每帧)
  updateArmJoystickMotion();
  handleSelfTestTask();
  handleDeviceStateReportTask();
  handleLocateBreathTask();

  broadcastDevice();

  if (!isLocateBreathActive()) {
    if (effectWaveEnabled) {
      updateEffectLoop();
    } else {
      updateLightingByToF();
    }
  }

  unsigned long now = millis();

  if (now - lastAnnounce > announceInterval) {
    lastAnnounce = now;
    sendAnnounce();
  }

  if (now - lastLightSend > lightSendInterval) {
    lastLightSend = now;
    sendLightLevelToServer();
  }
}
