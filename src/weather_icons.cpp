#include "weather_icons.h"

#include <math.h>

// All geometry below is expressed as a fraction of the icon box, so the same
// code produces the 64 px and 32 px versions. `SCALE(v)` turns a fraction into
// pixels; rounding to int is deliberate and happens once per coordinate.
//
// Not named F(): Arduino's WString.h already defines that as the flash-string
// macro, and shadowing it here would break any F("...") in a file that later
// includes this one.
#define SCALE(frac) ((int16_t)lroundf((frac) * (float)size))

// Stroke weight. Two pixels at 64 px, one at 32 - a 2 px outline on a 32 px
// glyph closes up the gaps in a cloud and it reads as a blob.
static int16_t stroke(int16_t size) { return size >= 48 ? 2 : 1; }

// Adafruit_GFX has no thick-line primitive. Offsetting perpendicular to the
// run is close enough at these lengths and avoids a polygon fill.
static void thickLine(Adafruit_GFX &g, int16_t x0, int16_t y0, int16_t x1,
                      int16_t y1, int16_t w, uint16_t color) {
  for (int16_t i = 0; i < w; i++) {
    // Offset across the shallower axis so the widening is visible.
    if (abs(x1 - x0) > abs(y1 - y0))
      g.drawLine(x0, y0 + i, x1, y1 + i, color);
    else
      g.drawLine(x0 + i, y0, x1 + i, y1, color);
  }
}

// ---------------------------------------------------------------------------
// Cloud: the union of three discs and a rectangle, filled solid. Called twice
// (once at full size in the foreground colour, once inset in the background)
// to leave an outline of `inset` pixels.
// ---------------------------------------------------------------------------
static void cloudFill(Adafruit_GFX &g, int16_t x, int16_t y, int16_t size,
                      int16_t inset, uint16_t color) {
  const int16_t r1 = SCALE(0.22f) - inset;
  const int16_t r2 = SCALE(0.28f) - inset;
  const int16_t r3 = SCALE(0.20f) - inset;
  if (r1 <= 0 || r2 <= 0 || r3 <= 0)
    return;

  g.fillCircle(x + SCALE(0.30f), y + SCALE(0.58f), r1, color);
  g.fillCircle(x + SCALE(0.50f), y + SCALE(0.46f), r2, color);
  g.fillCircle(x + SCALE(0.72f), y + SCALE(0.58f), r3, color);

  const int16_t rx = x + SCALE(0.10f) + inset;
  const int16_t rw = SCALE(0.80f) - 2 * inset;
  const int16_t ry = y + SCALE(0.52f);
  const int16_t rh = SCALE(0.26f) - inset;
  if (rw > 0 && rh > 0)
    g.fillRect(rx, ry, rw, rh, color);
}

static void drawCloud(Adafruit_GFX &g, int16_t x, int16_t y, int16_t size,
                      uint16_t fg, uint16_t bg) {
  cloudFill(g, x, y, size, 0, fg);
  cloudFill(g, x, y, size, stroke(size), bg);
}

// ---------------------------------------------------------------------------
// Sun: solid disc plus eight rays. `scale` shrinks it for the partly-cloudy
// glyph, where the cloud needs room.
// ---------------------------------------------------------------------------
static void drawSun(Adafruit_GFX &g, int16_t x, int16_t y, int16_t size,
                    float cxF, float cyF, float rF, uint16_t fg) {
  const int16_t cx = x + SCALE(cxF);
  const int16_t cy = y + SCALE(cyF);
  const int16_t r = SCALE(rF);
  const int16_t w = stroke(size);

  g.fillCircle(cx, cy, r, fg);

  for (int i = 0; i < 8; i++) {
    float a = (float)i * (float)M_PI / 4.0f;
    int16_t x0 = cx + (int16_t)lroundf(cosf(a) * (float)r * 1.45f);
    int16_t y0 = cy + (int16_t)lroundf(sinf(a) * (float)r * 1.45f);
    int16_t x1 = cx + (int16_t)lroundf(cosf(a) * (float)r * 2.05f);
    int16_t y1 = cy + (int16_t)lroundf(sinf(a) * (float)r * 2.05f);
    thickLine(g, x0, y0, x1, y1, w, fg);
  }
}

