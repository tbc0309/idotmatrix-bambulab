# 3D Print Pixel Monitor for Clockwise ESP32 + HUB75 64×64

Build with ESP-IDF 6.1 and the classic ESP32 toolchain.

```sh
idf.py -B build-clockwise -D MATRIX_BOARD=clockwise-esp32-hub75 build
```

On boards already using this profile's partition table, the application in OTA slot 0 can be updated at offset `0x20000`. The default firmware renders the 32×32 printer layout at 2× scale on the 64×64 panel. For the optional native 64×64 layout, see [Native 64×64](layouts/native64/README.md). The display brightness setting reaches 110/255 at 100%. Open **3D Print Pixel Monitor** to control the onboard panel’s power, brightness, rotation in 90° steps and print view. Setup uses the `PrintPixel-xxxxxx` hotspot (password `12345678`) and the portal at `http://192.168.4.1/`.

For first installation, flash `clockwise-esp32-hub75-full.bin` at `0x0`. Use `clockwise-esp32-hub75.bin` for application OTA.

The vendored HUB75 driver is [ESP32-HUB75-MatrixPanel-I2S-DMA 3.0.12](https://github.com/mrfaptastic/ESP32-HUB75-MatrixPanel-I2S-DMA/tree/6c7fc1e325cf2b1e5ca817ca4a07b45620aa63e9), adapted for ESP-IDF 6.1. Its license is in `vendor/LICENSE.txt`.
