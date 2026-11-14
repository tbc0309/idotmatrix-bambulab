# Clockwise ESP32 + HUB75 64×64

Build with ESP-IDF 6.1 and the classic ESP32 toolchain.

```sh
idf.py -B build-clockwise -D MATRIX_BOARD=clockwise-esp32-hub75 build
```

On boards already using this profile's partition table, the application in OTA slot 0 can be updated at offset `0x20000`. The local panel scales the shared 32×32 renderer by 2×. It starts at brightness 16/255. Setup uses the open `PrintPixel-xxxxxx` hotspot and the portal at `http://192.168.4.1/`.

For first installation, flash `clockwise-esp32-hub75-full.bin` at `0x0`. Use `clockwise-esp32-hub75.bin` for application OTA.

The vendored HUB75 driver is [ESP32-HUB75-MatrixPanel-I2S-DMA 3.0.12](https://github.com/mrfaptastic/ESP32-HUB75-MatrixPanel-I2S-DMA/tree/6c7fc1e325cf2b1e5ca817ca4a07b45620aa63e9), adapted for ESP-IDF 6.1. Its license is in `vendor/LICENSE.txt`.
