#pragma once

#include "config.h"
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Weather
//
// Fetched by the ESP32 itself rather than relayed through the bridge, because
// the screen this feeds is the one shown when Claude Code *isn't* running -
// which most often means the PC hosting the bridge is off. A weather panel that
// goes stale exactly when it becomes visible would be worse than none.
//
// Source is Open-Meteo: no API key, no account, and a response small enough to
// parse on the wire. https://open-meteo.com/en/docs
// ---------------------------------------------------------------------------

// Condition buckets. Open-Meteo reports WMO codes (0-99); several map onto the
// same picture, so the renderer works in these instead.
enum class WxIcon : uint8_t {
  Unknown,
  Clear,
  PartlyCloudy,
  Overcast,
  Fog,
  Drizzle,
  Rain,
  Snow,
  Storm,
};

struct WeatherDay {
  bool valid = false;
  char label[4] = ""; // "SAT", from the date the API returned
  WxIcon icon = WxIcon::Unknown;
  int hi = 0;
  int lo = 0;
};

struct WeatherData {
  bool valid = false;   // a fetch has succeeded at least once
  long fetchedAt = 0;   // unix epoch seconds of the last good fetch

  int temp = 0;
  int feels = 0;
  int hi = 0; // today's high / low, from daily[0]
  int lo = 0;
  bool hasToday = false;

  WxIcon icon = WxIcon::Unknown;
  char condition[24] = ""; // "Partly cloudy"

  int wind = 0;
  char windDir[4] = ""; // "NW"

  WeatherDay days[WEATHER_FORECAST_DAYS];
};

// Blocking HTTPS GET + parse, roughly 1-2 s on a good connection. Returns false
// and leaves `out` untouched on any failure, so a flaky fetch never blanks a
// screen that already has good data on it.
bool weatherFetch(WeatherData &out);

// Human-readable condition for a WMO code, and the bucket it renders as.
WxIcon wxIconForCode(int wmoCode);
const char *wxConditionForCode(int wmoCode);
