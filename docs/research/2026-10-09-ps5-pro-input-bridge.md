# PS5 Pro 局域网手柄桥接：可行性与延迟核验

核验日期：2026-10-09。目标 PC：Windows。用户已有对照设备：**Brook Wingman FGC 2**；手柄型号尚未提供。

## 需求

第三方手柄接 PC，PC 经有线局域网向 PS5 Pro 发送 Remote Play 输入；用户直接观看 PS5 Pro 经 HDMI 输出到电视的画面。PC 不承担观看串流的用途。音视频处理应尽量省掉，优先降低输入延迟和抖动。

开发的前提是核验这种路径是否能达到不高于 Wingman FGC 2 的输入延迟。把网络 ping、串流画面延迟、PC 收到输入到发包的时间，都当成完整输入延迟，会得出错误结论。

## 当前结论

1. **技术路线可行，且已有相近的现成实现。** XIM MATRIX 官方文档描述通过有线局域网 Remote Play 控制 PS5，画面继续在主机连接的电视上显示。chiaki-ng v1.10.0 已有禁用音视频的设置，手柄输入发送路径独立于视频解码。
2. **不能把客户端“禁用音视频”说成主机“完全停止传输音视频”。** 检查的 chiaki-ng 代码是在收到媒体包后丢弃；音频分支还保留触觉数据。不能由此证明主机编码器停工或媒体带宽降为零。
3. **尚未证明用户这台 Windows PC 的实现不慢于 Wingman FGC 2。** 专用设备的宣传或测量不能直接套用到 Windows PC。最终需要同一手柄、同一 PS5 Pro、同一游戏和同一电视进行 A/B 测试。
4. **PS5 Pro 兼容有社区连接成功记录。** 这属于兼容性证据，不属于对用户环境的验证，也不是延迟数据。

## 已核验来源

### Sony：PC 可以通过 Remote Play 控制 PS5

