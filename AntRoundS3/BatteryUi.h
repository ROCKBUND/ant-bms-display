#pragma once
#include <Arduino_GFX_Library.h>
#include "BmsLink.h"

// All UI/LVGL calls belong to the Arduino loop task, never the BLE worker.
bool beginBatteryUi(Arduino_GC9A01& display);
void updateBatteryUi(const Snapshot& snapshot, int page, bool choosing,
                     unsigned selection, bool touchOK, bool force = false);
void tickBatteryUi(uint32_t now);
