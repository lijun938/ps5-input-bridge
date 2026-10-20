# 本地验证记录

日期：2026-10-09（Asia/Shanghai）。目标：Windows x64。交付：`dist/PS5InputBridge/Ps5InputBridge.exe` 与 `dist/PS5InputBridge-source.zip`。

## 已执行

- `tools/build-native.ps1`：原生 Release DLL 构建成功；本项目 C 源码启用 `-Wall -Wextra -Werror`。固定的第三方源码存在 CMake 兼容性提示，不影响构建。
- 原生 C 自检：按钮/扳机/摇杆转换、Y 轴边界、断开后中立状态与结构布局通过。
- `python native/tests/test_abi.py`：4 项通过，加载实际 DLL；涵盖 20 轮创建停止销毁、注册取消、同上下文反复取消和并发状态读取、模拟主机不应答时停止及线程计数停止增长。
- `python native/tests/test_bootstrap.py`：1 项通过；HTTP 下载中断不会留下可用的半截缓存，重试成功。
- `dotnet test tests/Ps5Bridge.Tests/Ps5Bridge.Tests.csproj -c Release`：24 项通过；涵盖账户密文保存与损坏处理、DPAPI、会话停止顺序/取消/切换失败、原生 P/Invoke ABI、真实本地 TCP 连接取消。
- WPF Release 严格构建：0 警告、0 错误。Core / App / tests 格式检查通过。
- UI：本地布局渲染、无账户启动、不自动连接、配对输入校验、4 位登录密码校验、缺 DLL 错误和关闭路径经过测试；最终默认采集档为 500 μs。
- 发布后实际 EXE 创建主窗口并正常关闭，退出码 0；原生 DLL 与最终构建版本的哈希一致。
- 源码包包含本项目、构建脚本、许可及原生依赖源码压缩包；发布包含 .NET / Windows Desktop 运行时。

## 评审修复

状态通知订阅者异常不能阻止停止旧会话；临时文件清理失败也必须释放账户存储锁；取消连接不得在调用方 UI 线程同步等待原生停止；已取消的旧选择不得取消更新的连接请求。上述问题已通过针对性复现和回归验证处理。

## 未验证

没有本次用户的 PS5 Pro、主机地址、PSN Account-ID 或设备配对码，因此未进行真实主机注册、连接、账户切换或游戏输入验证。没有真实控制器电气计时与电视响应数据，因此未验证端到端延迟、长时间稳定性、漏键或卡键，也不能宣布比 Wingman FGC 2 快。

500 μs 是输入线程请求间隔，不是实际 USB 轮询率或端到端延迟。客户端统计不包含采样等待、手柄、独立反馈线程发送、网络、PS5 或电视延迟。禁用音视频选项不等于已确认主机完全停发媒体；仍需实机抓包。
