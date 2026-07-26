#pragma once

#include "config.h"
#include <GxEPD2_BW.h>

#if defined(EPD_PANEL_SSD1683)
using EpdPanel = GxEPD2_420_GDEY042T81;
constexpr const char *PANEL_NAME = "GDEY042T81 / SSD1683";
#elif defined(EPD_PANEL_UC8176)
using EpdPanel = GxEPD2_420;
constexpr const char *PANEL_NAME = "GDEW042T2 / UC8176";
#else
#error "Select a panel in include/config.h (EPD_PANEL_UC8176 or EPD_PANEL_SSD1683)"
#endif

// The whole 400x300 frame is 15 kB, so one page covers the screen.
using EpdDisplay = GxEPD2_BW<EpdPanel, EpdPanel::HEIGHT>;
