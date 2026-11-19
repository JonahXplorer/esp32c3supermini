# Sugarat Button 舵机遥控

独立 ESP32-S3 固件。`SPEED AT 90` 一行的 `+` 和 `−` 调节两只舵机的速度，显示值按 90° 行程折算。`ANGLE` 一行每次改变 B 点相对 A 点的设定角度 5°。位于 B 点时立即按新角度移动，位于 A 点时下次切到 B 点生效。`GO TO A` 和 `GO TO B` 切换目标位置。ESP32-C3 的 GPIO4 实体按键使用同一组设定，屏幕会同步接收板回报的状态。

| 项目 | 设置 |
| --- | --- |
| Sugarat Button | ESP32-S3，COM20，360 × 360 ST77916 圆屏，CST816 触摸 |
| 舵机接收板 | ESP32-C3 Super Mini，COM21 |
| GPIO6 | A 为 1000 µs，90° 对应 2000 µs，约 145° 对应 2500 µs |
| GPIO7 | A 为 2000 µs，90° 对应 1000 µs，约 145° 对应 500 µs |
| 角度 | 两路同步调节，0–145°，每次接收端上电默认 90° |
| 速度 | 两路共用，90° 行程约 0.20–1.00 秒，每次接收端上电默认约 0.40 秒 |
| 无线连接 | ESP-NOW 信道 6，两端按已识别的 Wi-Fi MAC 单播，无需路由器 |

角度由各路校准点之间的脉宽插值估算。实际角度和转动时间取决于舵机和负载。`ANGLE MAX` 显示接收端已开放的最大角度。实测流程见 [ESP32-C3 接收端说明](../esp32c3/README.md#校准遥控上限)。

## 编译和烧录

使用 ESP-IDF 6.1。Sugarat 项目直接编译旁边 `Agent_link/firmware` 中的 SDGOODS 屏幕初始化和 QSPI 驱动。可以通过 CMake 参数 `-DAGENT_LINK_FIRMWARE=...` 指定其他位置。换板时，在两边的 CMake 配置中分别用 `-DSERVO_PEER_MAC=...` 指定对方的 Wi-Fi STA MAC。

从本仓库根目录依次运行：

```powershell
idf.py -C esp32c3 build
idf.py -C esp32c3 -p COM21 app-flash
idf.py -C sugarat build
idf.py -C sugarat -p COM20 flash
```

首次安装 Sugarat 时，整机烧录会写入独立项目的启动程序、分区表和应用，并切换为开机直接进入舵机遥控。后续两端均可用 `app-flash` 更新应用，角度控制需要两端更新到支持角度的版本。
