#pragma once

// ===========================================================================
// Panel controller
//
// Waveshare has shipped the 4.2" 400x300 B/W in two flavours, and GxEPD2 needs
// the matching driver class. "rev2.1" on the silkscreen is the *driver board*
// revision, not the panel, so it doesn't tell you which one you have.
//
// Start with UC8176. If the panel test is blank, heavily ghosted, or torn,
// comment it out and use SSD1683 instead.
// ===========================================================================
#define EPD_PANEL_UC8176 // GDEW042T2, UC8176 (IL0398)
// #define EPD_PANEL_SSD1683    // GDEY042T81, SSD1683 ("V2" panels)

// ===========================================================================
// Pins - ESP32-S3-N16R8
//
// Avoids GPIO 26-37 (16 MB flash + 8 MB octal PSRAM) and the strapping / USB
// pins 0, 3, 19, 20, 45, 46.
// ===========================================================================
#define EPD_MOSI 11 // module DIN
#define EPD_SCK 12  // module CLK
#define EPD_CS 10
#define EPD_DC 9
#define EPD_RST 8
#define EPD_BUSY 7

// ===========================================================================
// MQTT topics
// ===========================================================================
#define TOPIC_STATE "claude/display/state"   // retained snapshot from bridge
#define TOPIC_BRIDGE "claude/display/bridge" // bridge LWT: "online" / "offline"
#define TOPIC_DEVICE "claude/display/device" // our own LWT

// ===========================================================================
// Refresh policy
//
// These panels are rated for roughly one refresh per 180 s, but the bridge
// pushes state on nearly every message. So each screen zone tracks its own
// content signature and a minimum repaint interval: the status banner is the
// actionable signal and repaints quickly, the gauges lazily. A periodic full
// refresh clears the ghosting that accumulates from partial updates.
// ===========================================================================
#define STATUS_MIN_INTERVAL_MS 3000UL   // status banner: near-immediate
#define GAUGE_MIN_INTERVAL_MS 60000UL   // usage bars: slow
#define FOOTER_MIN_INTERVAL_MS 60000UL  // footer + header clock
#define FULL_REFRESH_INTERVAL_MS 1800000UL // 30 min de-ghost
// Ghosting became visible on changing digits at 20. Lower this if you still see
// old values behind new ones; raise it if the full-refresh flash bothers you
// more than the ghosting does.
#define PARTIALS_BEFORE_FULL 10

// Bridge silence after which we assume nothing is running.
#define BRIDGE_STALE_MS 900000UL // 15 min

// ===========================================================================
// Layout - 400 x 300, origin top-left
// ===========================================================================
#define SCREEN_W 400
#define SCREEN_H 300

#define Z_HEADER_Y 0
#define Z_HEADER_H 26

#define Z_STATUS_Y 28
#define Z_STATUS_H 86

#define Z_GAUGE_Y 116
#define Z_GAUGE_H 132

#define Z_FOOTER_Y 250
#define Z_FOOTER_H 50

#define MARGIN 14
#define BAR_H 18

// ===========================================================================
// Clock - POSIX TZ string, US Central with DST.
//
// The trailing rules are the DST changeover: M3.2.0 is the second Sunday in
// March, M11.1.0 the first Sunday in November. Keep them - without the rules
// the clock is an hour out for half the year.
//
// Other zones: https://github.com/nayarsystems/posix_tz_db
//   Eastern  EST5EDT,M3.2.0,M11.1.0
//   Mountain MST7MDT,M3.2.0,M11.1.0
//   Pacific  PST8PDT,M3.2.0,M11.1.0
//   UK       GMT0BST,M3.5.0/1,M10.5.0
// ===========================================================================
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.nist.gov"
#define TZ_POSIX "CST6CDT,M3.2.0,M11.1.0"
