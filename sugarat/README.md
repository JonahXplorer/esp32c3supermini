# Sugarat Button 舵机遥控与 Jarvis 对话

独立 ESP32-S3 固件。点顶部的 `Servos` 或 `Jarvis` 切换页面，也可以在滑块外左右划。舵机页点 `Position A` 或 `Position B` 切换目标，拖动 `B angle` 或 `Travel time / 90°` 滑块调节角度或速度。达到目标设定后松手，界面会等接收端确认。B 点以 5° 为步长，位于 B 点时立即移动，位于 A 点时下次切到 B 点生效。速度显示为 90° 行程的估计用时，向右滑更快。ESP32-C3 的 GPIO4 实体按键使用同一组设定，屏幕会同步接收板回报的状态。

| 项目 | 设置 |
| --- | --- |
| Sugarat Button | ESP32-S3，COM18，360 × 360 ST77916 圆屏，CST816 触摸 |
| 舵机接收板 | ESP32-C3 Super Mini，COM21 |
| GPIO6 | A 为 1000 µs，90° 对应 2000 µs，约 145° 对应 2500 µs |
| GPIO7 | A 为 2000 µs，90° 对应 1000 µs，约 145° 对应 500 µs |
| 角度 | 两路同步调节，0–145°，每次接收端上电默认 90° |
| 速度 | 两路共用，90° 行程约 0.20–1.00 秒，每次接收端上电默认约 0.40 秒 |
| 无线连接 | ESP-NOW 按已有 Wi-Fi MAC 配对，C3 自动跟随 Sugarat 的 2.4 GHz 信道 |

角度由各路校准点之间的脉宽插值估算。实际角度和转动时间取决于舵机和负载。`B angle` 显示接收端已开放的角度范围。实测流程见 [ESP32-C3 接收端说明](../esp32c3/README.md#校准遥控上限)。

## Jarvis 语音

- 首次启动时连接 `Sugarat-Jarvis-XXXX` 热点，通过 Agent Link 配网页设置 2.4 GHz Wi-Fi。Sugarat 按当前 Wi-Fi 信道发送 ESP-NOW。C3 失联后轮询本机地区配置允许的信道，收到已配对 Sugarat 的数据后锁定该信道。舵机页显示 `Finding receiver...` 或已连上的信道。配网热点仍使用信道 6。
- 在 `Settings → Network → Voice server` 选择 `Overseas` 或 `China`，确认后设备会重启。升级默认使用海外平台。两边独立保存设备绑定，共用 Wi-Fi 设置。
- Jarvis 页显示六位激活码时，在屏幕提示的平台认领设备并绑定支持语音的 Agent。`Say “Jarvis”` 表示可以唤醒。[海外平台](https://bot.sugarat.ai) · [国内平台](https://bot.deotaland.cn)
- 说 **Jarvis** 后接着说话。设备通过 ESP-SR 的单麦 AFE 在本地做语音活动和 WakeNet 检测，唤醒后缓冲最多约 8 秒语音，Agent Link 的 Realtime 会话就绪后才发送。通话使用已绑定 Agent 的最近一个会话，没有会话时会尝试创建。
- Realtime 回复从 Sugarat 扬声器播放。播放期间上传静音，播放结束后恢复真实麦克风。点 Jarvis 页的 `End call` 结束通话，设备回到唤醒待机。

## 源码结构

| 路径 | 职责 |
| --- | --- |
| `main/` | ESP-NOW 遥控、LVGL 页面及圆屏和触摸适配 |
| `components/sugarat_board/` | Sugarat 屏幕、触摸、I2S 麦克风和扬声器 |
| `components/sugarat_voice/` | Jarvis 模型、Realtime 会话和播放队列 |
| `third_party/agent_link/` | 内置 Agent Link SDK，来源 `277420e`，附 MIT 许可 |
| `../common/servo_radio/` | 本仓库内 Sugarat 与 ESP32-C3 共用的舵机协议 |

本项目使用自带的 SDK 和板级驱动，外部依赖由 ESP-IDF 组件管理器按 `dependencies.lock` 下载。

## 编译和烧录

使用 ESP-IDF 6.1。换板时，在两边的 CMake 配置中分别用 `-DSERVO_PEER_MAC=...` 指定对方的 Wi-Fi STA MAC。

从本仓库根目录依次运行：

```powershell
idf.py -C esp32c3 build
idf.py -C esp32c3 -p COM21 app-flash
idf.py -C sugarat build
idf.py -C sugarat -p COM18 flash
```

首次安装语音功能需要对 Sugarat 执行完整 `flash`，编译生成的 Jarvis 模型和新分区表会一起写入。两路舵机的角度控制仍由 ESP32-C3 接收端执行。之后仅更新 Sugarat 应用代码时可用 `app-flash`。
