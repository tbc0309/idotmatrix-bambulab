# Third-party components

| Component | Use | Licence / source |
| --- | --- | --- |
| ArduinoJson 6.21.6 | JSON parsing; header library only, without the Arduino framework | MIT; [source](https://github.com/bblanchon/ArduinoJson) |
| ESP-IDF 6.1.x | ESP32 runtime, Wi-Fi, NimBLE, TLS, storage, HTTP and OTA | Apache-2.0 and component licences; [source](https://github.com/espressif/esp-idf) |
| esp-mqtt 1.1.0 | Native MQTT client | Apache-2.0; [source](https://github.com/espressif/esp-mqtt) |

The application's independently authored code is MIT-licensed. Third-party components retain their own licenses.

The unmodified board illustration at `boards/lckfb-esp32s3r8n8/board.png` is reproduced from [LCKFB's ESP32-S3 expansion-board documentation](https://wiki.lckfb.com/zh-hans/esp32s3r8n8/projects/expansion-board.html), which credits Xiaohan Electronic Studio and specifies [CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/). It is used only in the README, not embedded in firmware.


Timezone definitions are exported from Microsoft Windows using its built-in System.TimeZoneInfo APIs and resource labels. No external timezone library or conversion table is bundled. See `resources/timezones/SOURCE.txt`.

The Seeed Studio XIAO ESP32C6 board photo in the README was supplied for this project.
