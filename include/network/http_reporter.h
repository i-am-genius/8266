#pragma once
#include "app_config.h"

String httpUrl(const String& path);
int    postJsonToServer(const String& path, const String& jsonBody);
void   requestDeviceStateReport(const char* reason, bool includeSelfTest = false);
bool   sendDeviceStateReport(bool includeSelfTest = false, const char* reason = "DIRECT");
void   handleDeviceStateReportTask();
void   sendAnnounce();
void   sendLightLevelToServer();
void   sendStayRecordToServer(unsigned long durationSeconds);
