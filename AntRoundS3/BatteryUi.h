#pragma once
#include <Arduino_GFX_Library.h>
#include "BmsLink.h"

// All UI/LVGL calls belong to the Arduino loop task, never the BLE worker.
// Starts a non-blocking 0 -> 100 SOC sweep at boot. A separate sweep runs once
// per connection after valid data. Smoothly retracts the arc after the peak,
// then restores the latest actual SOC (or -- while data is unavailable).
bool beginBatteryUi(Arduino_GC9A01& display);
void updateBatteryUi(const Snapshot& snapshot, int page, bool choosing,
                     unsigned selection, bool touchOK, bool force = false);
void tickBatteryUi(uint32_t now);
