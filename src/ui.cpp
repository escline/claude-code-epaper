#include "ui.h"
#include "config.h"
#include "epd_panel.h"
#include "weather_icons.h"

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <SPI.h>
#include <time.h>

static EpdDisplay display(EpdPanel(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// ---------------------------------------------------------------------------
// Zones
//
// Each zone owns a rectangle, a signature of the content currently on the
// glass, and a floor on how often it may repaint. A zone redraws only when its
// signature changes AND its interval has elapsed, which is what keeps a
// per-message push firehose from wearing the panel out.
//
// Two screens share the table. The header is on both; the rest belong to one
// or the other, and only the active screen's zones are considered for repaint.
// Without that gating the status gauges' countdown would keep ticking behind
// the weather screen and drag the panel through a refresh a minute.
// ---------------------------------------------------------------------------
enum ZoneId {
  Z_HEADER,
  // Status screen
  Z_STATUS,
  Z_GAUGE,
  Z_FOOTER,
  // Weather screen
  Z_WX_NOW,
  Z_WX_FC,
  Z_WX_FOOT,
  ZONE_COUNT
};

struct Zone {
  int16_t y, h;
  uint32_t interval;
  uint32_t drawnSig; // signature of what is on the glass
  uint32_t wantSig;  // signature of what we want there
  uint32_t lastDraw;
};

static Zone zones[ZONE_COUNT] = {
    {Z_HEADER_Y, Z_HEADER_H, FOOTER_MIN_INTERVAL_MS, 0, 0, 0},
    {Z_STATUS_Y, Z_STATUS_H, STATUS_MIN_INTERVAL_MS, 0, 0, 0},
    {Z_GAUGE_Y, Z_GAUGE_H, GAUGE_MIN_INTERVAL_MS, 0, 0, 0},
    {Z_FOOTER_Y, Z_FOOTER_H, FOOTER_MIN_INTERVAL_MS, 0, 0, 0},
    // Weather only moves when a fetch lands, every 20 minutes, so the floor
    // here is a formality rather than a throttle.
    {Z_WX_NOW_Y, Z_WX_NOW_H, FOOTER_MIN_INTERVAL_MS, 0, 0, 0},
    {Z_WX_FC_Y, Z_WX_FC_H, FOOTER_MIN_INTERVAL_MS, 0, 0, 0},
    {Z_WX_FOOT_Y, Z_WX_FOOT_H, FOOTER_MIN_INTERVAL_MS, 0, 0, 0},
};

static ClaudeState current;
static WeatherData weather;
static bool haveState = false;
static char bannerLine1[32] = "";
static char bannerLine2[48] = "";
static bool showBanner = true;

static bool fullRefreshPending = true;
static uint32_t lastFullRefresh = 0;
static uint16_t partialsSinceFull = 0;
static bool wxOnGlass = false; // which screen the panel is currently showing

// ---------------------------------------------------------------------------
// Screen selection
//
// The weather screen takes over whenever there is nothing live to say about
// Claude Code. "Nothing live" is two distinct cases and both count: the bridge
// is gone (OFFLINE), or the bridge is up and reports no session open. An IDLE
// session that is merely waiting on you is *not* one of them - that is a state
// worth showing, and burying it under a forecast would defeat the display.
// ---------------------------------------------------------------------------
static bool weatherScreenActive() {
#if WEATHER_ENABLED
  // Mid-connection the banner is the more useful thing to be looking at, and
  // no fetch has landed yet anyway.
  if (showBanner || !weather.valid)
    return false;
  if (current.status == ClaudeStatus::Offline)
    return true;
  // hasSessions guards against a bridge too old to publish the count: without
  // it, every IDLE would read as "no session" and the panel would hide a live
  // status behind the weather.
  return current.status == ClaudeStatus::Idle && current.hasSessions &&
         current.sessions == 0;
#else
  return false;
#endif
}

static bool zoneActive(ZoneId id, bool wx) {
  if (id == Z_HEADER)
    return true;
  return (id >= Z_WX_NOW) == wx;
}

// FNV-1a. Only needs to detect change, not resist collisions.
static uint32_t hashInit() { return 2166136261u; }
static uint32_t hashData(uint32_t h, const void *p, size_t n) {
  const uint8_t *b = (const uint8_t *)p;
  while (n--) {
    h ^= *b++;
    h *= 16777619u;
  }
  return h;
}
static uint32_t hashStr(uint32_t h, const char *s) {
  return hashData(h, s, strlen(s));
}
static uint32_t hashInt(uint32_t h, int32_t v) {
  return hashData(h, &v, sizeof(v));
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------
static void textAt(const char *s, int16_t x, int16_t baseline,
                   const GFXfont *font, uint16_t color = GxEPD_BLACK) {
  display.setFont(font);
  display.setTextColor(color);
  display.setCursor(x, baseline);
  display.print(s);
}

static int16_t textWidth(const char *s, const GFXfont *font) {
  int16_t bx, by;
  uint16_t bw, bh;
  display.setFont(font);
  display.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
  return bw;
}

static void textCentered(const char *s, int16_t baseline, const GFXfont *font,
                         uint16_t color = GxEPD_BLACK) {
  int16_t bx, by;
  uint16_t bw, bh;
  display.setFont(font);
  display.getTextBounds(s, 0, baseline, &bx, &by, &bw, &bh);
  display.setTextColor(color);
  display.setCursor((SCREEN_W - bw) / 2 - bx, baseline);
  display.print(s);
}

static void textRight(const char *s, int16_t rightX, int16_t baseline,
                      const GFXfont *font, uint16_t color = GxEPD_BLACK) {
  textAt(s, rightX - textWidth(s, font), baseline, font, color);
}

static void textCenteredIn(const char *s, int16_t centerX, int16_t baseline,
                           const GFXfont *font) {
  textAt(s, centerX - textWidth(s, font) / 2, baseline, font);
}

// Where the cursor ends up after printing `s` - which is NOT what textWidth
// reports. getTextBounds measures the ink box: it excludes both side bearings,
// and a trailing space contributes no ink at all. That is the right answer for
// centring and right-alignment, and the wrong one for placing anything *after*
// a string. Getting these two confused put the degree ring on top of the last
// digit and swallowed the space after "feels".
//
// Summing xAdvance is what Adafruit's own cursor does. Reading the font tables
// directly is safe here because the ESP32 has a flat address space and PROGMEM
// is a no-op; on AVR this would need pgm_read_*.
static int16_t textAdvance(const char *s, const GFXfont *font) {
  if (!s || !font)
    return 0;
  int16_t total = 0;
  for (const uint8_t *p = (const uint8_t *)s; *p; ++p) {
    if (*p < font->first || *p > font->last)
      continue;
    total += font->glyph[*p - font->first].xAdvance;
  }
  return total;
}

// Draws a run of text and returns the x to continue at, for lines assembled
// from alternating text and degree marks.
static int16_t textRun(const char *s, int16_t x, int16_t baseline,
                       const GFXfont *font) {
  textAt(s, x, baseline, font);
  return x + textAdvance(s, font);
}

// A temperature and its degree mark. Adafruit's GFX fonts stop at ASCII 126,
// so there is no '°' glyph to print - it gets drawn as a ring instead. The cap
// height comes from measuring a digit in the font actually in use, so the ring
// lines up with the top of the numerals at 9 pt and at 24 pt without a
// per-font fudge constant. Returns the x to continue at.
static int16_t drawTemp(int16_t x, int16_t baseline, int value,
                        const GFXfont *font, int16_t r) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", value);
  // Advance, not ink width: the ring goes after the digits, and "114" has a
  // wide left bearing on the leading 1 that ink measurement discards.
  int16_t w = textAdvance(buf, font);
  textAt(buf, x, baseline, font);

  int16_t bx, by;
  uint16_t bw, bh;
  display.setFont(font);
  display.getTextBounds("8", 0, 0, &bx, &by, &bw, &bh);

  const int16_t cx = x + w + r + 2;
  const int16_t cy = baseline - (int16_t)bh + r;
  display.drawCircle(cx, cy, r, GxEPD_BLACK);
  // A single-pixel ring disappears next to 24 pt numerals; double it up once
  // the mark is big enough for the second circle to land on distinct pixels.
  if (r >= 4)
    display.drawCircle(cx, cy, r - 1, GxEPD_BLACK);

  return cx + r + 3;
}

// "2h 14m", "3d 4h", "12m". Empty when the deadline is unknown or passed.
static void formatRemaining(long secs, char *buf, size_t cap) {
  if (secs <= 0) {
    snprintf(buf, cap, "resetting");
    return;
  }
  long d = secs / 86400;
  long h = (secs % 86400) / 3600;
  long m = (secs % 3600) / 60;
  if (d > 0)
    snprintf(buf, cap, "resets in %ldd %ldh", d, h);
  else if (h > 0)
    // 5-minute steps above an hour. A per-minute tick would repaint the gauge
    // zone 60 times an hour, and every partial update leaves a little more
    // ghosting behind; at this range the extra precision is not worth it.
    snprintf(buf, cap, "resets in %ldh %02ldm", h, (m / 5) * 5);
  else
    snprintf(buf, cap, "resets in %ldm", m);
}

// ---------------------------------------------------------------------------
// Zone painters. Each assumes its rectangle has already been cleared.
// ---------------------------------------------------------------------------
static void paintHeader() {
  display.fillRect(0, Z_HEADER_Y, SCREEN_W, Z_HEADER_H, GxEPD_BLACK);
  textAt("CLAUDE CODE", MARGIN, Z_HEADER_Y + 18, &FreeSansBold9pt7b,
         GxEPD_WHITE);

  time_t now = time(nullptr);
  struct tm tmNow;
  // Before NTP syncs, the epoch is near 1970; don't show a bogus clock.
  if (now > 1600000000 && localtime_r(&now, &tmNow)) {
    char clock[8];
    strftime(clock, sizeof(clock), "%H:%M", &tmNow);
    textRight(clock, SCREEN_W - MARGIN, Z_HEADER_Y + 18, &FreeSansBold9pt7b,
              GxEPD_WHITE);
  }
}

static void paintStatus() {
  if (showBanner) {
    textCentered(bannerLine1, Z_STATUS_Y + 44, &FreeSansBold12pt7b);
    if (bannerLine2[0])
      textCentered(bannerLine2, Z_STATUS_Y + 70, &FreeSans9pt7b);
    return;
  }

  const char *label = statusLabel(current.status);

  // NEEDS YOU is the whole point of the display, so it gets inverted to be
  // readable across a room and unmistakable in peripheral vision.
  if (current.status == ClaudeStatus::NeedsYou) {
    display.fillRect(MARGIN, Z_STATUS_Y + 4, SCREEN_W - 2 * MARGIN, 52,
                     GxEPD_BLACK);
    textCentered(label, Z_STATUS_Y + 44, &FreeSansBold24pt7b, GxEPD_WHITE);
  } else {
    textCentered(label, Z_STATUS_Y + 44, &FreeSansBold24pt7b);
  }

  if (current.detail[0])
    textCentered(current.detail, Z_STATUS_Y + 76, &FreeSans9pt7b);
}

static void paintGaugeRow(const char *label, const Gauge &g, int16_t labelY,
                          int16_t barY, int16_t resetY) {
  textAt(label, MARGIN, labelY, &FreeSansBold9pt7b);

  char pct[8];
  if (g.valid)
    snprintf(pct, sizeof(pct), "%d%%", g.usedPct);
  else
    snprintf(pct, sizeof(pct), "--");
  textRight(pct, SCREEN_W - MARGIN, labelY, &FreeSansBold9pt7b);

  const int16_t barW = SCREEN_W - 2 * MARGIN;
  display.drawRect(MARGIN, barY, barW, BAR_H, GxEPD_BLACK);
  if (g.valid && g.usedPct > 0) {
    int16_t fill = (int32_t)(barW - 4) * g.usedPct / 100;
    display.fillRect(MARGIN + 2, barY + 2, fill, BAR_H - 4, GxEPD_BLACK);
  }

  if (g.valid && g.resetsAt > 0) {
    time_t now = time(nullptr);
    char buf[24];
    formatRemaining((long)(g.resetsAt - now), buf, sizeof(buf));
    textAt(buf, MARGIN, resetY, &FreeSans9pt7b);
  }
}

static void paintGauges() {
  paintGaugeRow("SESSION (5h)", current.session, Z_GAUGE_Y + 18, Z_GAUGE_Y + 24,
                Z_GAUGE_Y + 60);
  paintGaugeRow("WEEK (7d)", current.week, Z_GAUGE_Y + 84, Z_GAUGE_Y + 90,
                Z_GAUGE_Y + 126);
}

static void paintFooter() {
  display.drawFastHLine(MARGIN, Z_FOOTER_Y + 2, SCREEN_W - 2 * MARGIN,
                        GxEPD_BLACK);

  char line[64];
  const char *model = current.model[0] ? current.model : "-";
  const char *proj = current.project[0] ? current.project : "-";
  snprintf(line, sizeof(line), "%s | %s", model, proj);
  textAt(line, MARGIN, Z_FOOTER_Y + 24, &FreeSans9pt7b);

  char right[48];
  right[0] = '\0';
  if (current.hasContext && current.hasCost)
    snprintf(right, sizeof(right), "ctx %d%%   $%.2f", current.contextPct,
             current.costUsd);
  else if (current.hasContext)
    snprintf(right, sizeof(right), "ctx %d%%", current.contextPct);
  else if (current.hasCost)
    snprintf(right, sizeof(right), "$%.2f", current.costUsd);
  if (right[0])
    textAt(right, MARGIN, Z_FOOTER_Y + 44, &FreeSans9pt7b);
}

// ---------------------------------------------------------------------------
// Weather screen painters
// ---------------------------------------------------------------------------
static const char *tempUnit() { return WEATHER_IMPERIAL ? "F" : "C"; }
static const char *windUnit() { return WEATHER_IMPERIAL ? "mph" : "km/h"; }

// True once the "current" conditions are old enough that presenting them as
// current would be a lie. Unknown clock counts as stale - a fetch we cannot
// date is a fetch we cannot vouch for.
static bool weatherStale() {
  time_t now = time(nullptr);
  if (weather.fetchedAt <= 0 || now <= 1600000000)
    return true;
  return (now - weather.fetchedAt) > (long)(WEATHER_STALE_MS / 1000UL);
}

static void paintWxNow() {
  const int16_t iconY = Z_WX_NOW_Y + 28;
  wxDrawIcon(display, weather.icon, MARGIN, iconY, WX_ICON_LG, GxEPD_BLACK,
             GxEPD_WHITE);

  const int16_t tx = MARGIN + WX_ICON_LG + 22;

  // Temperature, degree ring, unit letter.
  int16_t x = drawTemp(tx, Z_WX_NOW_Y + 52, weather.temp, &FreeSansBold24pt7b,
                       6);
  textAt(tempUnit(), x, Z_WX_NOW_Y + 52, &FreeSansBold12pt7b);

  if (weather.condition[0])
    textAt(weather.condition, tx, Z_WX_NOW_Y + 80, &FreeSansBold12pt7b);

  const int16_t detailY = Z_WX_NOW_Y + 104;
  x = textRun("feels ", tx, detailY, &FreeSans9pt7b);
  x = drawTemp(x, detailY, weather.feels, &FreeSans9pt7b, 2);
  if (weather.hasToday) {
    x = textRun("   H ", x, detailY, &FreeSans9pt7b);
    x = drawTemp(x, detailY, weather.hi, &FreeSans9pt7b, 2);
    x = textRun("  L ", x, detailY, &FreeSans9pt7b);
    x = drawTemp(x, detailY, weather.lo, &FreeSans9pt7b, 2);
  }

  char wind[40];
  snprintf(wind, sizeof(wind), "wind %d %s %s", weather.wind, windUnit(),
           weather.windDir);
  // +122, not +124: "mph" has a descender, and the zone's last row is +127.
  textAt(wind, tx, Z_WX_NOW_Y + 122, &FreeSans9pt7b);
}

static void paintWxForecast() {
  display.drawFastHLine(MARGIN, Z_WX_FC_Y, SCREEN_W - 2 * MARGIN, GxEPD_BLACK);

  const int16_t colW = (SCREEN_W - 2 * MARGIN) / WEATHER_FORECAST_DAYS;

  for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
    const WeatherDay &d = weather.days[i];
    if (!d.valid)
      continue;

    const int16_t cx = MARGIN + i * colW + colW / 2;

    textCenteredIn(d.label, cx, Z_WX_FC_Y + 18, &FreeSansBold9pt7b);
    wxDrawIcon(display, d.icon, cx - WX_ICON_SM / 2, Z_WX_FC_Y + 26,
               WX_ICON_SM, GxEPD_BLACK, GxEPD_WHITE);

    // No degree rings here: the column is 74 px wide and the unit is already
    // established by the current conditions above.
    char hilo[12];
    snprintf(hilo, sizeof(hilo), "%d/%d", d.hi, d.lo);
    textCenteredIn(hilo, cx, Z_WX_FC_Y + 82, &FreeSans9pt7b);
  }
}

static void paintWxFooter() {
  display.drawFastHLine(MARGIN, Z_WX_FOOT_Y + 2, SCREEN_W - 2 * MARGIN,
                        GxEPD_BLACK);

  // Say which flavour of "not active" put the weather on screen, so a dead
  // bridge is still distinguishable from a closed editor at a glance.
  const char *why = (current.status == ClaudeStatus::Offline)
                        ? "Claude Code offline"
                        : "no active session";
  textAt(why, MARGIN, Z_WX_FOOT_Y + 24, &FreeSans9pt7b);

  char usage[32];
  usage[0] = '\0';
  if (current.session.valid && current.week.valid)
    snprintf(usage, sizeof(usage), "5h %d%%   7d %d%%", current.session.usedPct,
             current.week.usedPct);
  else if (current.session.valid)
    snprintf(usage, sizeof(usage), "5h %d%%", current.session.usedPct);
  else if (current.week.valid)
    snprintf(usage, sizeof(usage), "7d %d%%", current.week.usedPct);
  if (usage[0])
    textRight(usage, SCREEN_W - MARGIN, Z_WX_FOOT_Y + 24, &FreeSans9pt7b);

  char upd[40];
  struct tm tmFetch;
  time_t fetched = (time_t)weather.fetchedAt;
  if (weather.fetchedAt > 0 && localtime_r(&fetched, &tmFetch)) {
    char clock[8];
    strftime(clock, sizeof(clock), "%H:%M", &tmFetch);
    snprintf(upd, sizeof(upd), "weather %s%s", clock,
             weatherStale() ? " - stale" : "");
  } else {
    snprintf(upd, sizeof(upd), "weather - update time unknown");
  }
  textAt(upd, MARGIN, Z_WX_FOOT_Y + 44, &FreeSans9pt7b);
}

static void paintZone(ZoneId id) {
  switch (id) {
  case Z_HEADER:
    paintHeader();
    break;
  case Z_STATUS:
    paintStatus();
    break;
  case Z_GAUGE:
    paintGauges();
    break;
  case Z_FOOTER:
    paintFooter();
    break;
  case Z_WX_NOW:
    paintWxNow();
    break;
  case Z_WX_FC:
    paintWxForecast();
    break;
  case Z_WX_FOOT:
    paintWxFooter();
    break;
  default:
    break;
  }
}

// ---------------------------------------------------------------------------
// Signatures - what each zone would draw right now
// ---------------------------------------------------------------------------
static uint32_t sigHeader() {
  uint32_t h = hashInit();
  time_t now = time(nullptr);
  struct tm tmNow;
  if (now > 1600000000 && localtime_r(&now, &tmNow)) {
    h = hashInt(h, tmNow.tm_hour * 60 + tmNow.tm_min); // minute resolution
  }
  return h;
}

static uint32_t sigStatus() {
  uint32_t h = hashInit();
  if (showBanner) {
    h = hashStr(h, bannerLine1);
    h = hashStr(h, bannerLine2);
  } else {
    h = hashInt(h, (int)current.status);
    h = hashStr(h, current.detail);
  }
  return h;
}

// Hash the rendered countdown strings rather than the raw epochs, so the
// signature changes exactly when the pixels would. That keeps this in step with
// formatRemaining automatically - coarsening the text there immediately cuts
// the number of partial refreshes, with no second place to remember to update.
static uint32_t sigGauge() {
  time_t now = time(nullptr);
  uint32_t h = hashInit();
  char buf[24];

  h = hashInt(h, current.session.valid ? current.session.usedPct : -1);
  h = hashInt(h, current.week.valid ? current.week.usedPct : -1);

  if (current.session.valid && current.session.resetsAt > 0) {
    formatRemaining((long)(current.session.resetsAt - now), buf, sizeof(buf));
    h = hashStr(h, buf);
  }
  if (current.week.valid && current.week.resetsAt > 0) {
    formatRemaining((long)(current.week.resetsAt - now), buf, sizeof(buf));
    h = hashStr(h, buf);
  }
  return h;
}

static uint32_t sigFooter() {
  uint32_t h = hashInit();
  h = hashStr(h, current.model);
  h = hashStr(h, current.project);
  h = hashInt(h, current.hasContext ? current.contextPct : -1);
  h = hashInt(h, current.hasCost ? (int)(current.costUsd * 100) : -1);
  return h;
}

// Every field paintWxNow draws. A value that isn't hashed here would land in
// the WeatherData struct and never reach the glass.
static uint32_t sigWxNow() {
  uint32_t h = hashInit();
  h = hashInt(h, weather.temp);
  h = hashInt(h, weather.feels);
  h = hashInt(h, (int)weather.icon);
  h = hashStr(h, weather.condition);
  h = hashInt(h, weather.wind);
  h = hashStr(h, weather.windDir);
  h = hashInt(h, weather.hasToday ? weather.hi : -1000);
  h = hashInt(h, weather.hasToday ? weather.lo : -1000);
  return h;
}

static uint32_t sigWxFc() {
  uint32_t h = hashInit();
  for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
    const WeatherDay &d = weather.days[i];
    h = hashInt(h, d.valid ? 1 : 0);
    if (!d.valid)
      continue;
    h = hashStr(h, d.label);
    h = hashInt(h, (int)d.icon);
    h = hashInt(h, d.hi);
    h = hashInt(h, d.lo);
  }
  return h;
}

