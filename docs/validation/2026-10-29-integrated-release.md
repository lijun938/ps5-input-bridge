# 1.1.1：Chiaki-ng 修复与输入延迟优化整合

应用版本 1.1.1，原生公共 ABI 6。当前发行目录为 `dist/PS5InputBridge`，完整分享包为 `dist/PS5InputBridge-win-x64.zip`，内含对应源码包。早先的 1.0.5-latency.1、1.1.0 测试目录保留作历史版本，不参与源码提交。

## 合入内容

- 固定 chiaki-ng main `c8bea2642d0e95402f373af55584a3b98cd89417`，包括 #823 的逐事件发包与事件顺序、#838 的最近事件历史及 20/50/100/200 ms 尾部补发、#840 的正确事件数。
- 首次发送仍在输入线程同步完成；后台线程负责补发和保活。停止时保留 30 ms 松开补发窗口。上游修改使用两组可重复应用的精确补丁，移除旧的整文件发送器覆盖。
- 保留加密上下文复用、小包栈缓冲、HID 预挂读取及同步完成快路径。XInput 100/500/1000 μs 选项不变，HID 按报告处理。
- 默认关闭详细时序统计；可单独开启 XInput 计时。统计窗口使用独立锁和非阻塞发布，读取、切换、松键、失败计数与故障保护保持有效。
- 输入和反馈阶段诊断默认关闭；开启后按有界环缓冲记录，停止后导出。发行包包含普通、计时、诊断三个启动器和 `INPUT-LATENCY-GUIDE.md`。
- 修正发送器回归测试的事件数错误累计，防止前面多键用例的错误被后续用例清零。基准工具补充私有头文件校验，并清除子进程继承的诊断环境变量，避免隐式开启观察开销。

## 本轮验证

`pwsh -NoProfile -File tools/build-native.ps1` 退出 0，11 个原生测试程序全部通过。包括发送器 10/10、唤醒/恢复 4/4、反馈诊断 6/6、输入诊断 5/5、HID 20/20、统计快照 11/11、416 组加密参考向量，以及真实加密 UDP 包事件数和发送失败恢复检查。Python ABI、生命周期与 bootstrap 13/13 通过。

审查修正事件计数断言后，单独重编译发送器测试，再次 10/10 通过；未改动生产发送路径。日志为 `artifacts/integration-1.1.1/native-build.log`、`sender-review-tests.log`。

`pwsh -NoProfile -File tools/publish.ps1 -SkipNativeBuild` 退出 0，包含 .NET Release 84/84、离线 WPF 87 项检查和 Windows x64 自包含发布。日志为 `artifacts/integration-1.1.1/publish.log`。

## 新发送器的基准工具兼容检查

使用真实输入工作线程、会话发送、加密和 UDP 环回，输入源由测试程序合成。仅做短时运行和记录一致性检查，没有运行旧版 A/B 性能实验。

| 路径 | 测量窗口生成数 | 未观察到的合成状态 / FIFO 丢弃 | 前台提交失败 |
| --- | ---: | ---: | ---: |
| XInput 100 μs，native UI，默认统计关闭 | 8000 | 0 | 0 |
| XInput 100 μs，native UI，计时开启 | 8000 | 1869 | 0 |
| XInput 100 μs，WPF UI，默认统计关闭 | 8000 | 7 | 0 |
| XInput 100 μs，WPF UI，计时开启 | 8000 | 93 | 0 |
| HID 合成 1K，稳态 | 500 | 0 | 0 |
| HID 合成 8K，稳态 | 4000 | 0 | 0 |
| 原 8K runner，当前版本 mixed | 800 | 0 | 0 |

HID 两次运行的缺失按钮边沿均为 0。XInput 计时开启的某次运行出现明显调度停顿；该模型只提供最新状态，停顿期间旧状态会被覆盖。记录保留这些未观察状态，不能将“提交失败为零”解释成所有状态都被发送，也不能从这组短测证明真实性能提升或其因果。历史 1.0.5-latency.1 数值不适用于此次更换发送器后的版本。

原始 CSV、JSON、编译输入/输出哈希和链接记录保存在本机 `artifacts/integration-1.1.1/{xinput-smoke,hid-smoke,legacy-runner-smoke}`，不作为 Git 源码提交。复现命令如下；输出目录需要使用尚未存在的新目录：

```powershell
python tools/latency-contention/run.py --versions current-default current-timing --profiles mixed --ui native wpf --pairs 1 --slots 8000 --output artifacts/integration-1.1.1/xinput-smoke
python tools/latency-hid/run.py --versions current --profiles 1000-steady 8000-steady --pairs 1 --seconds 0.5 --output artifacts/integration-1.1.1/hid-smoke
python tools/latency-8k/run.py --versions current --profiles mixed --pairs 1 --slots 800 --output artifacts/integration-1.1.1/legacy-runner-smoke
```

## 实机边界

未测真实 USB、网卡出线、PS5 接收或按键到画面的延迟。上游历史尾部补发取代旧版 2/8/24 ms 策略，摇杆丢包仍依赖约 200 ms 保活；这些是调度目标，不是可靠送达保证。需要在 Xbox/XInput 与 DS/HID 两种模式各确认 L2 松开同时 R2 变化、单次换弹、快速组合键、摇杆回中及断开释放。使用方法见 [整合版说明](2026-10-29-latency-test-guide.md)。
