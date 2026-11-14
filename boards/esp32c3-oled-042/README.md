# ESP32-C3 0.42 OLED

01Space-style ESP32-C3 board with 4 MB flash and a 72×40 monochrome SSD1306 OLED.
The display uses I²C address `0x3c`, SDA on GPIO5, SCL on GPIO6 and a controller
column offset of 28. BOOT is GPIO9. The board also supports an external
32×32 iDotMatrix over BLE.

The OLED shows a large status label above the clock or print progress. It displays
`SETUP` before Wi-Fi connects, and the print percentage during preparation,
printing and pause. While idle, the clock and scrolling IP address alternate: time for 10 seconds, then the IP address for 5 seconds.

```sh
idf.py -B build-c3-oled -D MATRIX_BOARD=esp32c3-oled-042 build
python scripts/package.py --build build-c3-oled --output dist-c3-oled
```

- [01Space schematic and examples](https://github.com/01Space/ESP32-C3-0.42LCD)
- [Zephyr board configuration](https://pigweed.googlesource.com/third_party/github/zephyrproject-rtos/zephyr/+/refs/tags/v4.2.1/boards/01space/esp32c3_042_oled/esp32c3_042_oled.dts)
