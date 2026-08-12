#pragma once

// ESP8266 HTTPClient calls are synchronous. Keep non-critical HTTP work out of
// the Wave loop so a slow server cannot freeze PWM updates for several seconds.
constexpr bool shouldRunNonCriticalHttp(bool waveEnabled) {
  return !waveEnabled;
}
