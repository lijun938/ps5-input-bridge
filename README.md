# PS5 Input Bridge

Windows x64 局域网手柄桥接原型。**手柄接 PC，画面看 PS5 / PS5 Pro 直连电视。**

原生 libchiaki 处理配对和 Remote Play 输入，独立线程直接采集 XInput 手柄；WPF 界面只负责账户和连接。普通音视频处理关闭，没有视频解码、显示、音频播放或麦克风采集。它不是 Sony 官方客户端。

## 启动与配对

1. 在 PC 启动 `dist/PS5InputBridge/Ps5InputBridge.exe`。发行目录包含 .NET 运行时，不需要另外安装；请保持整个目录完整，不要只复制 exe。
2. PC 与 PS5 接同一有线局域网，PS5 保持开机。手柄接 PC。两种接法：**Xbox / XInput / PC 模式**（四个 XInput 槽位），或 **DS / PS4 / PS5 模式**（DualShock 4、DualSense 以及用这两种报告格式的第三方手柄，程序以 HID 直读；手柄列表中显示为“HID 直读”）。HID 直读延迟最低并支持触摸板滑动，有 DS 模式的手柄优先用它；不支持其他 DirectInput 手柄。没有手柄时也可以连接，随后插入手柄。
3. 在 PS5 登录要控制的账户，打开 **设置 → 系统 → 远程游玩 → 启用远程游玩**。在网络设置查看主机局域网 IP。
4. 点击程序的 **添加并配对**，填写账户显示名称、主机 IP、该 PSN 账户的 **Account-ID（Base64）**。
5. 在 PS5 的远程游玩设置选择 **关联设备**，将新显示的 **8 位配对码**填入程序并配对。每个 PSN 账户完成一次；不需要每次切换重复配对。

