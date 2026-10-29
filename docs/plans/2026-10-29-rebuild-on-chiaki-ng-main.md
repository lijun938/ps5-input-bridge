> 本文为压缩包的原始交接方案。本地合入额外保留 HID、统计快照、加密优化和可选诊断；上游修改合并为互不重叠、可重复应用的精确补丁，分布于 chiaki-edits.json 和 chiaki-low-latency.json。Windows 验证结果见 [本地合入记录](../validation/2026-10-29-chiaki-main-import.md)。

# 1.1.0：改为基于 chiaki-ng 最新 main 重建

## 为什么重建

现场问题：Xbox 手柄和 DS 模式手柄都会偶发"松开左扳机后游戏里一直开镜"，按几次左扳机才恢复；也出现过换弹要按两下。

原因：发给 PS5 的按键包（feedback history）包头第 3 字节表示"包内有几个事件"，旧版一直写 0。写 0 时 PS5 只认包里第一个（最新的）事件。1.0.x 的发送器是自己重写的，会把同一次采样里变化的所有键塞进一个包，并且 R2 总排在 L2 前面。所以"松开 L2 的同时 R2 在变"时，L2 的松开被丢掉，之后的补发内容也一样，补不回来。

chiaki-ng 在 2026-10-09 合入了三项修复，其中 #840 在 PS5（GT7 油门/刹车）上实测过：count=0 时 L2 失灵、R2 正常；填上事件数后两个都正常。

- #823：每个事件单独一个包（顺序：松开 → 触摸 → 按下 → L2 → R2）。
- #838：最后一个包在 20/50/100/200 ms 后重发，每个事件包附带最近 8 个事件。
- #840：包头写入事件数。

1.0.x 用整文件替换的方式改了上游发送器，所以拿不到这些修复。1.1.0 改为直接使用上游，只保留最少的修改。

## 改了什么

| 位置 | 改动 |
|---|---|
| `native/scripts/bootstrap.py` | chiaki-ng 固定到 main `c8bea2642d`（子模块版本与之前相同）。下载包按 URL 区分缓存；固定版本变化时删掉旧源码重新解压，不会沿用 v1.10.0 的旧文件。跳过 chiaki-ng 中 Switch 资源的悬空符号链接。 |
| `native/patches/chiaki/`（已删除） | 不再整文件替换上游代码。 |
| `native/patches/chiaki-edits.json` | 全部上游修改都在这里，共 9 处精确修改。原有 2 处：`session.c`、`takion.c` 返回发送错误。新增 7 处：上游发送器改为由调用线程（输入线程）直接发送，发送线程只负责重发和保活，两边都持同一把锁发送，包不会交错。 |
| `native/src/ps5bridge.c` | 停止时等待 30 ms（原 10 ms），让最后一个"全部松开"包的第一次重发（20 ms）发出去。更新了相关注释。 |
| `native/tests/test_feedback_sender.c` | 按新行为重写。用两种接收模型回放实际发出的包：(1) 每包只认第一个事件（#823 观察到的 count=0 行为）；(2) 按包头事件数应用（#840）。两种模型下都必须正确。 |
| `native/tests/test_transport_errors.c` | 适配 `chiaki_takion_send_feedback_history` 新增的 `event_count` 参数。 |
| `native/tests/test_bootstrap.py` | 新增：固定版本变化时旧源码被替换、旧下载不被复用。 |
| `src/Ps5Bridge.App/Ps5Bridge.App.csproj` | 版本号 1.1.0。原生 ABI 仍为 6，接口未变。 |
| `README.md`、`native/README.md`、`THIRD-PARTY-NOTICES.md` | 按新行为更新。 |

## 和 1.0.x 行为上的区别

- **同时变化的多个键**：1.0.x 合成一个包；1.1.0 每个键一个包，包头带事件数。这是修复的核心。
- **按键丢包恢复**：1.0.x 在 2/8/24 ms 补发；1.1.0 按上游在 20/50/100/200 ms 重发。单次丢包的恢复时间从约 2 ms 变成约 20 ms。
- **摇杆回中包丢失**：1.0.x 在 2/8/24 ms 补发；1.1.0 只靠 200 ms 保活，最坏约 200 ms 才恢复。如果实测觉得明显，可以再加一处小修改补回摇杆的快速重发。
- **发送延迟**：不变，仍由输入线程读到即发。

## 构建（交给构建的人）

步骤和 1.0.x 相同，需要 Windows、.NET 9 SDK、Python 3.11+、PowerShell 7、首次构建联网：

```powershell
pwsh -NoProfile -File tools/build-native.ps1
dotnet test tests/Ps5Bridge.Tests/Ps5Bridge.Tests.csproj -c Release
dotnet run --project tests/Ps5Bridge.UiChecks/UiChecks.csproj -c Release
pwsh -NoProfile -File tools/publish.ps1 -SkipNativeBuild
```

如果这台机器构建过 1.0.x，`native/.deps` 和 `third_party/chiaki/source` 里的旧文件会被自动替换，不用手动删除。

可能遇到的问题：

- chiaki-ng main 比 v1.10.0 多编译了 `lib/src/aia.c`（证书链恢复，依赖 curl）。在本项目的 mbedTLS 配置下它只编译空实现，curl 本来就已编入；如有链接错误，先看这里。
- bootstrap 报 "Pinned upstream patch no longer matches"，说明上游源码和 `chiaki-edits.json` 对不上，不要跳过，把报错发回来。

## 交付前已验证（在 macOS 上，不是 Windows 构建）

- 9 处修改在官方 `c8bea2642d` 源码上全部应用成功，重复应用无变化；用真实的 bootstrap 下载、解压、打补丁走了一遍，结果一致。
- 修改后的 `feedbacksender.c` 在 clang `-Wall -Wextra -Werror` 下语法检查通过。
- `test_feedback_sender.c` 通过 POSIX 兼容层在 macOS 上实际运行，10/10 通过：L2 松开与 R2 同时变化、扳机扫动中按换弹、18 个键同时松开、10 ms 和 150 ms 丢包恢复（约 23 ms / 约 202 ms）、本地发送错误、包头事件数与内容一致等。
- `test_bootstrap.py` 在 Python 3.12 下 3/3 通过。

## 没有验证、需要在 Windows 和 PS5 上确认

- Windows 完整构建，以及 `ps5bridge-tests`、`ps5bridge-input-tests`、`ps5bridge-transport-tests`、`test_abi.py`、.NET 测试和 UI 检查。
- **实机**：Apex 训练场，Xbox 手柄和 DS 手柄各测一遍：
  1. 开镜（L2）后边开火（R2）边松开 L2，重复 30 次以上，看是否还会卡开镜。
  2. 打空弹匣后按一次 □ 换弹，重复 20 次，看是否都一次生效。
  3. 快速连按 L1/R1、拨十字键，看有无漏键。
  4. 摇杆快速甩动后松开，看视角会不会继续转。
- 有条件的话抓包确认：按键包包头第 3 字节等于包内事件数，状态包为 1。
