#pragma once
#include "app_config.h"

bool connectWiFi(const String& ssid, const String& password, unsigned long timeoutMs);
bool connectSavedWiFi();
bool beginSavedWiFiConnection();
bool finishSavedWiFiConnection();
void startParallelProvision();
void startAPPortal();
void handleProvisioningLoop();
bool ensureWiFiReady();
String getPortalHtml();
