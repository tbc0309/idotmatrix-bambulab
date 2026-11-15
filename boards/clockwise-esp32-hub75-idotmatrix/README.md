# Clockwise ESP32 + HUB75 64×64 · iDotMatrix

Independent firmware for the Clockwise board. It uses the iDotMatrix mobile app over Bluetooth. The separate Clockwise printer-monitor firmware is listed under `clockwise-esp32-hub75`.

Build from this directory with `pio run -e clockwise_esp32_hub75_64`. For this release, flash the `-full.bin` image at `0x0000`. For an existing installation of this App firmware with the same partition layout, flash the application `.bin` at `0x10000`. The 1.0.2 App release uses a new partition layout with 2 MiB for device media. Moving from an older layout or switching firmware requires the full image.

## Wi-Fi setup and time sync / 配网与对时

1. 首次配网时连接设备的开放热点（与蓝牙同名，例如 `IDM_64*64_4C42`）。若配网页面没有自动弹出，访问 `http://192.168.4.1/`。
2. 输入 2.4 GHz Wi-Fi 的名称和密码。设备联网后，选择并保存时区。配网热点随后关闭，设备自动重连 Wi-Fi 并定期对时。
3. 在同一局域网打开设备 IP，可以修改时区、切换 12/24 小时制或重新配网。配网页面打开后自动扫描 Wi-Fi，时区默认 UTC+08:00，时钟默认使用 24 小时制。右上角可切换简体中文和英文。

开机满 10 分钟仍未完成首次配网时，热点自动关闭。重启后可重新配网。开机即可用 iDotMatrix App 通过蓝牙连接和控制。首次配网期间和配网热点关闭后，均可继续使用蓝牙。

Connect to the open hotspot named after the Bluetooth device (for example, `IDM_64*64_4C42`) and open `http://192.168.4.1/` if the setup page does not appear. Join a 2.4 GHz network and save a time zone. The setup hotspot closes, and the device reconnects for automatic time sync. Use its LAN IP to change Wi-Fi, the time zone, or the clock format later. The setup page automatically scans for nearby Wi-Fi. The defaults are UTC+08:00 and 24-hour time. Use the top-right menu to switch between English and Simplified Chinese. On an unconfigured device the hotspot closes after 10 minutes. Restart to try again. Bluetooth control is available from boot, during Wi-Fi setup, and after the hotspot closes.

## 素材存储 / Media storage

素材分区为 2 MiB。从旧布局升级请选择完整固件。云动图上传空间不足时，设备会按需清理旧预览和残留临时文件，保留已收藏的设备素材、闹钟和日程素材。

When space is tight during a cloud GIF upload, the firmware reclaims previous previews and leftover temporary files. Saved device assets and media used by alarms or schedules are kept.

## BOOT button / BOOT 按键

| Press duration / 按住时间 | Action on release / 松手后执行 |
| --- | --- |
| Under 1 s / 不到 1 秒 | Play the next saved Device Assets item / 播放下一份已存设备素材 |
| 1 s to under 10 s / 1 秒至不足 10 秒 | No action / 无动作 |
| 10 s or longer / 10 秒及以上 | Clear Wi-Fi, time zone, saved settings, PIN, alarms, schedules, and assets, then reboot / 清除 Wi-Fi、时区、设置、密码、闹钟、定时任务和素材，然后重启 |

Based on [IDotMatrix-ESP32-Emulator v0.6.0](https://github.com/piggei/IDotMatrix-ESP32-Emulator). See the included MIT license.
