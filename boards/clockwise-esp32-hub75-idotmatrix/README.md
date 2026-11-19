# Clockwise 64脳64 路 iDotMatrix App

This firmware uses ESP-IDF 6.1, LVGL 9.6.0, ESP-IDF NimBLE and the native HUB75 DMA driver. Build from this directory with ESP-IDF 6.1 activated.

```sh
idf.py -B build/ble build
idf.py -B build/wifi -D CLOCKWISE_WITH_WIFI=ON build
```

Each build directory has its own SDK configuration. If the component registry is unreachable, use the official LVGL v9.6.0 source extracted to a directory named lvgl and use esp_littlefs v1.23.1 (including its submodule) in a directory named littlefs. Pass both `-D CLOCKWISE_LVGL_PATH=/absolute/path/to/lvgl` and `-D CLOCKWISE_LITTLEFS_PATH=/absolute/path/to/littlefs`. Both builds use the established Clockwise App flash partition layout and eight bits per HUB75 color channel. The Wi-Fi build adds ESP-IDF SoftAP, DHCP, captive DNS, HTTP provisioning, scan results, NVS, the bilingual Windows time-zone picker and SNTP (default UTC+8). After successful setup the hotspot closes and management remains available at the LAN address. An unconfigured hotspot closes after ten minutes and releases HTTP, DNS and Wi-Fi resources. The display pauses while a client is attached to the setup network.

The native firmware handles BLE device information, local time, brightness, power, PIN and seven lighting effects. Transfers stage RGB24, PNG, GIF and bitmap text in LittleFS; the regular media transfer checks CRC before replacing a file. Short BOOT cycles available stored GIF, PNG and RGB24 files; holding BOOT for at least ten seconds and releasing erases settings and media. A nonblank filesystem that cannot be mounted is left intact. Device Assets transfers are staged in a separate generation. A persisted manifest switches playback to the new slots after the batch completes, or after the eight-second settle period for a sparse batch. Interrupted transfers leave the previous manifest available after reboot. The firmware includes eight clock styles, alarms, schedules, and music-reactive modes.
