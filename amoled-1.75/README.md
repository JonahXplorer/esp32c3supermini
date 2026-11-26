# Waveshare ESP32-S3-Touch-AMOLED-1.75：Jarvis 与舵机遥控

面向微雪 **ESP32-S3-Touch-AMOLED-1.75**（标准版、B 外壳版共用基础硬件）的 ESP-IDF 工程，16 MB Flash、8 MB 八线 PSRAM。不是 1.75C 的固件。

直接引用 `../sugarat/main/` 的应用、`../sugarat/components/sugarat_voice/` 的语音业务、`../sugarat/third_party/agent_link/` 和 `../common/servo_radio/`。请保留本仓库的目录结构。这些引用是生产代码，不是独立复制的另一个应用。

## 功能

- A / Orbit 原生 AMOLED 主屏，接入现有全部 16 个设备状态；触摸发起通话、Jarvis 本地唤醒、结束通话、会话列表/新建/选择/删除。
- 开启通话、正常通话及通话内重连时主标题保持 `Listening`；正常通话中，Agent 播放只将中央输入波形切换为外扩圆波，标题、提示和按钮不变，不单独建立 AI 思考状态。待机波形入口改为唯一的 `Start call` 主按钮。
- Sugarat 平台激活、Agent 绑定、Wi-Fi 配网、Realtime 语音、语音工具及本地舵机口令。
- ESP-NOW 舵机 A/B 控制、角度/速度设置及接收端状态同步。
- 主屏使用原生 466×466 坐标，不再放大 360×360 画布；顺时针旋转 90°，触摸同步对应。音量、亮度、会话、手动控制等原有页面以 360×360 居中保留，从主屏顶部下拉或点击进入。AMOLED 亮度通过 CO5300 的 `0x51` 命令控制，音量/亮度设置继续保存到 NVS。
- UI 状态与按钮权限集中在 `main/ui/home_presenter.c`，布局和绘制在 `home_screen.c`，主题在 `ui_theme.h`。详见 [状态与代码结构](main/ui/README.md)。现有程序没有电量接口，主屏隐藏未知电量。

## 触摸与画面方向

CST9217 的实现移植自微雪仓库 `examples/arduino/10_Touch_CST9217` 及其自带 SensorLib，参考版本为 `54816190c194c3a9cf612f85063899c89b18a776`。正式实现位于 `components/sugarat_board/touch.c`，直接复用公用 I2C 总线。

- 与参考例程一样，用 GPIO11 下降沿中断记录待读取事件，界面任务读取数据；中断中不访问 I2C。
- 按 `TouchDrvCST92xx::getAttribute()` 的复位、属性读取顺序初始化，读取芯片型号、分辨率和固件信息。
- 一次读取 `0xD000` 的 15 字节双触点报文，随后发送 `D0 00 AB` 确认；空报文、无有效 ACK、无有效触点均返回“未按下”，不作为通信错误刷屏。指针界面使用第一个触点。
- 两次触摸中断之间保留最后的按下状态，避免按住和拖动时因没有新中断而被误释放。真正的 I2C 错误会释放指针，并限频记录错误码。
- 显示与触摸都直接使用 466×466 原生坐标；显示顺时针旋转 90°，触摸应用对应的逆变换，不再进行 360×360 缩放。

## 全程开麦与回声消除

音频采集、AEC 与网络上传分开执行。初始化成功后，ES7210、I2S RX 和 AEC 持续运行：待机、唤醒、建连、用户说话、设备播报、回复间隙以及通话重连期间都持续采集和处理。待机音频只用于本地唤醒；通话建立后才上传。结束通话停止上传并返回唤醒待机，不关闭麦克风硬件。

音频链路：

```text
ES7210 MIC1 + MIC2 → 等权混音 → ESP-SR 全双工 AEC → 16 kHz 单声道 → Realtime 上传
                                      ↑
                          ES7210 MIC3 ← ES8311 模拟播放回采
Realtime 回复 → 重采样至 16 kHz → 软件音量 → ES8311 → 功放 → 扬声器
```

