#include "weather.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ctype.h>
#include <math.h>
#include <time.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy include/secrets.h.example to include/secrets.h and fill it in"
#endif

// Deliberately a hard error rather than a default. A silently wrong default
// would show a plausible forecast for somewhere else entirely, which is a much
// worse failure than not building.
#if !defined(WEATHER_LAT) || !defined(WEATHER_LON)
#error "Add WEATHER_LAT and WEATHER_LON to include/secrets.h - see secrets.h.example"
#endif

// ---------------------------------------------------------------------------
// WMO 4677 weather codes -> what we draw and what we call it.
//
// Open-Meteo reports the full code range, but a 74 px forecast column can't
// spend pixels distinguishing "light drizzle" from "moderate drizzle". The
// icon buckets are deliberately coarse; the text keeps the detail.
// ---------------------------------------------------------------------------
WxIcon wxIconForCode(int c) {
  switch (c) {
  case 0:
    return WxIcon::Clear;
  case 1:
  case 2:
    return WxIcon::PartlyCloudy;
  case 3:
    return WxIcon::Overcast;
  case 45:
  case 48:
    return WxIcon::Fog;
  case 51:
  case 53:
  case 55:
  case 56:
  case 57:
    return WxIcon::Drizzle;
  case 61:
  case 63:
  case 65:
  case 66:
  case 67:
  case 80:
  case 81:
  case 82:
    return WxIcon::Rain;
  case 71:
  case 73:
  case 75:
  case 77:
  case 85:
  case 86:
    return WxIcon::Snow;
  case 95:
  case 96:
  case 99:
    return WxIcon::Storm;
  default:
    return WxIcon::Unknown;
  }
}

const char *wxConditionForCode(int c) {
  switch (c) {
  case 0:
    return "Clear";
  case 1:
    return "Mainly clear";
  case 2:
    return "Partly cloudy";
  case 3:
    return "Overcast";
  case 45:
    return "Fog";
  case 48:
    return "Freezing fog";
  case 51:
    return "Light drizzle";
  case 53:
    return "Drizzle";
  case 55:
    return "Heavy drizzle";
  case 56:
  case 57:
    return "Freezing drizzle";
  case 61:
    return "Light rain";
  case 63:
    return "Rain";
  case 65:
    return "Heavy rain";
  case 66:
  case 67:
    return "Freezing rain";
  case 71:
    return "Light snow";
  case 73:
    return "Snow";
  case 75:
    return "Heavy snow";
  case 77:
    return "Snow grains";
  case 80:
    return "Light showers";
  case 81:
    return "Showers";
  case 82:
    return "Heavy showers";
  case 85:
    return "Snow showers";
  case 86:
    return "Heavy snow showers";
  case 95:
    return "Thunderstorm";
  case 96:
  case 99:
    return "Thunderstorm, hail";
  default:
    return "";
  }
}

// 16-point compass. Sectors are 22.5 deg wide and centred on the label, hence
// the half-sector offset before the divide.
static const char *compass(float deg) {
  static const char *pts[16] = {"N",  "NNE", "NE", "ENE", "E",  "ESE",
                                "SE", "SSE", "S",  "SSW", "SW", "WSW",
                                "W",  "WNW", "NW", "NNW"};
  if (deg < 0)
    deg += 360.0f;
  int i = (int)((deg + 11.25f) / 22.5f) % 16;
  return pts[i < 0 ? 0 : i];
}

// "2026-07-26" -> "SAT". The API is queried with timezone=auto, so its dates
// are already local days; mktime at midday keeps a DST changeover from tipping
// the label onto the wrong side of midnight.
static void weekdayLabel(const char *iso, char *out, size_t cap) {
  out[0] = '\0';
  if (!iso || strlen(iso) < 10)
    return;

  struct tm tmDay = {};
  tmDay.tm_year = atoi(iso) - 1900;
  tmDay.tm_mon = atoi(iso + 5) - 1;
  tmDay.tm_mday = atoi(iso + 8);
  tmDay.tm_hour = 12;
  tmDay.tm_isdst = -1;

  if (mktime(&tmDay) == (time_t)-1)
    return;
  strftime(out, cap, "%a", &tmDay);
  for (char *p = out; *p; ++p)
    *p = toupper((unsigned char)*p);
}

static int roundToInt(float v) { return (int)lroundf(v); }

// ---------------------------------------------------------------------------
// Apparent temperature, the US National Weather Service way: heat index when
// it is hot, wind chill when it is cold and windy, plain air temperature in
// between. Both regressions are defined in Fahrenheit and mph.
//
// Deliberately not Open-Meteo's `apparent_temperature` field. That is a
// different quantity - it folds in wind cooling and solar radiation - and it
// reads several degrees below the heat index in humid heat, which is exactly
// the case the number exists to warn you about. Every US weather app shows the
// heat index, so a panel next to a phone should too.
// ---------------------------------------------------------------------------
static float apparentF(float tF, float rh, float windMph) {
  if (tF <= 50.0f && windMph > 3.0f) {
    // Wind chill needs real wind; below ~3 mph the regression runs away.
    const float v = powf(windMph, 0.16f);
    return 35.74f + 0.6215f * tF - 35.75f * v + 0.4275f * tF * v;
  }

  // Between the two regimes neither correction applies and "feels like" is
  // just the air temperature.
  if (tF < 80.0f)
    return tF;

  // The NWS procedure: try the cheap form, and only escalate to the full
  // regression once the average agrees it is genuinely hot.
  const float simple =
      0.5f * (tF + 61.0f + ((tF - 68.0f) * 1.2f) + (rh * 0.094f));
  if ((simple + tF) * 0.5f < 80.0f)
    return simple;

  float hi = -42.379f + 2.04901523f * tF + 10.14333127f * rh -
             0.22475541f * tF * rh - 0.00683783f * tF * tF -
             0.05481717f * rh * rh + 0.00122874f * tF * tF * rh +
             0.00085282f * tF * rh * rh - 0.00000199f * tF * tF * rh * rh;

  // Rothfusz is a fit to a table and drifts at the dry and humid extremes;
  // these are the NWS's own corrections for those corners.
  if (rh < 13.0f && tF <= 112.0f)
    hi -= ((13.0f - rh) / 4.0f) * sqrtf((17.0f - fabsf(tF - 95.0f)) / 17.0f);
  else if (rh > 85.0f && tF <= 87.0f)
    hi += ((rh - 85.0f) / 10.0f) * ((87.0f - tF) / 5.0f);

  return hi;
}