static uint32_t sigWxFoot() {
  uint32_t h = hashInit();
  h = hashInt(h, (int)current.status);
  h = hashInt(h, current.session.valid ? current.session.usedPct : -1);
  h = hashInt(h, current.week.valid ? current.week.usedPct : -1);
  // Minute resolution: the footer only prints HH:MM, and fetchedAt only moves
  // every 20 minutes anyway.
  h = hashInt(h, (int32_t)(weather.fetchedAt / 60));
  // Hashed as the rendered boolean rather than the age, so it flips the
  // signature exactly once, when the label changes.
  h = hashInt(h, weatherStale() ? 1 : 0);
  return h;
}

static void refreshSignatures() {
  zones[Z_HEADER].wantSig = sigHeader();
  zones[Z_STATUS].wantSig = sigStatus();
  zones[Z_GAUGE].wantSig = sigGauge();
  zones[Z_FOOTER].wantSig = sigFooter();
  zones[Z_WX_NOW].wantSig = sigWxNow();
  zones[Z_WX_FC].wantSig = sigWxFc();
  zones[Z_WX_FOOT].wantSig = sigWxFoot();
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------
static void drawFull() {
  const bool wx = weatherScreenActive();

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    for (int i = 0; i < ZONE_COUNT; i++)
      if (zoneActive((ZoneId)i, wx))
        paintZone((ZoneId)i);
  } while (display.nextPage());

  // Only the zones actually painted count as drawn. Leaving the other screen's
  // zones stale is what makes them repaint on the way back.
  uint32_t now = millis();
  for (int i = 0; i < ZONE_COUNT; i++) {
    if (!zoneActive((ZoneId)i, wx))
      continue;
    zones[i].drawnSig = zones[i].wantSig;
    zones[i].lastDraw = now;
  }
  lastFullRefresh = now;
  partialsSinceFull = 0;
  fullRefreshPending = false;
}

