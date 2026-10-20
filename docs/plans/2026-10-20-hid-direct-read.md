# HID 直读后端与延迟复查

日期：2026-10-20。目标：在 2026-10-19 的直发 / 折叠 / 补发改造之上，继续压低“手柄 → UDP”路径，并让 DS 模式手柄（光剑 Plus 等）不经 XInput 直接接入。

## 复查结论（改造前）

逐段测了发送路径的本机开销（Ryzen 7 7800X3D，Release 构建，20000 次）：

| 环节 | 平均 | 说明 |
| --- | --- | --- |
| history 包 CTR 加密（预生成密钥流） | 0.05–0.08 μs | gkcrypt 密钥流缓冲已经生效，直接 AES 也只有 0.08 μs |
| GMAC（mbedtls AES-GCM，每包建上下文） | 0.3–0.45 μs | 极个别 10–30 μs 尖峰是调度，不是算法 |
| UDP `send` 回环 36 字节 | 2.9 μs | 已是 Winsock 下限 |

加密与发送合计约 3–4 μs，XInput 路径剩余延迟几乎全在**等待采样**（0.5 ms 档平均 0.25 ms）和 **XInput 自身**（驱动把 USB 报告整形成 XInput 状态，不暴露报告到达事件）。feedback sender / takion 中没有多余线程切换；mbedtls 已启用 AES-NI。结论：软件发送侧无需再改，剩下能缩短的是**采集方式**。

## 改动

### A. HID 直读后端（`native/src/ps5bridge.c`）
- 手柄索引 4 起为 HID 手柄：`pb_hid_devices` 列出，`pb_controller_mask` 置位 4+n。识别依据：Sony VID，或 DS4 / DualSense 的报告长度（输入 64 / 输出 32 或 48 / 特征 64）。
- 输入线程对所选设备做重叠 `ReadFile` 阻塞等待，**报告到达即解析并提交**，没有采样间隔；8 个输入缓冲避免调度卡顿丢边沿。
- 解析 USB 报告 0x01 与蓝牙报告 0x11 / 0x31；打开时读一次校准特征报告，蓝牙手柄随之切到完整报告。摇杆 0x80 映射 0、满量程；扳机原值；两点触摸转发（DS4 行坐标按 942 → 1079 缩放）；IMU 不转发。
- 设备缺失或被独占时每 250 ms 重试并保持释放状态；切换槽位 / 停止 100 ms 内生效。枚举结果缓存 1 s（整机 HID 枚举约 10 ms）。
- 采样 / 提交 / 诊断逻辑抽成 `process_sample`，XInput 与 HID 共用，行为不变。
- ABI 升到 3。

### B. 托管端与界面
- 手柄列表在 XInput 1–4 之后列出“HID 直读 · <产品名>（DualShock 4 / DualSense 模式）”；首次加载优先选中实际在线的手柄。
- 会话参数允许索引 0–19。

## 本机验证
- 原生：`-Werror` 构建通过；`pb_self_test` 新增 DS4 / DualSense / 蓝牙报告解析用例；input worker 9/9；feedback sender 4/4；ABI 5/5（含 HID 列表与掩码一致性）。
- .NET 41/41。
- 实机（无 PS5，回环桩主机）：用插在开发机上的 DualShock 4（054C:09CC）走索引 4，报告率 1000 Hz，报告到达 → 提交平均 0.4 μs；XInput ↔ HID 切换、缺失设备重试、停止 15 ms 内完成。

## 待实机确认
- PS5 对 HID 直读的触摸板滑动、Create 键的响应（事件编码与上游一致）。
- 光剑 Plus DS 模式是否被识别（若其 VID 非 Sony 且报告长度不同，需要加规则）。
- 与 XInput 模式、Helios 的摄像对比。
