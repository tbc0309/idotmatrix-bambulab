# Clockwise 64×64 · iDotMatrix App (native prototype)

Build this board directory with ESP-IDF 6.1 using `idf.py build`. The manifest pins LVGL 9.6.0. This project uses ESP-IDF NimBLE and the repository's native HUB75 DMA driver. It uses the existing Clockwise App flash layout.

The prototype implements a single LVGL clock, the native 16-bit GATT service, device information, local time from the phone, brightness and power. It has no App media player, BLE password handling, BOOT reset or release/OTA integration yet. The prior firmware remains in the source history for protocol and display porting.