static void drawZonePartial(ZoneId id) {
  Zone &z = zones[id];
  display.setPartialWindow(0, z.y, SCREEN_W, z.h);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    paintZone(id);
  } while (display.nextPage());

  z.drawnSig = z.wantSig;
  z.lastDraw = millis();
  partialsSinceFull++;
}

void uiBegin() {
  SPI.end();
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(115200, true, 2, false);
  display.setRotation(0);
  fullRefreshPending = true;
}

void uiSetState(const ClaudeState &s) {
  current = s;
  haveState = true;
  showBanner = false;
}

void uiSetWeather(const WeatherData &w) { weather = w; }

void uiSetBanner(const char *line1, const char *line2) {
  // Once real state is on screen, connection chatter shouldn't replace it.
  if (haveState)
    return;
  strncpy(bannerLine1, line1 ? line1 : "", sizeof(bannerLine1) - 1);
  bannerLine1[sizeof(bannerLine1) - 1] = '\0';
  strncpy(bannerLine2, line2 ? line2 : "", sizeof(bannerLine2) - 1);
  bannerLine2[sizeof(bannerLine2) - 1] = '\0';
  showBanner = true;
}

void uiRequestFullRefresh() { fullRefreshPending = true; }

void uiTick() {
  refreshSignatures();
  uint32_t now = millis();

  // Swapping screens replaces every pixel below the header, so it gets a full
  // refresh rather than a run of partials - which is also the cheapest way to
  // clear the ghost of the layout being replaced.
  const bool wx = weatherScreenActive();
  if (wx != wxOnGlass) {
    wxOnGlass = wx;
    fullRefreshPending = true;
  }

  bool anyDirty = false;
  for (int i = 0; i < ZONE_COUNT; i++) {
    if (!zoneActive((ZoneId)i, wx))
      continue;
    if (zones[i].wantSig != zones[i].drawnSig) {
      anyDirty = true;
      break;
    }
  }

  // A full refresh is worth its ~3 s only when there is something new to show,
  // or when enough partials have stacked up to leave visible ghosting.
  bool fullDue = fullRefreshPending ||
                 (anyDirty && (now - lastFullRefresh) > FULL_REFRESH_INTERVAL_MS) ||
                 partialsSinceFull >= PARTIALS_BEFORE_FULL;

  if (fullDue && (anyDirty || fullRefreshPending)) {
    drawFull();
    return;
  }

  // Otherwise repaint at most one zone per tick, so a burst of changes doesn't
  // chain several refreshes back to back.
  for (int i = 0; i < ZONE_COUNT; i++) {
    Zone &z = zones[i];
    if (!zoneActive((ZoneId)i, wx))
      continue;
    if (z.wantSig == z.drawnSig)
      continue;
    if ((now - z.lastDraw) < z.interval)
      continue;
    drawZonePartial((ZoneId)i);
    return;
  }
}
