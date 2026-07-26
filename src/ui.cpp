#include "ui.h"
#include "config.h"
#include "epd_panel.h"

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
// ---------------------------------------------------------------------------
enum ZoneId { Z_HEADER, Z_STATUS, Z_GAUGE, Z_FOOTER, ZONE_COUNT };

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
};

static ClaudeState current;
static bool haveState = false;
static char bannerLine1[32] = "";
static char bannerLine2[48] = "";
static bool showBanner = true;

static bool fullRefreshPending = true;
static uint32_t lastFullRefresh = 0;
static uint16_t partialsSinceFull = 0;

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
    snprintf(buf, cap, "resets in %ldh %ldm", h, m);
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

// Reset countdowns are hashed at minute resolution so the zone doesn't go
// dirty every second, but still ticks down visibly.
static uint32_t sigGauge() {
  time_t now = time(nullptr);
  uint32_t h = hashInit();
  h = hashInt(h, current.session.valid ? current.session.usedPct : -1);
  h = hashInt(h, current.week.valid ? current.week.usedPct : -1);
  h = hashInt(h, current.session.resetsAt ? (current.session.resetsAt - now) / 60
                                          : -1);
  h = hashInt(h, current.week.resetsAt ? (current.week.resetsAt - now) / 60 : -1);
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

static void refreshSignatures() {
  zones[Z_HEADER].wantSig = sigHeader();
  zones[Z_STATUS].wantSig = sigStatus();
  zones[Z_GAUGE].wantSig = sigGauge();
  zones[Z_FOOTER].wantSig = sigFooter();
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------
static void drawFull() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    for (int i = 0; i < ZONE_COUNT; i++)
      paintZone((ZoneId)i);
  } while (display.nextPage());

  uint32_t now = millis();
  for (int i = 0; i < ZONE_COUNT; i++) {
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

  bool anyDirty = false;
  for (int i = 0; i < ZONE_COUNT; i++) {
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
    if (z.wantSig == z.drawnSig)
      continue;
    if ((now - z.lastDraw) < z.interval)
      continue;
    drawZonePartial((ZoneId)i);
    return;
  }
}
