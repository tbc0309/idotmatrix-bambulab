# Clockwise 64脳64 路 iDotMatrix App

This experimental port uses ESP-IDF 6.1, LVGL 9.6.0, ESP-IDF NimBLE and the native HUB75 DMA driver. Build from this directory with ESP-IDF 6.1 activated.

```sh
idf.py -B build/ble build
idf.py -B build/wifi -D CLOCKWISE_WITH_WIFI=ON build
```

Each build directory has its own SDK configuration. If the component registry is unreachable, use the official LVGL v9.6.0 source extracted to a directory named lvgl and pass -D CLOCKWISE_LVGL_PATH=/absolute/path/to/lvgl. Both builds use the established Clockwise App flash partition layout and eight bits per HUB75 color channel. The Wi-Fi build adds ESP-IDF SoftAP, DHCP, captive DNS, HTTP provisioning, scan results, NVS and UTC+8 SNTP. The display pauses while a client is attached to the setup network.

The BLE prototype handles device information, local time from the phone, brightness, power, PIN verification, display reset and returning to the clock. The first media port accepts a 64x64 RGB24 still image and validates the transfer CRC. Seven lighting effects and the ten-second BOOT reset are implemented. GIF and PNG playback, stored media and the full clock gallery remain to be ported. Phone compatibility and destructive reset testing for the newly ported commands are pending. The prior implementation is available in the parent history.
