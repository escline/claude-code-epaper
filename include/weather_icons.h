#pragma once

#include "weather.h"
#include <Adafruit_GFX.h>

// ---------------------------------------------------------------------------
// Weather glyphs, drawn from primitives rather than stored as bitmaps.
//
// The panel needs the same eight symbols at two sizes (64 px beside the current
// conditions, 32 px in the forecast columns). As bitmaps that would be sixteen
// blobs and a generator script to keep them in step; as geometry it is one
// definition scaled by a parameter, and the output on a 1-bit panel is
// identical. Proportions are all fractions of the box, so a third size costs
// nothing.
//
// Clouds are outlined rather than solid: at 64 px a filled cloud is a heavy
// black mass, and outlining also lets the sun behind a partly-cloudy glyph be
// occluded correctly - the cloud's white interior does the erasing.
// ---------------------------------------------------------------------------

// Draws into the size x size box whose top-left corner is (x, y). `bg` must
// match what the zone was cleared to, since some glyphs paint over themselves.
void wxDrawIcon(Adafruit_GFX &g, WxIcon icon, int16_t x, int16_t y,
                int16_t size, uint16_t fg, uint16_t bg);
