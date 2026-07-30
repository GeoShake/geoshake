// GeoShake DIY — live dashboard ("showcase" build)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Same detector as geoshake_diy, different presentation: instead of scrolling
// log lines it paints a fixed dashboard with a live STA/LTA bar. Scrolling text
// is fine for debugging and terrible on camera — nothing appears to happen
// until suddenly everything does. A bar that breathes with the room reads
// instantly, which is what you want when someone knocks on the desk on video.
//
// Needs a terminal that understands ANSI escapes. macOS Terminal, iTerm2,
// Windows Terminal and most Linux terminals all do:
//     screen /dev/cu.usbmodemXXXX 115200
// The Arduino IDE's built-in serial monitor does NOT — it will show the escape
// codes as garbage. Use a real terminal for filming.

#include <SPI.h>
#include "stalta.h"
#include "golden.h"
#include "lsm6_spi.h"

using namespace geoshake;

#define PIN_SCLK 12
#define PIN_MOSI 11
#define PIN_MISO 13
static const int CS_PINS[] = {9, 10};
static const int SENSOR_COUNT = sizeof(CS_PINS) / sizeof(CS_PINS[0]);

// ANSI
#define CLS       "\033[2J"
#define HOME      "\033[H"
#define CLR_EOL   "\033[K"
#define RESET     "\033[0m"
#define BOLD      "\033[1m"
#define DIM       "\033[2m"
#define RED       "\033[31m"
#define GREEN     "\033[32m"
#define YELLOW    "\033[33m"
#define CYAN      "\033[36m"
#define WHITE     "\033[37m"
#define BG_RED    "\033[41m"

static const int  BAR_WIDTH = 34;
static const double BAR_MAX = 12.0;   // ratio at full bar; above this it pins
static const uint32_t FRAME_MS = 100; // 10 fps — smooth on camera
static const uint32_t BANNER_MS = 2500;

static Lsm6Spi sensors;
static StaltaState detector;

static uint32_t sampleCount = 0;
static uint32_t triggerCount = 0;
static uint32_t lastFrameMs = 0;
static uint32_t bannerUntilMs = 0;
static uint32_t rateWindowStart = 0;
static uint32_t rateWindowSamples = 0;
static double   measuredRate = 0.0;
static double   lastRatio = 0.0;
static double   lastPga = 0.0;

static void drawBar(double ratio) {
  const double frac = (ratio > BAR_MAX) ? 1.0 : (ratio / BAR_MAX);
  const int filled = (int)(frac * BAR_WIDTH + 0.5);
  const int threshCol = (int)((STALTA_PARAMS.trigger / BAR_MAX) * BAR_WIDTH + 0.5);

  const char *color = GREEN;
  if (ratio >= STALTA_PARAMS.trigger)      color = RED;
  else if (ratio >= STALTA_PARAMS.trigger * 0.5) color = YELLOW;

  Serial.print("  ");
  Serial.print(color);
  for (int i = 0; i < BAR_WIDTH; i++) {
    if (i < filled)          Serial.print("█");   // full block
    else if (i == threshCol) Serial.print(DIM "│" RESET);  // threshold tick
    else                     Serial.print(DIM "░" RESET);  // light shade
    if (i < filled) Serial.print(color);
  }
  Serial.print(RESET);
  Serial.printf("  %s%6.2f%s", BOLD, ratio, RESET);
  Serial.println(CLR_EOL);
}

