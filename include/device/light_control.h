#pragma once
#include "app_config.h"

void applyLightSettings(int br, int tp);
void stopEffectWaveForManualControl();
void startLocateBreath(int times, int cycleMs);
void stopLocateBreath(bool restoreLight = true);
bool isLocateBreathActive();
void handleLocateBreathTask();
void updateEffectLoop();
void safeCopyFabric(const char* src);

void startCaptureLightingOverride(int brightness, int temp, unsigned long ttlMs);
void stopCaptureLightingOverride();
bool isCaptureLightingOverrideActive();
void handleCaptureLightingOverrideTask();
