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

[Sugarat Button 遥控](../sugarat/README.md)通过 ESP-NOW 切换 A/B 和调整两路的速度及 B 点角度。C3 按已配对的 Sugarat 地址识别数据，并在失联后重新寻找 Sugarat 所在的 2.4 GHz 信道。GPIO4 实体按键使用同一组设定。接收端每次重启恢复默认速度和 90°。

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
