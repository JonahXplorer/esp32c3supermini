# ESP32-C3 Super Mini 按键控制双舵机

ESP-IDF 6.1 工程，适用于两只 1171MG 这类三线 PWM 角度舵机。上电后两只舵机先转到 A 点。GPIO4 按一下，两只舵机平滑转到 B 点并保持。再按一下，转回 A 点并保持。程序对按键消抖，长按只触发一次，松手时保留目标角度。

| 线 | 接法 |
| --- | --- |
| 按键信号 | 底板黄色 `4`（GPIO4），0 V 为松开，3.3 V 为按下 |
| 舵机 1 信号 | 底板黄色 `6`（GPIO6） |
| 舵机 2 信号 | 底板黄色 `7`（GPIO7） |
| 两个舵机的电源正极 | 外置稳压电源 `+5V` |
| 两个舵机的地 | 外置电源 `GND`，再连到底板黑色 `GND` |
| 核心板电源 | 核心板的 USB-C 口 |

如果用的是无源按键，两端分别接 `GPIO4` 和 `3V3`，程序已开启内部下拉。如果用的是带信号输出的按键模块，模块也要与底板共地。舵机电源正极直接接外置电源，避开底板的红色 `VCC1/VCC2` 排针。接线表假设舵机可以识别 3.3 V PWM，若实物要求 5 V 控制电平，在信号线上加单向电平转换。

两路舵机使用 50 Hz PWM。GPIO6 的 A 点是 1000 µs，90° 对应 2000 µs，约 145° 对应 2500 µs。GPIO7 的 A 点是 2000 µs，90° 对应 1000 µs，约 145° 对应 500 µs。两路在空载同步点动时已验证约 145°。默认每 10 ms 向目标推进最多 25 µs，90° 行程的脉宽过渡约 0.4 秒。

在 `main/main.c` 用 `SERVO_GPIO6_A_US`、`SERVO_GPIO6_90_US` 校准 GPIO6，用 `SERVO_GPIO7_A_US`、`SERVO_GPIO7_90_US` 校准 GPIO7。按键日志会分别输出两路的目标脉宽。默认切换速度由 `../common/servo_radio/servo_radio.h` 中的 `SERVO_SPEED_DEFAULT` 设定。

[Sugarat Button 遥控](../sugarat/README.md)和 [AMOLED 1.75 遥控](../amoled-1.75/README.md)通过同一 ESP-NOW 协议切换 A/B、调整两路的速度及 B 点角度。C3 仅接收已配对控制器的数据，并自动寻找控制器所在的 2.4 GHz 信道。GPIO4 实体按键使用同一组设定。有效的角度/速度设置保存在 NVS，重启后恢复；没有有效记录时使用默认速度和 90°。

## 三个控制器的配对

接收端 STA 地址为 `EC:D6:1B:B6:09:30`（当前 USB 串口 COM21）。原有两台 Sugarat 保留，新增 AMOLED：

| CMake 配置 | 默认控制器 STA 地址 | 用途 |
| --- | --- | --- |
| `SERVO_PEER_MAC` | `EC:DA:3B:67:12:14` | 第一台 Sugarat |
| `SERVO_ADDITIONAL_PEER_MAC` | `EC:DA:3B:67:12:2C` | 第二台 Sugarat，当前 COM18 |
| `SERVO_AMOLED_PEER_MAC` | `B8:1F:3F:D1:EE:04` | AMOLED 1.75，当前 COM5 |

AMOLED 默认向接收端 `EC:D6:1B:B6:09:30` 发送。双方地址须互相对应；
控制器最多三个，空的可选地址跳过，重复地址合并，未配对设备仍被拒绝。
每个控制器独立维护 session/sequence，回执返回实际发送者，报文布局仍为 20 字节。
更换板卡时以该板启动日志的 `ESP-NOW local` / Wi-Fi STA 地址为准，
通过对应 CMake 配置修改配对地址；串口号会随 USB 插口和系统分配变化。

只编译本接收端：

```powershell
idf.py -C esp32c3 -DSERVO_AMOLED_PEER_MAC=B8:1F:3F:D1:EE:04 build
```

