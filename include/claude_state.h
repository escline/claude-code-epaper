#pragma once

#include <Arduino.h>

enum class ClaudeStatus : uint8_t {
  Unknown, // nothing received yet
  Offline, // bridge gone, or no session running
  Idle,    // session open, waiting on you
  Working, // actively processing
  NeedsYou // blocked on a permission prompt or question
};

// One rate-limit window (5-hour or 7-day).
struct Gauge {
  bool valid = false;
  int usedPct = 0;
  long resetsAt = 0; // unix epoch seconds, 0 if unknown
};

struct ClaudeState {
  ClaudeStatus status = ClaudeStatus::Unknown;
  char detail[64] = "";  // short line under the banner
  char model[24] = "";   // e.g. "Opus 5"
  char project[32] = ""; // basename of the workspace dir

  Gauge session; // rate_limits.five_hour
  Gauge week;    // rate_limits.seven_day

  bool hasContext = false;
  int contextPct = 0; // context window used, 0-100

  bool hasCost = false;
  float costUsd = 0.0f;

  // How many Claude Code sessions the bridge can currently see. IDLE with zero
  // sessions means "nothing is open" and hands the panel to the weather
  // screen; IDLE with one means "open, waiting on you" and must not. Absent
  // from older bridge builds, hence the flag.
  bool hasSessions = false;
  int sessions = 0;

  long ts = 0; // bridge clock at send time, unix epoch seconds
};

// Parse the retained JSON snapshot published on TOPIC_STATE.
// Returns false and leaves `out` untouched if the payload is not valid JSON.
bool parseClaudeState(const char *json, size_t len, ClaudeState &out);

const char *statusLabel(ClaudeStatus s);
