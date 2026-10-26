# Chiaki / Chiaki-ng 导入验证

日期：2026-10-21（Asia/Shanghai）。目标：Windows x64 桌面版。

## 实现范围

- 新增独立“从 Chiaki 导入”入口：读取同 Windows 用户的 Chiaki 默认配置及具名配置，也支持选择 Qt INI / CONF 文件。
- 按共享桌面配置字段读取 PS5 配对密钥；保留嵌入 NUL 的原始 16 字节注册密钥，分别处理 Qt INI 转义和 Windows 注册表 UTF-16 `@ByteArray` 序列化。
- 按 MAC 关联手动保存的主机地址。预览允许补填或修改局域网地址，确认后一次保存，取消不保存。
- 重复凭据、PS4 和不完整记录分别统计。忽略 PSN 令牌、账户密码和已保存的主机登录 PIN。
- HeliosImporter、AccountStore 序列化和原生连接/输入后端保持原实现。四个账户变更入口共用忙状态，防止异步导入快照覆盖另一笔保存。

## 已执行检查

- `dotnet build src/Ps5Bridge.App/Ps5Bridge.App.csproj -c Release --no-restore -p:TreatWarningsAsErrors=true`：0 警告、0 错误。
- `dotnet test tests/Ps5Bridge.Tests/Ps5Bridge.Tests.csproj -c Release --no-restore -p:TreatWarningsAsErrors=true`：62 项通过，其中新增 21 项 Chiaki 核心/注册表检查；原有 6 项 Helios 导入测试及其他账户存储、账户文件、DPAPI、会话、原生后端测试通过。
- `dotnet run --project tests/Ps5Bridge.UiChecks/UiChecks.csproj -c Release -p:TreatWarningsAsErrors=true`：34 项离线 WPF 检查通过，包括输入绑定、缺少地址/网址拦截、确认、取消、生命周期取消、共享账户变更忙状态及恢复、初始化保护，以及窗口宽度 760/620、导入预览 660/540 下的布局。
- Core、App、单元测试新增文件及 UiChecks 的 `dotnet format whitespace --verify-no-changes` 检查通过；`git diff --check` 无空白错误。
- 独立代码审阅已完成。审阅发现的并发导入覆盖风险已使用共享忙状态修复，并通过复核。

## 验证数据与边界

全部新增测试使用合成配对数据。注册表测试仅创建并清理随机命名的 `HKCU\Software\PS5InputBridge.Tests\...` 项；UI 检查绕过读取真实账户和 Chiaki 配置的 Loaded 处理。没有读取真实配对凭据、修改用户账户文件或连接真实 PS5。

格式依据为工程内固定 chiaki-ng 源码的 `gui/src/main.cpp`、`settings.cpp`、`host.cpp`，并核对历史 Chiaki v1.3.0 和 Qt 5.15 / 6.8 的 QSettings 实现。支持共享 Qt 桌面配置结构，不包含 Android / Switch 等客户端的其他导出格式。

截图生成在 `artifacts/chiaki-ui-validation/`，包括 `main-760.png`、`main-620.png`、`preview-660.png` 和 `preview-540.png`。截图只含合成账户。

## 本地发布

运行 `pwsh -NoProfile -File tools/publish.ps1 -SkipNativeBuild`，再次执行全部单元/原生集成测试，并更新自包含程序 `dist/PS5InputBridge/Ps5InputBridge.exe` 及对应源码包 `dist/PS5InputBridge-source.zip`。复用既有原生 DLL，本次不改动或重建原生输入后端。

真实用户的 Chiaki 配置导入与 PS5 连接仍需现场验证；自动化通过不等同于已经实机连接。