默认构建目录会保存此前的 CMake 参数；修改已使用过的配对地址时需显式传入新的 `-D` 值。
更新已有接收端应用可用 `idf.py -C esp32c3 -p COM21 app-flash`，保留 NVS 角度/速度设置。
启动后应看到三个 `ESP-NOW local ... peer ...` 配对日志。

AMOLED 顶部下拉或点击 → Settings → Manual control；等待 Receiver connected 后，
测试 Position B 再 Position A。C3 应记录 `Controller requested position` 和 `peer=2` 的回执，
AMOLED 应记录匹配 session/sequence 的 `C3 confirmed target`。这些证明命令与回执链路；
实际舵机是否转到目标仍需现场观察。还可在通话中用现有面罩开/合语音口令验证同一条链路。
更换信道由 C3 跟随，AMOLED 继续使用路由器信道，不切走其语音联网信道。

2026-11-26 实机验收：已通过 COM21 更新 C3 应用并校验写入内容，NVS 参数保留。
COM5 运行日志确认 AMOLED 的实际 STA 地址为 `B8:1F:3F:D1:EE:04`；
信道 1 上捕获 10 条 AMOLED A/B 指令与 C3 `peer=2` 回执及发送端确认匹配，
用户现场确认两只舵机均正常完成 B→A。同期 24 份 AEC 状态记录均为 FD_LOW_COST、
`rx_overflows=0`。这是屏幕控制的实机验收，本次未单独验收语音口令。
日志与记录在 `../amoled-1.75/build/servo-link-device.log` 和 `servo-pairing-checks.json`。
所有控制器控制同一组舵机，实际目标以接收端最后收到的有效指令为准。

## 校准遥控上限

1. 先断电拆下连杆，保留舵臂用于空载测角。上电后标记 A 点，并打开 ESP32-C3 的 USB 串口监视器。
2. 输入 `b` 并回车选择两路，或输入 `6`、`7` 选择单路。选中的舵机先移到已校准的 90° 位置。
3. 每次输入 `+` 并回车，向更大角度试调 20 µs，等舵臂停稳后再继续。`-` 并回车回退一档。重新选择舵机可回到 90° 位置。试调的程序边界为 500–2500 µs。若舵臂持续嗡鸣或顶住，停止增加行程并回退。
4. 从实际 A 点量到约 145° 时，记录串口 `Pulse probe GPIO… target … us` 显示的各路脉宽。输入 `q` 并回车恢复屏幕和实体按键控制。
5. 将两路实测值分别填入 ESP32-C3 的 `SERVO_GPIO6_MAX_US` 和 `SERVO_GPIO7_MAX_US` CMake 选项，然后重新编译并更新接收端。两路都完成校准时，屏幕自动开放 0–145°。

可用 `idf.py -D "SERVO_GPIO6_MAX_US=$gpio6Pulse" -D "SERVO_GPIO7_MAX_US=$gpio7Pulse" build` 传入实测值，其中 `$gpio6Pulse` 和 `$gpio7Pulse` 是实测脉宽（微秒）。

在已加载本机 ESP-IDF 6.1 的 PowerShell 中运行：

```powershell
Set-Location 'E:\Deotaland\esp32c3supermini'
idf.py build
idf.py -p COMx flash monitor
```

目标芯片已设为 `esp32c3`。把 `COMx` 换成开发板的实际串口号，`Ctrl+]` 退出监视器。

如果本机的环境激活脚本卡在 `clangd` 检查，可以用现有的 VS Code 环境配置为当前 PowerShell 加载同一套工具。完成后再运行上面的命令。

```powershell
$idfEnvironment = Get-Content 'D:\esp_idf\v6.1\VSCode-Environment.json' -Raw | ConvertFrom-Json
foreach ($setting in $idfEnvironment.PSObject.Properties) {
    if ($setting.Name -ine 'PATH') {
        Set-Item -Path "Env:$($setting.Name)" -Value ([string]$setting.Value)
    }
}
$env:Path = $idfEnvironment.PATH + ';' + $env:Path
```