static void drawFrame() {
  const bool warm = detector.count >= STALTA_PARAMS.warmupSamples;
  const double ratio = detector.sta / detector.lta;
  const bool banner = (millis() < bannerUntilMs);

  Serial.print(HOME);
  Serial.println(CLR_EOL);
  Serial.printf("  %s%sG E O S H A K E   D I Y%s   %slive seismic monitor%s%s\n",
                BOLD, CYAN, RESET, DIM, RESET, CLR_EOL);
  Serial.println(CLR_EOL);

  // status line
  Serial.print("  ");
  if (banner) {
    Serial.printf("%s%s  *** SHAKING DETECTED ***  %s", BOLD, BG_RED, RESET);
  } else if (warm) {
    Serial.printf("%s%s● ARMED%s   %sthreshold %.1f%s", BOLD, GREEN, RESET,
                  DIM, STALTA_PARAMS.trigger, RESET);
  } else {
    const int pct = (int)(100.0 * detector.count / STALTA_PARAMS.warmupSamples);
    Serial.printf("%s%s○ WARMING UP %d%%%s   %slearning the room%s", BOLD, YELLOW, pct, RESET,
                  DIM, RESET);
  }
  Serial.println(CLR_EOL);
  Serial.println(CLR_EOL);

  Serial.printf("  %sSTA / LTA%s%s\n", DIM, RESET, CLR_EOL);
  drawBar(ratio);
  Serial.println(CLR_EOL);

  Serial.printf("  %snoise floor%s   %s%.5f%s m/s²%s\n",
                DIM, RESET, WHITE, sqrt(detector.lta), RESET, CLR_EOL);
  Serial.printf("  %ssample rate%s   %s%.1f%s Hz%s\n",
                DIM, RESET, WHITE, measuredRate, RESET, CLR_EOL);
  Serial.printf("  %ssamples%s       %s%lu%s%s\n",
                DIM, RESET, WHITE, (unsigned long)sampleCount, RESET, CLR_EOL);
  Serial.printf("  %striggers%s      %s%s%lu%s%s\n",
                DIM, RESET, BOLD, (triggerCount ? RED : WHITE),
                (unsigned long)triggerCount, RESET, CLR_EOL);
  Serial.println(CLR_EOL);

  if (triggerCount > 0) {
    Serial.printf("  %slast trigger%s  ratio %s%.2f%s   PGA %s%.4f%s m/s²%s\n",
                  DIM, RESET, BOLD, lastRatio, RESET, BOLD, lastPga, RESET, CLR_EOL);
  } else {
    Serial.printf("  %sknock on the desk once it says ARMED%s%s\n", DIM, RESET, CLR_EOL);
  }
  Serial.print("\033[J");   // clear anything below
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.print(CLS);
  Serial.print(HOME);
  Serial.printf("%s%sGeoShake DIY%s\n\n", BOLD, CYAN, RESET);

  Serial.print("  golden vector self-test ... ");
  if (goldenSelfTest() != 0) {
    Serial.printf("%s%sFAILED%s\n", BOLD, RED, RESET);
    while (true) delay(1000);
  }
  Serial.printf("%s%sPASS%s  %sbit-compatible with the reference DSP%s\n",
                BOLD, GREEN, RESET, DIM, RESET);

  Serial.print("  sensors ................... ");
  const int found = sensors.begin(PIN_SCLK, PIN_MISO, PIN_MOSI, CS_PINS, SENSOR_COUNT);
  if (found == 0) {
    Serial.printf("%s%sNONE FOUND%s  (run spi_probe)\n", BOLD, RED, RESET);
    while (true) delay(1000);
  }
  Serial.printf("%s%s%d found%s  %s%s%s\n", BOLD, GREEN, found, RESET,
                DIM, sensors.partName(0), RESET);
  delay(1800);   // let the camera read the boot screen

  detector = initStalta();
  rateWindowStart = millis();
  Serial.print(CLS);
}

void loop() {
  if (sensors.sampleReady()) {
    double s0[3], s1[3];
    const bool ok0 = sensors.read(0, s0);
    const bool ok1 = (SENSOR_COUNT > 1) ? sensors.read(1, s1) : false;

    if (ok0 || ok1) {
      double buf[12];
      if (ok0 && ok1)      expandTwoSensors(s0, s1, buf);
      else if (ok0)        expandSingleSensor(s0[0], s0[1], s0[2], buf);
      else                 expandSingleSensor(s1[0], s1[1], s1[2], buf);

      const StaltaResult r = staltaStep(detector, buf);
      sampleCount++;
      rateWindowSamples++;

      if (r.fired) {
        triggerCount++;
        lastRatio = r.ratio;
        lastPga = r.pga;
        bannerUntilMs = millis() + BANNER_MS;
      }
    }
  }

  const uint32_t now = millis();
  if (now - rateWindowStart >= 5000) {
    measuredRate = rateWindowSamples * 1000.0 / (now - rateWindowStart);
    rateWindowStart = now;
    rateWindowSamples = 0;
  }

  if (now - lastFrameMs >= FRAME_MS) {
    lastFrameMs = now;
    drawFrame();
  }
}
