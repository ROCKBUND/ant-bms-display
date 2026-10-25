#pragma once

// ESP32S3-N16R16-128SPIT V1.0 integrated board.
// Verified against the supplied FullFunctionTest and board connection diagram.
constexpr int LCD_MOSI = 10, LCD_SCLK = 3, LCD_CS = 2, LCD_DC = 18;
constexpr int LCD_RST = 21, LCD_BL = 42;
constexpr unsigned LCD_SPI_HZ = 20000000;
constexpr int TOUCH_SDA = 8, TOUCH_SCL = 9, TOUCH_RST = 0, TOUCH_INT = 11;
constexpr unsigned TOUCH_I2C_HZ = 100000;
// User confirmed this board has no touch hardware (2026-10-19).
constexpr bool TOUCH_ENABLED = false;
constexpr unsigned DISPLAY_ROTATION = 0, TOUCH_ROTATION = 0;
// Vendor's complete example uses unmirrored raw coordinates at rotation 0.
constexpr bool TOUCH_SWAP_XY = false, TOUCH_MIRROR_X = false, TOUCH_MIRROR_Y = false;
constexpr int BUTTON_UP = 14, BUTTON_POWER = 15, BUTTON_DOWN = 16;
constexpr int SD_CS = 40;

constexpr char TARGET_NAME[] = "ANT-BLE24CAUB";
// Address type is discovered by BLE scan. Saved peers take precedence.
constexpr char TARGET_ADDRESS[] = "24:19:A4:02:23:45";
constexpr unsigned POLL_MS = 1000, STALE_MS = 5000, RECONNECT_MS = 15000;
constexpr unsigned SCAN_MS = 4000;
