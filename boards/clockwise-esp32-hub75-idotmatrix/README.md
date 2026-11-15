# Clockwise ESP32 + HUB75 64×64 · iDotMatrix

Independent firmware for the Clockwise board. It uses the iDotMatrix mobile app over Bluetooth. The separate Clockwise printer-monitor firmware is listed under `clockwise-esp32-hub75`.

Build from this directory with `pio run -e clockwise_esp32_hub75_64`. For this release, flash the `-full.bin` image at `0x0000`. For an existing installation of this App firmware with the same partition layout, flash the application `.bin` at `0x10000`. The 1.0.2 App release uses a new partition layout with 2 MiB for device media. Moving from an older layout or switching firmware requires the full image.

iDotMatrix App 通过蓝牙校时。The iDotMatrix app sets the clock over Bluetooth.

## 素材存储 / Media storage

素材分区为 2 MiB。从旧布局升级请选择完整固件。云动图上传空间不足时，设备会按需清理旧预览和残留临时文件，保留已收藏的设备素材、闹钟和日程素材。

When space is tight during a cloud GIF upload, the firmware reclaims previous previews and leftover temporary files. Saved device assets and media used by alarms or schedules are kept.

## BOOT button / BOOT 按键

| Press duration / 按住时间 | Action on release / 松手后执行 |
| --- | --- |
| Under 1 s / 不到 1 秒 | Play the next saved Device Assets item / 播放下一份已存设备素材 |
| 1 s to under 10 s / 1 秒至不足 10 秒 | No action / 无动作 |
| 10 s or longer / 10 秒及以上 | Clear saved settings, PIN, alarms, schedules, and assets, then reboot / 清除设置、密码、闹钟、定时任务和素材，然后重启 |

Based on [IDotMatrix-ESP32-Emulator v0.6.0](https://github.com/piggei/IDotMatrix-ESP32-Emulator). See the included MIT license.