// Slanted precipitation strokes under a cloud. `count` streaks spread across
// the middle of the box; `lenF` sets how far they fall.
static void drawStreaks(Adafruit_GFX &g, int16_t x, int16_t y, int16_t size,
                        int count, float lenF, uint16_t fg) {
  const int16_t w = stroke(size);
  const int16_t top = y + SCALE(0.80f);
  for (int i = 0; i < count; i++) {
    float fx = 0.28f + 0.22f * (float)i;
    int16_t sx = x + SCALE(fx);
    thickLine(g, sx, top, sx - SCALE(0.06f), top + SCALE(lenF), w, fg);
  }
}

static void drawSnowflakes(Adafruit_GFX &g, int16_t x, int16_t y, int16_t size,
                           uint16_t fg) {
  const int16_t r = SCALE(0.07f);
  for (int i = 0; i < 3; i++) {
    int16_t cx = x + SCALE(0.30f + 0.20f * (float)i);
    int16_t cy = y + SCALE(0.88f);
    // Three crossing strokes read as a flake; a plus sign reads as a cross.
    for (int k = 0; k < 3; k++) {
      float a = (float)k * (float)M_PI / 3.0f;
      int16_t dx = (int16_t)lroundf(cosf(a) * (float)r);
      int16_t dy = (int16_t)lroundf(sinf(a) * (float)r);
      g.drawLine(cx - dx, cy - dy, cx + dx, cy + dy, fg);
    }
  }
}

static void drawBolt(Adafruit_GFX &g, int16_t x, int16_t y, int16_t size,
                     uint16_t fg) {
  // Two triangles overlapped into a zigzag. Cheaper and crisper than stroking
  // a six-point polygon at this scale.
  g.fillTriangle(x + SCALE(0.58f), y + SCALE(0.74f), x + SCALE(0.30f), y + SCALE(0.95f),
                 x + SCALE(0.54f), y + SCALE(0.90f), fg);
  g.fillTriangle(x + SCALE(0.66f), y + SCALE(0.84f), x + SCALE(0.40f), y + SCALE(1.00f),
                 x + SCALE(0.52f), y + SCALE(0.86f), fg);
}

static void drawFogBars(Adafruit_GFX &g, int16_t x, int16_t y, int16_t size,
                        uint16_t fg) {
  const int16_t w = stroke(size);
  // Staggered ends suggest drifting fog rather than a stack of underlines.
  const float insets[3] = {0.14f, 0.24f, 0.18f};
  for (int i = 0; i < 3; i++) {
    int16_t ly = y + SCALE(0.80f + 0.08f * (float)i);
    thickLine(g, x + SCALE(insets[i]), ly, x + SCALE(1.0f - insets[i]), ly, w, fg);
  }
}

void wxDrawIcon(Adafruit_GFX &g, WxIcon icon, int16_t x, int16_t y,
                int16_t size, uint16_t fg, uint16_t bg) {
  switch (icon) {
  case WxIcon::Clear:
    drawSun(g, x, y, size, 0.50f, 0.50f, 0.20f, fg);
    break;

  case WxIcon::PartlyCloudy:
    // Sun first: the cloud's white interior then occludes it, which is what
    // makes the two shapes read as overlapping rather than adjacent.
    drawSun(g, x, y, size, 0.68f, 0.28f, 0.14f, fg);
    drawCloud(g, x, y, size, fg, bg);
    break;

  case WxIcon::Overcast:
    drawCloud(g, x, y, size, fg, bg);
    break;

  case WxIcon::Fog:
    drawCloud(g, x, y, size, fg, bg);
    drawFogBars(g, x, y, size, fg);
    break;

  case WxIcon::Drizzle:
    drawCloud(g, x, y, size, fg, bg);
    drawStreaks(g, x, y, size, 3, 0.08f, fg);
    break;

  case WxIcon::Rain:
    drawCloud(g, x, y, size, fg, bg);
    drawStreaks(g, x, y, size, 3, 0.17f, fg);
    break;

  case WxIcon::Snow:
    drawCloud(g, x, y, size, fg, bg);
    drawSnowflakes(g, x, y, size, fg);
    break;

  case WxIcon::Storm:
    drawCloud(g, x, y, size, fg, bg);
    drawBolt(g, x, y, size, fg);
    break;

  case WxIcon::Unknown:
  default:
    // Nothing. The condition text carries the meaning, and a guessed glyph
    // would be worse than an empty box.
    break;
  }
}
