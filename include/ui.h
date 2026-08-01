#pragma once

#include "claude_state.h"
#include "weather.h"

// Bring up SPI and the panel, and paint the splash screen.
void uiBegin();

// Hand the renderer the latest state. Cheap to call often: it diffs each zone
// against what is actually on the glass and repaints only what changed, no
// faster than that zone's minimum interval.
void uiSetState(const ClaudeState &s);

// Hand the renderer the latest forecast. The weather screen replaces the
// status screen on its own, whenever the state says nothing is running - the
// caller only has to keep this fed.
void uiSetWeather(const WeatherData &w);

// Show a connection banner before any state has arrived. While it is up the
// panel shows the banner and nothing else: the gauges and the footer have
// nothing to report yet, and empty bars read as data rather than as absence.
void uiSetBanner(const char *line1, const char *line2);

// Call from loop(). Performs any repaint that is due.
void uiTick();

// Force a full de-ghosting refresh on the next tick.
void uiRequestFullRefresh();
