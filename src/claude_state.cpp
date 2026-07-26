#include "claude_state.h"
#include <ArduinoJson.h>

static void copyStr(char *dst, size_t cap, const char *src) {
  if (!src) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

static ClaudeStatus statusFromString(const char *s) {
  if (!s)
    return ClaudeStatus::Unknown;
  if (!strcmp(s, "working"))
    return ClaudeStatus::Working;
  if (!strcmp(s, "needs_you"))
    return ClaudeStatus::NeedsYou;
  if (!strcmp(s, "idle"))
    return ClaudeStatus::Idle;
  if (!strcmp(s, "offline"))
    return ClaudeStatus::Offline;
  return ClaudeStatus::Unknown;
}

static void readGauge(JsonObjectConst obj, Gauge &g) {
  if (obj.isNull())
    return;
  // The bridge omits these when Claude Code hasn't reported them yet, so a
  // missing key means "unknown", not zero.
  if (!obj["used_pct"].isNull()) {
    g.usedPct = constrain(obj["used_pct"].as<int>(), 0, 100);
    g.valid = true;
  }
  if (!obj["resets_at"].isNull()) {
    g.resetsAt = obj["resets_at"].as<long>();
  }
}

bool parseClaudeState(const char *json, size_t len, ClaudeState &out) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len) != DeserializationError::Ok) {
    return false;
  }

  ClaudeState s;
  s.status = statusFromString(doc["status"].as<const char *>());
  copyStr(s.detail, sizeof(s.detail), doc["detail"].as<const char *>());
  copyStr(s.model, sizeof(s.model), doc["model"].as<const char *>());
  copyStr(s.project, sizeof(s.project), doc["project"].as<const char *>());

  readGauge(doc["session"].as<JsonObjectConst>(), s.session);
  readGauge(doc["week"].as<JsonObjectConst>(), s.week);

  if (!doc["context_pct"].isNull()) {
    s.hasContext = true;
    s.contextPct = constrain(doc["context_pct"].as<int>(), 0, 100);
  }
  if (!doc["cost_usd"].isNull()) {
    s.hasCost = true;
    s.costUsd = doc["cost_usd"].as<float>();
  }
  if (!doc["sessions"].isNull()) {
    s.hasSessions = true;
    s.sessions = doc["sessions"].as<int>();
  }
  s.ts = doc["ts"].as<long>();

  out = s;
  return true;
}

const char *statusLabel(ClaudeStatus s) {
  switch (s) {
  case ClaudeStatus::Working:
    return "WORKING";
  case ClaudeStatus::NeedsYou:
    return "NEEDS YOU";
  case ClaudeStatus::Idle:
    return "IDLE";
  case ClaudeStatus::Offline:
    return "OFFLINE";
  default:
    return "...";
  }
}
