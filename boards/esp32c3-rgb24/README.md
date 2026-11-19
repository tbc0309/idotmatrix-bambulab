# ESP32-C3 24 RGB Ring

ESP32-C3，4 MB Flash，24 颗 WS2812／GRB 灯珠，数据脚 GPIO10，BOOT 为 GPIO9。
板型 ID：`esp32c3-rgb24`。原生 ESP-IDF RMT 输出，保留项目的 Wi-Fi、
云端／局域网打印机、后台管理、OTA 和可选 iDotMatrix 蓝牙联动。
板载灯环独立工作，无需绑定外接像素屏。

| 状态 | 灯环显示 |
| --- | --- |
| 待机 | 红点为时针、绿点为分针、蓝点为秒针；重叠时混色 |
| 时间未同步 | 暗黄色追逐点 |
| 准备 | 黄色追逐点 |
| 打印 | 绿色累计进度，当前进度端点闪动，剩余灯珠暗灰 |
| 暂停 | 黄色进度 |
| 完成 | 七彩渐变跑马 10 秒，随后回到时钟 |
| 取消 | 全环暗红，30 秒后回到时钟 |
| 故障 | 红色闪动 |

时钟为 12 小时制，24 灯每颗代表半小时／2.5 分钟／2.5 秒。
以第 0 颗灯为起点，按灯珠串接顺序前进；安装方向需实机确认。
RGB 每通道上限为 32/255，限制亮度与功耗。

## 固件 / Firmware

- `esp32c3-rgb24-full.bin`：首次 USB 安装 / Initial USB installation.
- `esp32c3-rgb24.bin`：后台 OTA 升级 / Application OTA update.

[在线刷机 / Web installer](https://tbc0309.github.io/xTouch-CN/idotmatrix.html)
