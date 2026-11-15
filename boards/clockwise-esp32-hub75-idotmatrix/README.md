# Clockwise ESP32 + HUB75 64×64 · iDotMatrix

Independent firmware for the Clockwise board. It uses the iDotMatrix mobile app over Bluetooth. The separate Clockwise printer-monitor firmware is listed under `clockwise-esp32-hub75`.

Build from this directory with `pio run -e clockwise_esp32_hub75_64`. For a fresh installation, flash the `-full.bin` image at `0x0000`. For an existing installation of this App firmware with the same partition layout, flash the application `.bin` at `0x10000`. The app and printer-monitor images use different layouts. Use the full image when switching firmware.

## BOOT button / BOOT 按键

| Press duration / 按住时间 | Action on release / 松手后执行 |
| --- | --- |
| Under 5 s / 不到 5 秒 | Play the next saved Device Assets item / 播放下一份已存设备素材 |
| 5 s to under 10 s / 5 秒至不足 10 秒 | Clear the Bluetooth PIN only / 仅清除蓝牙密码 |
| 10 s or longer / 10 秒及以上 | Clear saved settings, PIN, alarms, schedules, and device assets, then reboot / 清除已保存的设置、密码、闹钟、定时任务和设备素材，然后重启 |

Based on [IDotMatrix-ESP32-Emulator v0.6.0](https://github.com/piggei/IDotMatrix-ESP32-Emulator). See the included MIT license.