Account-ID 是 8 字节账户编号的 Base64 编码，**不是 PSN 昵称、邮箱或密码**。如果已有 Chiaki 配对资料，可以从其注册界面查到。没有时，可按 [chiaki-ng 官方注册指南](https://streetpea.github.io/chiaki-ng/setup/configuration/#registering-your-playstation) 的 Account-ID 方法获取，或使用固定版本的[上游账户编号脚本](https://github.com/chiaki-ng/chiaki-ng/blob/0c4a45df0cae2af2ba2daef84e881850b07038a3/scripts/psn-account-id.py)。在浏览器的 Sony 登录页面操作；本程序的配对表单不要求 PSN 密码。不要将含授权码的跳转网址或访问令牌发送给他人。

**换一台电脑**：在原来那台点 **导出账户文件**，设置一个至少 8 位的密码，得到一个 `.bridge` 文件；把整个 `PS5InputBridge` 目录和这个文件拷到新电脑，打开程序点 **导入账户文件** 并输入同一密码即可，不需要重新配对。文件用 PBKDF2（30 万次迭代）派生密钥加 AES-GCM 加密，但**它等同于你的配对凭据，任何拿到文件和密码的人都能连上你的主机**，用完请删除或妥善保管。新电脑仍需与 PS5 在同一局域网，若主机 IP 变了，导入后账户会连不上，需要重新导入或重新配对。

**已在 Helios 配对过**：点 **从 Helios 导入**，程序读取同一 Windows 用户下 Helios 的 PS Remote Play 账户（`%LOCALAPPDATA%\HeliosProject\Helios\remote_play\playstation\credentials.dat`），直接沿用其配对凭据，不需要重新配对。只导入 PS5 账户；相同配对凭据已存在的会跳过。Helios 不保存 Base64 Account-ID，导入的账户不带 Account-ID：局域网连接只用注册密钥和 morning 密钥认证，不受影响；以后若要重新配对，再按下文获取 Account-ID。凭据只在本机内存中解密并立即重新加密保存，不会显示或上传。

账户配对成功才会保存。文件位于 `%LOCALAPPDATA%\PS5InputBridge\accounts.dat`，使用当前 Windows 用户的 DPAPI 加密；复制到另一 Windows 用户通常不能解密。程序不保存 PSN 密码或主机 4 位登录密码。

## 使用与切换账户

- 选择已保存账户和手柄后点 **连接**；只有主机报告完成连接才显示“已连接”。
- 连接过程中选择另一个账户，会先释放按键并结束旧会话，再使用新账户凭据连接。一次控制一个账户；实际连接速度由 PS5 会话握手决定。
- **Ctrl + 1…9** 按列表顺序选择并连接账户；**Ctrl + D** 断开。快捷键在程序窗口有焦点时生效。
- 账户有 PS5 **4 位主机登录密码**时，连接会弹窗请求，不会保存。它不同于 PSN 密码和 8 位设备配对码。
- HID 直读手柄的全部按键（含 PS、Create/Share、触摸板点击和双指触摸滑动）直接转发，不需要映射；Back 键映射选项只对 XInput 手柄生效。蓝牙连接的 DS4 / DualSense 也能直读（程序会切换到完整报告），但蓝牙本身的延迟和抖动高于有线。
- PS 键、触摸板点击可通过界面按钮触发。XInput 的 A/B/X/Y 分别映射到叉/圈/方块/三角，Start 映射 Options，手柄 Home / Xbox 键映射 PS 键（通过 XInput 扩展接口读取；Windows Xbox Game Bar 若占用该键，请在“设置 → 游戏 → Xbox Game Bar”关闭“使用控制器上的此按钮打开”）。Back / View 键默认映射**触摸板点击**（PS5 游戏里的地图、记分板，如 Apex 的地图）；需要截图、录屏时可在界面改为“分享 / Create”，连接中切换立即生效。
- 断开和关窗释放输入并结束会话，不要求主机待机。网络已经中断时无法保证释放报文送达主机。

如果主机 DHCP 地址改变，需要用新的主机地址重新添加配对账户。已有凭据不会自动转移到另一个主机或账户。

## 延迟目标与统计

**HID 直读手柄没有采样间隔**：输入线程阻塞等待 USB 报告，报告到达即解析、加密并发出（本机实测从报告到达到 UDP 交给系统约 0.4 μs，不含 USB 本身）；采样间隔选项对它无效。有线 DS4 / DualSense 的 USB 报告率为 1000 Hz（本机实测 DS4 相邻报告中位 1.000 ms），这是延迟下限。XInput 手柄默认 **500 μs（0.5 ms）**采集间隔；**100 μs 极限档**在手柄有输入变化时忙等计时，停止操作约 200 ms 后自动切回低耗等待，因此只在真正操作时占用 CPU（极限档下输入线程降为低于普通优先级，不会压住其他程序）；**1000 μs** 档降低 CPU 占用。Windows 定时器只能按约 0.5 ms 的节拍唤醒，程序已按实测补偿，500 μs 档实际约 0.5 ms（修正前实际约 1 ms）。这是 PC 读取手柄的间隔，不代表手柄 USB 回报率或端到端延迟。系统负载、调度、XInput 驱动和手柄本身仍会影响实际间隔。

输入线程检测到变化后**在同一线程内直接加密并发出 UDP 包**，不再排队等待 libchiaki 的反馈线程；每次采样只要状态变化（按键、扳机、摇杆）就立即发送，不做合并。按键事件包按控件折叠（每个控件只带当前值），连续扳机变化不会把换弹等按键挤出数据包；每次变化后约 2 / 8 / 24 ms 原样补发同一个包，单个 UDP 丢包可在约 2 ms 内恢复。摇杆没有附加曲线、死区或人为平滑；输入线程注册 MMCSS “Pro Audio” 调度。

现场排查漏键：启动前设置环境变量 `PS5BRIDGE_TRACE=1`，断开后会在 `%LOCALAPPDATA%\PS5InputBridge\trace-*.csv` 写出每次按键/扳机边沿的时间与提交结果。

界面的“PC 采集 → 提交”测 **XInput 取到状态（或 HID 报告到达）后，到加密并交给系统发出 UDP 包返回**的耗时。它不包含手柄/USB 延迟、等待采样、网络、PS5 处理和电视显示。报告次数是提交次数，不是 UDP 包数（补发包不计）。

禁用音视频使用上游选项；主机实际是否完全停发媒体尚未实机抓包验证。**本原型尚未证明延迟不高于 Brook Wingman FGC 2。** 对照方法及证据见 [研究记录](docs/research/2026-10-09-ps5-pro-input-bridge.md)。

## 开发、构建与本地测试

需要 .NET 9 SDK、Python 3.11+、PowerShell 7。首次原生构建需要联网下载项目内工具链和固定依赖，之后可复用缓存；无需安装系统 C/C++ 编译器。

```powershell
pwsh -NoProfile -File tools/build-native.ps1
dotnet test tests/Ps5Bridge.Tests/Ps5Bridge.Tests.csproj -c Release
pwsh -NoProfile -File tools/publish.ps1 -SkipNativeBuild
```

账户切换/取消/存储测试使用可控后端；DPAPI 测试使用真实 Windows 加密接口；原生 ABI 测试加载实际 DLL，并测试无有效主机、真实注册取消、循环创建销毁及本地 TCP 模拟主机不回应时的停止路径。UI 已做本地启动、布局和关闭验证。

没有连接用户的真实 PS5 Pro，因此 **实机配对、账户切换、输入正确性、漏键/卡键、完全停传媒体及实际端到端延迟**仍需现场测试。自动化本地测试不能代替这些验证。

## 源码与许可

基于固定的 chiaki-ng v1.10.0 提交。**修改了上游反馈发送器**（`lib/src/feedbacksender.c`、`lib/include/chiaki/feedbacksender.h`）：调用线程直接发送、按控件折叠的按键历史、丢包补发。修改后的完整文件在 `native/patches/chiaki/`，构建时由 `native/scripts/bootstrap.py` 覆盖到上游源码；源码与构建依赖列表也在该脚本中；具体原生接口说明在 `native/README.md`。本项目按 AGPL-3.0 分发；发布包同时保留源码构建信息和第三方许可，`dist/PS5InputBridge-source.zip` 包含本项目源码及原生依赖源码压缩包。详见 `LICENSE` 和 `THIRD-PARTY-NOTICES.md`。