- 使用 ESP-SR 2.6.1 的低运算量全双工模式 `AEC_MODE_FD_LOW_COST`，滤波长度 4，NLP 为 NORMAL，以保留双方同时说话时的近端语音。双麦混音不包含波束成形。
- ES7210 是音频 ADC；实际回声消除算法运行在 ESP32-S3 上，板载模拟回采为算法提供参考信号。
- ES7210 四槽顺序为 **MIC1、MIC3（回采）、MIC2、MIC4（未连接）**，与物理麦克风编号不同。
- STD TX 与 TDM RX 共用 GPIO42/9/45 的 MCLK/BCLK/LRCK。输出为 2×32-bit 槽（16-bit PCM），输入为 4×16-bit 槽，BCLK 均为 64 fs，MCLK 为 256 fs。
- 扬声器播放期间仍上传真实的 AEC 输出，不启用原板的播放静音保护。AEC 在回复和通话边界持续保留自适应状态。
- 功放开关只控制播放输出，不停止 ADC、RX 或共享时钟。断网期间继续处理本地音频，恢复后重新开启上传，不保证断网音频补传。
- 采集任务在每次读取前检查 100 ms 调度预算，到期主动阻塞 1 个系统 tick，为空闲任务和其他工作留出运行机会。当前 100 Hz 配置中，1 tick 为 10 ms。此期间 I2S DMA、麦克风和 AEC 状态持续保留，随后处理缓冲中的真实音频，不插入静音或主动丢弃样本。看门狗仍保持原有的 5 秒超时设置。
- AEC 初始化失败会报告语音不可用，不会退回未消回声的全双工模式。云端对打断的响应仍取决于绑定 Agent 的语音服务。

默认麦克风增益 24 dB，模拟回采增益 0 dB，可在 `menuconfig → Waveshare AMOLED 1.75 audio` 调整。启动日志应显示 `mode=FD_LOW_COST`。每约 5 秒输出 `AEC continuous` 日志，包含原始麦克风、回采、处理后平均绝对幅度、削顶数、AEC 平均/最长耗时、超出单块音频时长的次数 `over_budget`、主动调度次数 `capture_yields` 及 RX 溢出数。耗时是包含抢占的实际经过时间；此日志用于检查通道和实时性，单凭幅度下降不能证明实际回声消除效果。

## 引脚

| 功能 | 配置 |
| --- | --- |
| 公用 I2C | SDA GPIO15 / SCL GPIO14 |
| CO5300 QSPI | CLK38 / CS12 / D0–D3 GPIO4–7 / RESET39，列偏移 6 |
| CST9217 | 地址 0x5A / RESET40 / INT11；原双轴镜像加画面旋转逆变换 |
| ES8311 | 地址 0x18，播放数据 GPIO8 |
| ES7210 | 地址 0x40，采集数据 GPIO10 |
| 音频时钟 | MCLK42 / BCLK9 / LRCK45 |
| 功放控制 | GPIO46，高有效，由音频驱动管理 |
| AXP2101 | 地址 0x34；使用现有供电，不修改电池充电参数 |

## 编译

使用已安装的 **ESP-IDF 6.1** 环境，在仓库根目录执行：

```powershell
$env:CMAKE_BUILD_PARALLEL_LEVEL = '2'
$env:IDF_PY_BUILD_JOBS = '2'
idf.py -C amoled-1.75 build
```

只构建此板，复用 `amoled-1.75/build/`。组件版本由 `dependencies.lock` 固定。新板使用 16 MB Flash 配置；不要复用 Sugarat 的 `sdkconfig`。

## 配对、烧录与验收

默认舵机接收端的 Wi-Fi STA MAC 为 `EC:D6:1B:B6:09:30`，与现有 `sugarat` 相同。若接收端不同，在编译时指定：

```powershell
idf.py -C amoled-1.75 -DSERVO_PEER_MAC=AA:BB:CC:DD:EE:FF build
```

