#pragma once

// ===================== 绯荤粺 Includes =====================
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <LittleFS.h>
#include <Wire.h>
#include <Adafruit_VL53L0X.h>
#include <BH1750.h>

// ===================== Serial aliases =====================
// Serial(GPIO1/GPIO3) talks to Nano.
// Serial1(GPIO2) is debug output only.
#define nanoSerial Serial
#define DEBUG_SERIAL Serial1
#define LOG_WS_HEARTBEAT 0

// ===================== 寮曡剼瀹氫箟 =====================
#define LED_COLD_PIN D2
#define LED_WARM_PIN D1
#define BLUR         D7
#define TOF_SDA_PIN  D5
#define TOF_SCL_PIN  D6

// ===================== 鍥轰欢淇℃伅 =====================
#define FW_DEVICE_TYPE  "lamp"
#define FW_VERSION      "1.0.8"
#define FW_VERSION_CODE 10008
#define FW_CHANNEL      "stable"

// ===================== 鍛藉悕甯搁噺 =====================
const float   WAVE_FREQ_FACTOR          = 0.32f;
const float   TOF_TRANSITION_MS         = 2000.0f;
const uint16_t TOF_MAX_RANGE_MM         = 8200;
const unsigned long TOF_DEBOUNCE_MS     = 1000;
const unsigned long TOF_READ_INTERVAL_MS = 50;
const char* const AP_DEFAULT_PASSWORD   = "12345678";
const char* const WS_PATH               = "/ws/device";
const int     LOCATE_MIN_BRIGHTNESS     = 5;
const int     LOCATE_MAX_BRIGHTNESS     = 100;
const int     LOCATE_STEPS              = 36;

// Garment-coordinate aiming calibration. The default reuses the existing
// aim_cloth preset. Tune FOV values after physical camera/lamp calibration.
const float GARMENT_AIM_DEFAULT_PAN_DEG   = 0.0f;
const float GARMENT_AIM_DEFAULT_TILT_DEG  = 20.0f;
const float GARMENT_AIM_HORIZONTAL_FOV_DEG = 60.0f;
const float GARMENT_AIM_VERTICAL_FOV_DEG   = 45.0f;

// ===================== 榛樿鏈嶅姟鍣ㄩ厤缃?=====================
const char* const DEFAULT_SERVER_HOST = "device.genius.show";
const uint16_t DEFAULT_HTTP_PORT = 80;
const uint16_t DEFAULT_WS_PORT   = 80;
const char* const DEFAULT_WIFI_SSID = "somebody的iPhone";
const char* const DEFAULT_WIFI_PASSWORD = "20040000";

// ===================== 瀹氭椂鍙傛暟 =====================
const unsigned long lightSendInterval    = 30000;
const unsigned long lightUpdateInterval  = 50;
const unsigned long wifiConnectTimeout   = 15000;
const unsigned long smartConfigTimeout   = 30000;

const unsigned long announceInterval     = 5000;
const unsigned long broadcastInterval    = 5000;
const unsigned long wsPingInterval       = 5000;
const unsigned long stateReportBackoffMs = 30000;
const unsigned long otaProgressReportMinIntervalMs = 3000;
const int           otaProgressReportMinStep       = 5;

const int udpPort = 4210;
const uint32_t NANO_BAUD = 57600;

// Nano self-test switches. Set either to false to skip motion or Hall reads.
extern bool selfTestNanoHomingEnabled;
extern bool selfTestNanoHallReadEnabled;

// ===================== DeviceConfig =====================
struct DeviceConfig {
  String ssid = DEFAULT_WIFI_SSID;
  String password = DEFAULT_WIFI_PASSWORD;
  String serverHost;
  uint16_t httpPort;
  uint16_t wsPort;
  String uploadSecret;  // 日志上传密钥
};

// ===================== 浼犳劅鍣?/ 澶栬 (extern) =====================
extern BH1750 lightMeter;
extern Adafruit_VL53L0X lox;
extern WebSocketsClient webSocket;
extern ESP8266WebServer server;
extern WiFiUDP udp;

// ===================== 杩愯鐘舵€?(extern) =====================
extern bool bh1750Ready;
extern bool tofReady;
extern bool fsReady;
extern bool wsConnected;
extern bool selfTestDone;
extern bool selfTestFsOk;
extern bool selfTestWifiOk;
extern bool selfTestWsOk;
extern bool selfTestBh1750Ok;
extern bool selfTestTofOk;
extern bool selfTestNanoOk;
extern unsigned long selfTestCheckedAtMs;
extern String selfTestNanoStatus;
extern bool enableBroadcast;
extern bool enableAnnounce;
extern bool backendDeviceAdded;
extern bool wsClientStarted;
extern bool provisioningMode;
extern bool smartConfigActive;
extern bool smartConfigDoneHandled;
extern unsigned long smartConfigStartMs;
extern bool otaInProgress;
extern String firmwareChannel;
extern String otaStatus;
extern int otaProgress;
extern int lastOtaProgressLog;
extern int lastOtaProgressReport;
extern unsigned long lastOtaProgressReportMs;

extern unsigned long lastLightSend;
extern unsigned long lastLightUpdate;
extern unsigned long lastAnnounce;
extern unsigned long lastBroadcast;
extern unsigned long lastPing;
extern unsigned long lastToFRead;

extern IPAddress cachedBroadcastIP;
extern bool broadcastIPCached;

extern bool pendingStateReport;
extern bool pendingStateReportWithSelfTest;
extern unsigned long lastStateReportFailedAt;
extern unsigned long lastWsConnectedMs;
extern bool bootOnlineReportRequested;
extern bool bootOnlineReportDone;
extern bool bootSelfTestStarted;
extern bool bootSelfTestReportDone;

// ===================== 鐏厜鎺у埗鍙傛暟 (extern) =====================
extern int brightness;
extern int temp;
extern bool autoMode;
extern int recommendedBrightness;
extern int recommendedTemp;
extern int luxAutoTarget;
extern int luxAutoBrightness;
extern char fabric[16];

extern bool effectWaveEnabled;
extern int effectBaseTemp;
extern int effectRange;
extern float effectSpeed;
extern int effectBrightness;
extern float effectPhaseOffset;
extern int effectRestoreBrightness;
extern int effectRestoreTemp;
extern unsigned long effectStartMs;
extern unsigned long lastEffectUpdateMs;
extern const unsigned long WAVE_UPDATE_INTERVAL_MS;

// ===================== Nano 浜戝彴鍙傛暟 (extern) =====================
extern int panDeg;
extern int tiltDeg;
extern int sliderMm;
extern int angleStep;
extern int sliderStep;
extern int panSpeedDeg;
extern int tiltSpeedDeg;
extern int sliderSpeedMm;

// ===================== 璁惧閰嶇疆 (extern) =====================
extern DeviceConfig cfg;
extern String deviceId;
