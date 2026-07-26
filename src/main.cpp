// Claude Code status + usage display for a Waveshare 4.2" e-Paper panel on an
// ESP32-S3-N16R8.
//
// Subscribes to a retained MQTT topic published by the Node bridge in bridge/,
// which merges Claude Code statusline data (5h / 7d rate limits, cost, context)
// with hook events (working / needs-you / idle) into a single snapshot.
//
// Retained + last-will means a reboot repaints the correct state within a
// second of reconnecting, and a dead bridge shows as OFFLINE rather than a
// stale screen.
//
// See README.md for wiring.

#include "claude_state.h"
#include "config.h"
#include "ui.h"

#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <time.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy include/secrets.h.example to include/secrets.h and fill it in"
#endif

static WiFiClient net;
static PubSubClient mqtt(net);

static uint32_t lastStateMs = 0;
static uint32_t lastReconnectAttempt = 0;
static uint32_t reconnectBackoff = 1000;
static bool bridgeOnline = true;

static void onMessage(char *topic, byte *payload, unsigned int len) {
  if (!strcmp(topic, TOPIC_BRIDGE)) {
    bridgeOnline = (len >= 6 && !strncmp((const char *)payload, "online", 6));
    Serial.printf("[mqtt] bridge %s\n", bridgeOnline ? "online" : "offline");
    if (!bridgeOnline) {
      ClaudeState s;
      s.status = ClaudeStatus::Offline;
      strncpy(s.detail, "bridge not running", sizeof(s.detail) - 1);
      uiSetState(s);
    }
    return;
  }

  if (strcmp(topic, TOPIC_STATE))
    return;

  ClaudeState s;
  if (!parseClaudeState((const char *)payload, len, s)) {
    Serial.println("[mqtt] state payload was not valid JSON, ignoring");
    return;
  }
  lastStateMs = millis();
  uiSetState(s);
  Serial.printf("[mqtt] state: %s  5h=%d%%  7d=%d%%\n", statusLabel(s.status),
                s.session.valid ? s.session.usedPct : -1,
                s.week.valid ? s.week.usedPct : -1);
}

static void connectWiFi() {
  Serial.printf("[wifi] connecting to %s\n", WIFI_SSID);
  uiSetBanner("Connecting", WIFI_SSID);
  uiTick();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // keep MQTT push latency low
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[wifi] connected, ip=%s rssi=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println("[wifi] failed; will keep retrying in the background");
    uiSetBanner("No WiFi", WIFI_SSID);
  }
}

static bool connectMqtt() {
  Serial.printf("[mqtt] connecting to %s:%d\n", MQTT_HOST, MQTT_PORT);

  const char *user = strlen(MQTT_USER) ? MQTT_USER : nullptr;
  const char *pass = strlen(MQTT_PASSWORD) ? MQTT_PASSWORD : nullptr;

  // Our own last will, so anything else on the bus can see the panel drop off.
  bool ok = mqtt.connect(MQTT_CLIENT_ID, user, pass, TOPIC_DEVICE, 0, true,
                         "offline");
  if (!ok) {
    Serial.printf("[mqtt] connect failed, rc=%d\n", mqtt.state());
    return false;
  }

  mqtt.publish(TOPIC_DEVICE, "online", true);
  mqtt.subscribe(TOPIC_STATE);
  mqtt.subscribe(TOPIC_BRIDGE);
  Serial.println("[mqtt] connected and subscribed");

  // The retained snapshot arrives on its own; show something until it does.
  uiSetBanner("Waiting", "for Claude Code");
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(2000); // let the S3's USB CDC port enumerate
  Serial.println("\n=== Claude Code e-Paper display ===");

  uiBegin();
  uiSetBanner("Starting", "");
  uiTick();

  connectWiFi();
  configTzTime(TZ_POSIX, NTP_SERVER_1, NTP_SERVER_2);

  // PubSubClient's default 256-byte buffer truncates the state snapshot.
  mqtt.setBufferSize(2048);
  mqtt.setKeepAlive(30);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(500);
    return;
  }

  if (!mqtt.connected()) {
    uint32_t now = millis();
    if (now - lastReconnectAttempt > reconnectBackoff) {
      lastReconnectAttempt = now;
      if (connectMqtt()) {
        reconnectBackoff = 1000;
      } else {
        reconnectBackoff = min<uint32_t>(reconnectBackoff * 2, 30000);
        uiSetBanner("No broker", MQTT_HOST);
      }
    }
  } else {
    mqtt.loop();
  }

  // Belt and braces alongside the bridge's last will: if the broker itself
  // vanishes we never receive the will, so fall back to a silence timer.
  if (lastStateMs && (millis() - lastStateMs) > BRIDGE_STALE_MS) {
    ClaudeState s;
    s.status = ClaudeStatus::Offline;
    strncpy(s.detail, "no updates", sizeof(s.detail) - 1);
    uiSetState(s);
    lastStateMs = 0;
  }

  uiTick();
  delay(100);
}