// Same thing in whatever units the panel is configured for.
static float apparentInDisplayUnits(float temp, float rh, float wind) {
#if WEATHER_IMPERIAL
  return apparentF(temp, rh, wind);
#else
  // The formulas only exist in F/mph, so convert in and back out again.
  const float tF = temp * 9.0f / 5.0f + 32.0f;
  const float windMph = wind * 0.621371f;
  return (apparentF(tF, rh, windMph) - 32.0f) * 5.0f / 9.0f;
#endif
}

#if WEATHER_IMPERIAL
#define WX_UNIT_QS "&temperature_unit=fahrenheit&wind_speed_unit=mph"
#else
#define WX_UNIT_QS "" // API defaults are already celsius and km/h
#endif

bool weatherFetch(WeatherData &out) {
  if (WiFi.status() != WL_CONNECTED)
    return false;

  char url[512];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast"
           "?latitude=%.4f&longitude=%.4f"
           // Humidity, not apparent_temperature: the "feels like" is computed
           // here instead, see apparentF().
           "&current=temperature_2m,relative_humidity_2m,weather_code,"
           "wind_speed_10m,wind_direction_10m"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min"
           "&timezone=auto&forecast_days=%d"
           "&models=" WEATHER_MODEL WX_UNIT_QS,
           (double)WEATHER_LAT, (double)WEATHER_LON,
           WEATHER_FORECAST_DAYS + 1); // +1: index 0 is today

  // Certificates are not pinned. This is a public, unauthenticated forecast on
  // a display with no secrets to leak, and a pinned root would silently brick
  // the weather screen the day Open-Meteo rotates it.
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(WEATHER_HTTP_TIMEOUT_MS / 1000);

  HTTPClient http;
  http.setTimeout(WEATHER_HTTP_TIMEOUT_MS);
  http.setConnectTimeout(WEATHER_HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    Serial.println("[wx] http begin failed");
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[wx] GET failed, http=%d\n", code);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("[wx] parse failed: %s\n", err.c_str());
    return false;
  }

  JsonObjectConst cur = doc["current"];
  JsonObjectConst daily = doc["daily"];
  if (cur.isNull() || daily.isNull()) {
    Serial.println("[wx] response had no current/daily block");
    return false;
  }

  // Build into a local and commit at the end, so a response that turns out to
  // be half-formed can't leave a mix of new and old values on the screen.
  WeatherData w;

  int wmo = cur["weather_code"] | -1;
  const float temp = cur["temperature_2m"] | 0.0f;
  const float wind = cur["wind_speed_10m"] | 0.0f;
  // Default to 50 % rather than 0: a missing humidity should leave the feels-
  // like near the air temperature, not invent a desert.
  const float rh = cur["relative_humidity_2m"] | 50.0f;

  w.temp = roundToInt(temp);
  w.feels = roundToInt(apparentInDisplayUnits(temp, rh, wind));
  w.icon = wxIconForCode(wmo);
  strncpy(w.condition, wxConditionForCode(wmo), sizeof(w.condition) - 1);
  w.wind = roundToInt(wind);
  strncpy(w.windDir, compass(cur["wind_direction_10m"] | 0.0f),
          sizeof(w.windDir) - 1);

  JsonArrayConst dates = daily["time"];
  JsonArrayConst codes = daily["weather_code"];
  JsonArrayConst highs = daily["temperature_2m_max"];
  JsonArrayConst lows = daily["temperature_2m_min"];

  if (highs.size() > 0 && lows.size() > 0) {
    w.hi = roundToInt(highs[0] | 0.0f);
    w.lo = roundToInt(lows[0] | 0.0f);
    w.hasToday = true;
  }

  // Index 0 is today, already shown beside the current conditions; the strip
  // starts at tomorrow.
  for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
    size_t src = (size_t)i + 1;
    if (src >= highs.size() || src >= lows.size())
      break;

    WeatherDay &d = w.days[i];
    d.hi = roundToInt(highs[src] | 0.0f);
    d.lo = roundToInt(lows[src] | 0.0f);
    d.icon = wxIconForCode(src < codes.size() ? (codes[src] | -1) : -1);
    if (src < dates.size())
      weekdayLabel(dates[src] | "", d.label, sizeof(d.label));
    d.valid = true;
  }

  time_t now = time(nullptr);
  w.fetchedAt = (now > 1600000000) ? (long)now : 0;
  w.valid = true;

  out = w;
  Serial.printf("[wx] %d%s (feels %d, rh %d%%) %s, wind %d %s, %d-day forecast"
                " [%s]\n",
                w.temp, WEATHER_IMPERIAL ? "F" : "C", w.feels, roundToInt(rh),
                w.condition, w.wind, w.windDir, WEATHER_FORECAST_DAYS,
                WEATHER_MODEL);
  return true;
}
