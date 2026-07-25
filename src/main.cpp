// Bring-up test for a Waveshare 4.2" e-Paper Module (rev2.1, 400x300 B/W)
// on an ESP32-S3-N16R8, using GxEPD2 + Adafruit_GFX.
//
// Sequence on boot: full-refresh test pattern -> five partial-refresh updates
// -> hibernate. Watch the serial monitor at 115200 for progress.
//
// ---------------------------------------------------------------------------
// Wiring
//
//   Module   ESP32-S3    Notes
//   ------   --------    -------------------------------------------------
//   VCC      3V3         rev2.1 has level shifting, but 3.3V is correct here
//   GND      GND
//   DIN      GPIO 11     SPI MOSI
//   CLK      GPIO 12     SPI SCK
//   CS       GPIO 10     chip select, active low
//   DC       GPIO 9      data / command
//   RST      GPIO 8      reset, active low
//   BUSY     GPIO 7      busy, driven by the panel
//
// These avoid GPIO 26-37 (the N16R8 uses those for its 16 MB flash and 8 MB
// octal PSRAM) and the strapping / USB pins 0, 3, 19, 20, 45, 46.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMonoBold18pt7b.h>

#define EPD_MOSI 11
#define EPD_SCK  12
#define EPD_CS   10
#define EPD_DC    9
#define EPD_RST   8
#define EPD_BUSY  7

// Which controller is on the panel. Set via -D flags in platformio.ini: if the
// pattern below comes out blank, ghosted or scrambled, switch to the other one.
#if defined(EPD_PANEL_SSD1683)
using EpdPanel = GxEPD2_420_GDEY042T81;
static const char PANEL_NAME[] = "GDEY042T81 / SSD1683";
#elif defined(EPD_PANEL_UC8176)
using EpdPanel = GxEPD2_420;
static const char PANEL_NAME[] = "GDEW042T2 / UC8176";
#else
#error "Define EPD_PANEL_UC8176 or EPD_PANEL_SSD1683 in platformio.ini"
#endif

// Page buffer holds the whole 400x300 frame (15 kB) — ample on an S3.
GxEPD2_BW<EpdPanel, EpdPanel::HEIGHT> display(
    EpdPanel(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// Draw `text` horizontally centred, with `y` as the text baseline.
static void printCentered(const char *text, int16_t y)
{
  int16_t bx, by;
  uint16_t bw, bh;
  display.getTextBounds(text, 0, y, &bx, &by, &bw, &bh);
  display.setCursor((display.width() - bw) / 2 - bx, y);
  display.print(text);
}

// Full-panel pattern. Every element is a check: the nested borders and corner
// blocks prove the controller addresses all four edges, and the checkerboard
// plus line ramp expose byte-order or line-pitch mismatches from a wrong panel
// class — those show up as smearing or diagonal tearing rather than clean squares.
static void drawTestPattern()
{
  display.setFullWindow();
  display.firstPage();
  do
  {
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

    // Checkerboard of 16px cells.
    const int16_t bx = 44, by = 132, bs = 16;
    for (int16_t row = 0; row < 4; row++)
    {
      for (int16_t col = 0; col < 9; col++)
      {
        if ((row + col) % 2 == 0)
        {
          display.fillRect(bx + col * bs, by + row * bs, bs, bs, GxEPD_BLACK);
        }
      }
    }

    // Vertical lines at widening pitch, to spot dropped columns.
    int16_t x = w / 2 + 30;
    for (int16_t gap = 1; gap <= 8 && x < w - 50; gap++)
    {
      display.fillRect(x, by, gap, bs * 4, GxEPD_BLACK);
      x += gap * 2 + 2;
    }

    display.setFont(&FreeMonoBold9pt7b);
    printCentered("partial refresh test below", 236);
  }
  while (display.nextPage());
}

// Redraw only the counter box. On a healthy panel this is fast and leaves the
// rest of the image untouched.
static void drawPartialCounter(int n)
{
  const int16_t bw = 200, bh = 34;
  const int16_t bx = (display.width() - bw) / 2;
  const int16_t by = 246;

  display.setPartialWindow(bx, by, bw, bh);
  display.firstPage();
  do
  {
    display.fillScreen(GxEPD_WHITE);
    display.drawRect(bx, by, bw, bh, GxEPD_BLACK);

    char line[32];
    snprintf(line, sizeof(line), "update %d of 5", n);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);
    printCentered(line, by + 22);
  }
  while (display.nextPage());
}

void setup()
{
  Serial.begin(115200);
  delay(2000); // let the USB CDC port enumerate before the first print
  Serial.println("\n=== Waveshare 4.2\" e-Paper bring-up ===");
  Serial.printf("Panel class: %s\n", PANEL_NAME);

  // Remap the ESP32-S3's FSPI bus onto our pins. MISO is unused: the panel is
  // write-only, so pass -1.
  SPI.end();
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);

  // (diag baud, initial reset, reset pulse ms, pulldown-first reset)
  display.init(115200, true, 2, false);
  Serial.printf("Init done: %d x %d\n", display.width(), display.height());

  uint32_t t0 = millis();
  drawTestPattern();
  Serial.printf("Full refresh: %lu ms\n", millis() - t0);

  if (display.epd2.hasPartialUpdate)
  {
    for (int i = 1; i <= 5; i++)
    {
      t0 = millis();
      drawPartialCounter(i);
      Serial.printf("Partial refresh %d: %lu ms\n", i, millis() - t0);
      delay(1000);
    }
  }
  else
  {
    Serial.println("Panel reports no partial update support; skipping.");
  }

  // Leaving an e-paper controller powered with a charged panel causes ghosting
  // and shortens panel life. Hibernate draws ~1uA; the image persists.
  display.hibernate();
  Serial.println("Hibernating. Image should remain on screen.");
}

void loop()
{
  delay(10000);
}
