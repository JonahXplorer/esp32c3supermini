# A / Orbit：状态与代码结构

## 职责

```text
现有 voice.c：通话业务状态机、音频采集与网络恢复（本次未改）
     │ sugarat_voice_view() → screen_state_t.voice
     │ Wi-Fi → screen_state_t.wifi_connected / wifi_setup_active
     │ 现有 voice_playback_level() → 播放读数
     ▼
home_presenter.c：纯函数，将设备快照投影为 UI 快照，集中定义文案和按钮权限
     ▼
home_screen.c：A 方案控件、统一按钮、中央动效，只读快照，不操作音频或网络
     │ home_action_cb_t：按钮意图
     ▼
共享 screen.c：显示页面或返回现有 SCREEN_* 动作
     ▼
原有 main.c：执行发起/结束通话、配网等动作（本次未改）
```

- `home_presenter.h/.c`：全部设备状态映射、播放视觉选择、唯一按钮权限判断。
- `home_screen.h/.c`：不依赖 ESP-IDF 的 LVGL 组件；不会启动/停止麦克风、切换 AEC 或改变业务状态。
- `ui_theme.h`：颜色、字体、布局、触摸范围和动画节奏。
- `../screen_port.c`：原生 RGB565 局部刷新、90° 顺时针旋转及屏幕输入。
- 共享 `sugarat/main/screen.c` 只在此板编译时通过 `SUGARAT_NATIVE_AMOLED_UI` 选择 A 方案；其他板仍使用原有主屏。

## 现有业务状态机

业务状态仍由 `sugarat/components/sugarat_voice/voice.c` 管理，不在 UI 中复制。
内部有 9 个模式；它结合平台和网络状态，对外提供 16 个 `sugarat_voice_phase_t`。
内部 `IDLE` 仅表示没有进行中的通话，不等于已联网或已就绪。

```mermaid
stateDiagram-v2
    [*] --> BOOT
    BOOT --> IDLE: 初始化成功
    BOOT --> FAULT: 初始化失败
    IDLE --> STARTING: 唤醒或点击且语音连接已就绪
    STARTING --> CONNECTING: 请求开启通话
    CONNECTING --> ACTIVE: 语音流打开
    CONNECTING --> ENDING: 取消、失败或20秒超时
    ACTIVE --> RECOVERING: 连接暂时中断
    RECOVERING --> ACTIVE: 恢复并打开语音流
    ACTIVE --> ENDING: 挂断或会话终止
    RECOVERING --> ENDING: 挂断或连接终止
    ENDING --> IDLE: 清理完成
    IDLE --> MANAGING: 新建、选择或删除会话
    MANAGING --> IDLE: 操作完成
```

## 16 个设备状态的显示映射

| 设备 phase | 主标题 | 中央视觉 | 主操作 |
| --- | --- | --- | --- |
| BOOT | Starting | 连接进度圆弧 | Waiting，禁用 |
| SETUP | Wi-Fi setup | 断开的圆环与 Wi-Fi 图标 | Wi-Fi；热点已开时禁用 |
| JOINING | Joining Wi-Fi | 连接进度圆弧 | Waiting，禁用 |
| PLATFORM_CONNECTING | Connecting | 连接进度圆弧 | Waiting，禁用 |
| PLATFORM_RETRYING | Offline | 断开的圆环与 Wi-Fi 图标 | Settings |
| ACTIVATE | Activate | 真实六位激活码 | Waiting，禁用 |
| BIND_AGENT | Choose agent | 静态圆环 | Waiting，禁用；提示在平台绑定 |
| PREPARING | Preparing | 连接进度圆弧 | Waiting，禁用 |
| READY | Ready | 静态圆环 | Start call |
| OPENING | Listening | 连接进度圆弧 | End call，受 can_end 控制 |
| IN_CALL | Listening | 麦克风波形；播放时为外扩圆波 | End call |
| RECONNECTING | 通话内为 Listening；空闲连接恢复为 Reconnecting | 连接进度圆弧 | 通话内 End call；空闲时 Settings |
| ENDING | Ending call | 连接进度圆弧 | Waiting，禁用 |
| ERROR | Voice error | 断开的圆环 | Settings，提示重启设备 |
| RETRY | Interrupted | 连接进度圆弧 | Settings |
| NO_SESSION | No session | 静态圆环 | Sessions |

所有按钮都由一个 `action_enabled()` 判定是否可用：触摸不可用时禁用；
开始通话须为 READY；结束通话须由现有 `can_end` 允许；
配网须无通话且热点尚未开启；打开会话列表须已绑定 Agent 且无通话。
主屏控件和点击分发都使用同一份判定结果，主应用仍保留原有业务保护。

## 通话、播放与开麦

IN_CALL 内部只有“输入波形/播放圆波”两种绘图选择，不新增业务状态。
两者主标题都是 **Listening**，提示都是 **Microphone is on**，主操作都是 **End call**。
播放检测使用已有 `voice_playback_level()`，不使用 AEC 设备中刻意固定为 false 的 `voice.speaking`。
麦克风音量仍来自现有 AEC 输出，0–100 读数仅在 UI 显示时映射为 0–20。

不根据静音推测“AI 思考”。PREPARING/连接圆弧仅代表连接准备。
播放、UI 状态切换、菜单导航都不改变麦克风或 AEC；保留现有 FD_LOW_COST 全双工采集。
现有重连期间仍读麦，但 RECOVERING 不将帧加入上行队列；本次不改变该业务行为。

## 显示和验收

- 主屏原生 466×466，纯黑；其他原有页面保持 360×360，居中于原生画布。
- 90° 顺时针旋转和触摸逆变换仍匹配，CO5300 的 2×2 刷新窗口保留边界像素。
- 中央动画最多每 80 ms 更新一次，仅使中央区域失效；离开主屏暂停动画。
- 播放切换不重绘标题、提示或按钮；原生 LVGL 渲染验证约 20.1% 画布更新。
- 没有真实电量接口时隐藏电量，正式固件不显示预览的 74%。
- `ui-preview/render.ps1` 使用实际 `home_presenter.c` 和 `home_screen.c`，模拟现有设备快照，
  渲染全部 16 个 phase，检查按钮保护、原生尺寸、圆屏边缘和播放切换的刷新范围。
- `tests/run-host-tests.ps1` 验证全部 217156 个显示/触摸位置以及既有持续采集与 AEC 调度。

原生 PNG、播放动效和检查结果在 `ui-preview/renders/device-states/`。
设备上的触摸手感、声学回声消除和实际功耗仍需实机验收。
