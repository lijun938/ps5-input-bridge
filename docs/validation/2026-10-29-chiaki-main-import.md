# 2026-10-29 chiaki-ng main 修复本地合入

引入 Downloads/ps5-input-bridge-next.zip 中的重建方案，固定上游 c8bea2642d0e95402f373af55584a3b98cd89417，应用逐事件发包、事件数和 20/50/100/200 ms 历史尾部补发。首发仍由输入线程同步完成，停止时等待 30 ms。应用版本 1.1.0，原生 ABI 6。

## 本地差异保留

保留原有 HID 预挂读取、同步完成快路径、输入锁优化、默认关闭详细统计、独立统计快照、输入诊断、界面和测试。GKCrypt 复用及小包缓冲优化改为精确补丁；反馈诊断迁移到上游发送器，默认关闭时无诊断时钟或分配。

删除整文件覆盖发送器/加密模块和旧的 chiaki-return-errors.json。chiaki-edits.json 包含 15 个不重叠补丁（协议、同步首发、错误返回、诊断、小包缓冲），chiaki-low-latency.json 包含 9 个加密补丁。真实固定源码连续应用两次结果一致；意外源码漂移会拒绝。

保留上游逐次通知行为，旧发送器减少通知的优化不再使用。旧版 8K 对照数值不代表本版本性能。摇杆没有旧版 2/8/24 ms 快速补发，丢包由约 200 ms 保活恢复。上游历史窗口包含新事件及最多 8 个旧事件，不再保留旧版完整 18 控件折叠快照。

bootstrap 按 URL 缓存并替换旧 pin 源码；提取后刷新源码时间戳，避免新归档的历史时间戳导致增量构建复用旧对象文件。回归测试先在旧实现上失败（新 pin 仍读取 old pin），修复后通过，并检查提取文件时间戳。

## Windows 验证

- tools/build-native.ps1 完整通过；11 个原生测试程序全部退出 0。
- 发送器 10/10（两种接收模型）、通知/丢包恢复 4/4、反馈诊断 6/6、输入诊断 5/5、HID 20/20、统计快照 11/11。
- 真实加密 UDP 发送错误与恢复、事件计数、小包内存及加密一致性测试通过。
- Python ABI/生命周期/bootstrap 13/13；bootstrap 单独复验 5/5。
- .NET Release 84/84；离线 WPF 87 项通过。
- tools/publish.ps1 -SkipNativeBuild 发布通过；dist/PS5InputBridge-win-x64.zip 含应用和对应源码包。

日志：artifacts/import-native-build.log、artifacts/import-managed-tests.log、artifacts/import-publish.log。

未连接真实 PS5、USB 手柄或使用真实账户。仍需按原方案在 Xbox 与 DS 模式各复测 L2 松开同时 R2 变化、单次换弹、快速按键和摇杆回中；本地接收模型不能证明 PS5 固件行为。