[Sony 官方 PC/Mac Remote Play 指南](https://www.playstation.com/en-us/support/games/playstation-remote-play-on-pc-and-mac/)描述 Windows PC 控制 PS5 的使用方法。官方客户端列出的支持手柄是 DualSense、DualSense Edge 和 DUALSHOCK 4，不能把它解释成任意第三方手柄均受官方客户端支持。

### Brook：对照设备确认为 FGC 2

[Wingman FGC 2 官方产品页](https://www.brookaccessory.com/products/wingmanfgc2/index.html)声明支持所有 PS5 游戏，并列出有线手柄兼容范围。页面的“Ultra-Low Latency”没有提供可据此比较 PC Remote Play 的定量方法或数据。

[初代 FGC 产品页](https://www.brookaccessory.com/products/wingmanfgc/index.html)声明主要支持 PS5 格斗游戏；初代测试数据与兼容范围不能直接替代 FGC 2。

### XIM：存在输入走局域网、画面走电视的产品路线

[XIM MATRIX 官方 PS5 指南](https://guide.xim.tech/Gaming-On-PlayStation-5/)明确写道：

> Connection to your PlayStation 5 is through your local network using a wired Ethernet connection via the Remote Play system.
>
> It is equivalent to USB speeds and latency.
>
> Your network is only used for input. Video is displayed as normal through your connected monitor or TV.

指南建议 PS5 也使用有线网络。这证明存在与需求相近的产品实现与厂商声明；没有附带独立测量方法，不能单凭这几句话证明 PC 达到同样的延迟。

### chiaki-ng：发布版已有仅控制用途的设置

[v1.10.0 发布页](https://github.com/chiaki-ng/chiaki-ng/releases/tag/v1.10.0)，发布时间 2026-04-03，提供 Windows portable 包。

以下源码固定在 v1.10.0 对应提交 `0c4a45df0cae2af2ba2daef84e881850b07038a3`，避免以后分支变动导致引用失真：

- [SettingsDialog.qml:553](https://github.com/chiaki-ng/chiaki-ng/blob/0c4a45df0cae2af2ba2daef84e881850b07038a3/gui/src/qml/SettingsDialog.qml#L553)：Settings → General 的 Audio/Video 下拉框提供 **Audio and Video Disabled**。
- [takion.c:1636](https://github.com/chiaki-ng/chiaki-ng/blob/0c4a45df0cae2af2ba2daef84e881850b07038a3/lib/src/takion.c#L1636)：禁用视频时，对收到的视频包 `free(buf)` 后返回。
- [takion.c:1650](https://github.com/chiaki-ng/chiaki-ng/blob/0c4a45df0cae2af2ba2daef84e881850b07038a3/lib/src/takion.c#L1650)：禁用普通音频时丢包；`!packet.is_haptics` 条件意味着触觉包保留。
- [session.c:258](https://github.com/chiaki-ng/chiaki-ng/blob/0c4a45df0cae2af2ba2daef84e881850b07038a3/lib/src/session.c#L258)：禁用视频时强制采用 360p 预设和 FPS 参数 0；[预设实现:97](https://github.com/chiaki-ng/chiaki-ng/blob/0c4a45df0cae2af2ba2daef84e881850b07038a3/lib/src/session.c#L97)和 [FPS 默认分支:132](https://github.com/chiaki-ng/chiaki-ng/blob/0c4a45df0cae2af2ba2daef84e881850b07038a3/lib/src/session.c#L132)对应 `640×360`、`2000 kbps`、`max_fps=0`。这是客户端请求参数，不能直接等同于主机实际视频帧率或实测带宽；是否因此完全停传需要抓包验证。
- [feedbacksender.c:77](https://github.com/chiaki-ng/chiaki-ng/blob/0c4a45df0cae2af2ba2daef84e881850b07038a3/lib/src/feedbacksender.c#L77)：状态更新与反馈历史处理，随后发信号唤醒发送线程。
- [feedbacksender.c:274](https://github.com/chiaki-ng/chiaki-ng/blob/0c4a45df0cae2af2ba2daef84e881850b07038a3/lib/src/feedbacksender.c#L274)：独立反馈发送线程；状态变化会触发发送，不要求等待视频帧解码。
- [controllermanager.cpp:131](https://github.com/chiaki-ng/chiaki-ng/blob/0c4a45df0cae2af2ba2daef84e881850b07038a3/gui/src/controllermanager.cpp#L131)：GUI 客户端配置 4 ms 的 SDL 事件检查定时器。这是软件计划轮询间隔，不能说真实稳定 250 Hz，更不能说端到端延迟为 4 ms。

代码中的 `FEEDBACK_STATE_TIMEOUT_MIN_MS` 注释尚为 TODO，不能据此宣称实际输入固定每 8 ms 发送一次，或协议固有上限只有 125 Hz。发送线程独立也不等于整个 Windows 客户端输入采集没有其他等待。

[维护者对 Controller Only 请求的答复](https://github.com/chiaki-ng/chiaki-ng/issues/674#issuecomment-3533994054)可用于核对功能背景。

[PS5 Pro 社区连接成功报告](https://github.com/chiaki-ng/chiaki-ng/issues/404#issuecomment-2463217222)可用于核对机型兼容背景。

## 找到但未核验实际结果的对比视频

作者 Agent Preston47 有两个与问题直接相关的视频：

- [Brook Wingman FGC2 Input Lag Test vs Xim Matrix Remote Play](https://www.youtube.com/watch?v=RmQXJ9lnnSM)，页面英文日期为 2024-12-10，中文界面日期为 2024-12-11，显示日期存在时区差异。简介列出的章节包括 1:42 与 MATRIX Remote Play 对比、9:47 与 P5Mate 对比、10:48 结论。
- [The Ultimate (PS5) USB vs Remote Play Input Lag - Ft. Xim Matrix](https://www.youtube.com/watch?v=AA-Ban4mLkA)，发布于 2023-07-28。章节包括 3:39 MATRIX Remote Play 与 MATRIX USB 对比、7:01 PSRC 与 USB 对比。

本次读取到了标题、作者、日期和章节；实际播放被 YouTube 登录/反机器人验证阻断，字幕读取也未成功。**没有核验视频中的数值、控制变量、样本数或结论，不能写成“视频证明了 Remote Play 更快”。** 它们只是有针对性的后续核验线索。即使专用 XIM 硬件测得很好，也仍需另外验证 Windows PC 输入采集与调度造成的差异。

## 对比验证方法

推荐先利用 chiaki-ng 现有禁用音视频功能，确认本地输入路径和手柄兼容性，再决定专用工具的开发范围。先做实际试验可以避免为了已经存在的功能重写注册、认证和传输协议。

两条路径：

- A：同一手柄 → Wingman FGC 2 → PS5 Pro → HDMI → 同一电视。
- B：同一手柄 → Windows PC → 同一有线局域网 → PS5 Pro → HDMI → 同一电视。

两组使用同一游戏、相同帧率、相同电视游戏模式和相同控制设置，交替分组测试。PC 和 PS5 Pro 建议接同一交换机；手柄先使用有线连接。不要在 B 组读取 PC 串流画面计时。

测量起点应尽可能是电气按键触发或可靠的同步指示灯，终点是电视上同一个明确视觉响应。PC 日志只能测客户端处理片段；网络 RTT 只能说明网络状况；二者均不能代替终点测量。

可先每组记录至少 200 次作筛查，比较均值、中位数、P95 与分布，另记漏输入、断连及按键是否正常释放。评价 P99 等极端尾延迟需要更多样本。测试后应给出差值和测量误差，而不是只写“体感没有延迟”。

240 fps 摄像的单帧间隔约 4.17 ms，适合发现帧级别差异；不能单凭它可靠证明亚毫秒差异。更严格的对比需要更高帧率或光电测量，并说明实验误差。不要将手指开始移动当成准确的按键触发时间。

[inputlag.science 测试方法](https://inputlag.science/controller/methodology)说明了电气触发、USB 状态测量、LED/摄像等方法。USB 设备侧测试只覆盖输入链路的一部分；本需求的两条路径应另外比较 PS5 上的实际响应。

在没有本地数据之前，合适的结论是“值得验证、可以实现”，不能写“已经证明 PC 比 FGC 2 更快”。
