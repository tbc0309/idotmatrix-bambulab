# Clockwise ESP32 + HUB75 64×64 · iDotMatrix

Independent firmware for the Clockwise board. It uses the iDotMatrix mobile app over Bluetooth. The separate Clockwise printer-monitor firmware is listed under `clockwise-esp32-hub75`.

Build from this directory with `pio run -e clockwise_esp32_hub75_64`. For a fresh installation, flash the `-full.bin` image at `0x0000`. For an existing installation of this App firmware with the same partition layout, flash the application `.bin` at `0x10000`. The app and printer-monitor images use different layouts. Use the full image when switching firmware.

Based on [IDotMatrix-ESP32-Emulator v0.6.0](https://github.com/piggei/IDotMatrix-ESP32-Emulator). See the included MIT license.
