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

默认使用 50 Hz PWM，A 点是 1000 µs，B 点是 2000 µs。程序每 10 ms 将当前脉宽向目标推进最多 10 µs，两端之间单程约 1 秒。实际摆角取决于舵机和机构，请先断电拆开连杆，空载确认两端的行程。若某一端顶到限位或持续嗡鸣，将该端的脉宽往中间收一些。

在 `main/main.c` 调整 `SERVO_POSITION_A_US` 和 `SERVO_POSITION_B_US` 可校准两个角度。`SERVO_MOVE_STEP_US` 决定切换速度。

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
