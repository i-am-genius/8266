#pragma once

// ESP8266 HTTPClient calls are synchronous. Keep non-critical HTTP work out of
// timing-sensitive Wave and person-tracking loops so a slow server cannot
// freeze PWM or gimbal updates for several seconds.
constexpr bool shouldRunNonCriticalHttp(
  bool waveEnabled,
  bool personTrackingActive
) {
  return !waveEnabled && !personTrackingActive;
}
