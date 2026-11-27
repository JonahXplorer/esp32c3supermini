# A / Orbit 原生 UI 渲染

AMOLED 和 Sugarat 固件共用 `../../sugarat/main/ui/` 下的 Orbit 主屏和状态映射。
状态与模块职责详见 [UI 状态说明](../main/ui/README.md)。

## 运行

在仓库根目录执行：

```powershell
& .\amoled-1.75\ui-preview\render.ps1
```

加 `-Board Sugarat` 可渲染 360×360 LCD 版本。Sugarat 的预览额外模拟点击主屏顶部、进入设置、切换到手动控制和网络确认。输出位于 `../../sugarat/build/ui-preview-renders/`。

脚本复用 `../build/ui-preview/`，用现有 MSVC、Windows SDK、ESP-IDF CMake/Ninja，
以 2 个编译任务直接构建项目下载的 LVGL 9.2.2 源码。Python Pillow 将真实 RGB565 帧
无缩放转为 PNG/GIF。硬件数据仅在此渲染程序中模拟；激活码 582041 为演示值。
没有复制 LVGL 源码或应用仓库，没有运行第二个固件构建。

## 当前输出

`renders/device-states/` 内：

- `device_00_boot.png` 至 `device_15_no_session.png`：全部 16 个设备 phase，每张 466×466。
- `device_listening.png`、`device_speaking.png`：通话内麦克风输入和扬声器播放，主标题均为 Listening。
- `device_button_pressed.png`、`device_button_disabled.png`：真实按钮按下和禁用状态。
- `A-device-states-native.png`：16 状态总览，每个圆屏保持原生像素。
- `A-listening-speaking.gif`：Listening 内输入波形与播放圆波动效，466×466、约 30 fps。
  模拟时钟按 33 ms 推进；GIF 用 30/30/40 ms 循环适配其 10 ms 时间精度，设备实际帧率以串口统计为准。
- `device-states.csv`：实际映射出的文案、按钮可用性与视觉状态。
- `device-refresh.csv`、`render-report.json`：全帧刷新与截图验证结果。

渲染程序使用生产代码验证每个 phase 都有展示、按钮权限遵守原接口、
触摸不可用时不派发动作、播放时文字与控件保持相同、切换仅改变中央像素、
离开主屏暂停动画、未知 phase 安全展示。打包步骤检查圆屏边缘、静态状态保留、
文字对比度及原生图像尺寸。渲染程序与固件均使用 DIRECT 模式保留静态像素，发生变化时提交完整
466×466 帧；中央视觉变化不等于局部物理传输。这些结果不替代设备实际触摸、音频和功耗验收。

上一级 `renders/` 中的 A/B 对比及含思考的旧预览为选型记录，不代表当前固件行为；
当前代码只保留 A 方案，不实现 AI 思考状态，也不显示模拟电量。
