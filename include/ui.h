#pragma once

#include "claude_state.h"

// Bring up SPI and the panel, and paint the splash screen.
void uiBegin();

// Hand the renderer the latest state. Cheap to call often: it diffs each zone
// against what is actually on the glass and repaints only what changed, no
// faster than that zone's minimum interval.
void uiSetState(const ClaudeState &s);

// Show a connection banner before any state has arrived.
void uiSetBanner(const char *line1, const char *line2);

// Call from loop(). Performs any repaint that is due.
void uiTick();

// Force a full de-ghosting refresh on the next tick.
void uiRequestFullRefresh();