新 AMOLED 板有自己的 Wi-Fi STA MAC。接收端 `esp32c3` 必须允许这个 MAC；仅复用 Sugarat 的目标 MAC 不会自动获得接收端授权。可在接收端的 `SERVO_PEER_MAC` 或 `SERVO_ADDITIONAL_PEER_MAC` 中配置。实际 MAC 可在本板启动时的 `servo_radio` 日志中查看；舵机页面打开后才按需启动遥控。未确认新板 MAC 前，本工程不会改动接收端配置。

确认设备串口后，在 ESP-IDF 环境中执行完整 `flash`，它会同时写入应用、分区表和 Jarvis 模型：

```powershell
idf.py -C amoled-1.75 -p COMx flash monitor
```

`COMx` 替换为实际串口。首次安装不能只执行 `app-flash`；后续只更新应用时才可使用它。

本次生成的合并固件为 `build/amoled-175-jarvis-full.bin`，包含引导、分区表、应用和 Jarvis 模型，写入地址为 **0x0**。已逐段核对合并内容与原始文件一致。可通过以下命令重新生成：

```powershell
idf.py -C amoled-1.75 build merge-bin -o amoled-175-jarvis-full.bin
```

合并镜像中的填充区会覆盖现有 NVS，因此用于首次安装；保留配网与激活信息的更新应使用上面的分段 `flash` 或 `app-flash`。

1. 检查画面相对原固件顺时针旋转 90°，点击与拖动位置对应，松手后正确释放，待机无 `Read touch` 刷屏；再检查亮度与音量设置。连接外置扬声器到板载扬声器接口。
2. 首次启动连接 `Sugarat-Jarvis-XXXX` 热点，设置 2.4 GHz Wi-Fi。根据屏幕激活码在 <https://bot.sugarat.ai> 认领，并绑定语音 Agent。
3. 启动日志应有 `AEC full duplex ready`。说 Jarvis 或触摸开始通话，确认用户声音能上传并听到回复。
4. **单独播报**：播放期间保持安静，确认回采 `ref` 随播报变化、回采不削顶、没有把设备播报重复识别为用户发言。
5. **同时说话**：设备播报时说另一句完整的话，检查语音转写、服务端打断和设备停止旧回复的效果；不能只测播报结束后的识别。
6. 连续对话、静音间隙、音量变化、结束后再次唤醒及断网重连，检查麦克风持续处理，正常运行时 `rx_overflows` 应为 0。
7. 配置好接收端的发送者 MAC 后，验证触摸与语音舵机控制。

编译验证不替代以上实机声学验收，尤其需要在实际外壳、扬声器音量与使用距离下测试同时说话。

## 官方依据与许可证

- [产品文档](https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-1.75/)
- [微雪硬件参考](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/HARDWARE_REFERENCE.md)
- [微雪维护的 BSP](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/firmware/brookesia/components/waveshare__esp32_s3_touch_amoled_1_75/esp32_s3_touch_amoled_1_75.c)
- [微雪原理图](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/Schematic/ESP32-S3-Touch-AMOLED-1.75-schematic.pdf)
- [微雪触摸中断示例](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/54816190c194c3a9cf612f85063899c89b18a776/examples/arduino/10_Touch_CST9217/10_Touch_CST9217.ino)
- [微雪自带 SensorLib 触摸协议实现](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/54816190c194c3a9cf612f85063899c89b18a776/examples/arduino/libraries/SensorLib/src/touch/TouchDrvCST92xx.cpp)

引脚、CO5300 初始化参数及音频时钟/槽位配置依据上述官方资料重新实现；CST9217 的协议移植及 MIT 许可见 `components/sugarat_board/NOTICE.touch` 与 `LICENSE.SensorLib`。ES8311 和 ES7210 使用组件管理器下载的官方驱动及其许可证。复用的 QSPI 适配器与 Agent Link 保留原目录的许可文件。
