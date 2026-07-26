// Standalone panel bring-up test. Build with:  pio run -e paneltest -t upload
//
// Full-refresh test pattern -> five partial refreshes -> hibernate. Use this to
// confirm wiring and to work out which panel controller you have before
// bothering with WiFi or MQTT.

#include "config.h"
#include "epd_panel.h"

#include <Arduino.h>
#include <Fonts/FreeMonoBold18pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <SPI.h>

static EpdDisplay display(EpdPanel(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

static void printCentered(const char *text, int16_t y) {
  int16_t bx, by;
  uint16_t bw, bh;
  display.getTextBounds(text, 0, y, &bx, &by, &bw, &bh);
  display.setCursor((display.width() - bw) / 2 - bx, y);
  display.print(text);
}

// Every element is a check: the nested borders and corner blocks prove the
// controller addresses all four edges, and the checkerboard plus line ramp
// expose byte-order or line-pitch mismatches from a wrong panel class - those
// show up as smearing or tearing rather than clean squares.
static void drawTestPattern() {
  display.setFullWindow();
  display.firstPage();
  do {
    const int16_t w = display.width();
    const int16_t h = display.height();

    display.fillScreen(GxEPD_WHITE);
    display.drawRect(0, 0, w, h, GxEPD_BLACK);
    display.drawRect(3, 3, w - 6, h - 6, GxEPD_BLACK);

    const int16_t c = 22;
    display.fillRect(8, 8, c, c, GxEPD_BLACK);
    display.fillRect(w - 8 - c, 8, c, c, GxEPD_BLACK);
    display.fillRect(8, h - 8 - c, c, c, GxEPD_BLACK);
    display.fillRect(w - 8 - c, h - 8 - c, c, c, GxEPD_BLACK);

    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeMonoBold18pt7b);
    printCentered("ESP32-S3 e-Paper", 66);

    display.setFont(&FreeMonoBold9pt7b);
    printCentered(PANEL_NAME, 92);

    char line[48];
    snprintf(line, sizeof(line), "%d x %d  full refresh", w, h);
    printCentered(line, 112);

    const int16_t bx = 44, by = 132, bs = 16;
    for (int16_t row = 0; row < 4; row++) {
      for (int16_t col = 0; col < 9; col++) {
        if ((row + col) % 2 == 0) {
          display.fillRect(bx + col * bs, by + row * bs, bs, bs, GxEPD_BLACK);
        }
      }
    }

    int16_t x = w / 2 + 30;
    for (int16_t gap = 1; gap <= 8 && x < w - 50; gap++) {
      display.fillRect(x, by, gap, bs * 4, GxEPD_BLACK);
      x += gap * 2 + 2;
    }

    printCentered("partial refresh test below", 236);
  } while (display.nextPage());
}

static void drawPartialCounter(int n) {
  const int16_t bw = 200, bh = 34;
  const int16_t bx = (display.width() - bw) / 2;
  const int16_t by = 246;

  display.setPartialWindow(bx, by, bw, bh);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawRect(bx, by, bw, bh, GxEPD_BLACK);

    char line[32];
    snprintf(line, sizeof(line), "update %d of 5", n);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);
    printCentered(line, by + 22);
  } while (display.nextPage());
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== Waveshare 4.2\" e-Paper bring-up ===");
  Serial.printf("Panel class: %s\n", PANEL_NAME);

  SPI.end();
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);

  display.init(115200, true, 2, false);
  Serial.printf("Init done: %d x %d\n", display.width(), display.height());

  uint32_t t0 = millis();
  drawTestPattern();
  Serial.printf("Full refresh: %lu ms\n", millis() - t0);

  if (display.epd2.hasPartialUpdate) {
    for (int i = 1; i <= 5; i++) {
      t0 = millis();
      drawPartialCounter(i);
      Serial.printf("Partial refresh %d: %lu ms\n", i, millis() - t0);
      delay(1000);
    }
  } else {
    Serial.println("Panel reports no partial update support; skipping.");
  }

  display.hibernate();
  Serial.println("Hibernating. Image should remain on screen.");
}

void loop() { delay(10000); }
