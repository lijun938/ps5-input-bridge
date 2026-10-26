using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;
using Ps5Bridge.Core;

namespace Ps5Bridge.App;

/// <summary>UI-side lifecycle only. All controller sampling and feedback submission remain native.</summary>
public sealed class NativeBridgeBackend : IBridgeBackend
{
    private readonly SemaphoreSlim lifecycle = new(1, 1);
    private readonly CancellationTokenSource shutdown = new();
    private readonly object pairingLock = new();
    private readonly HashSet<Task> pairings = [];
    private BridgeHandle? context;
    private int disposed;
    private bool libraryChecked;
    public Func<CancellationToken, Task<string?>>? LoginPinProvider { get; set; }

    public const int XInputSlots = 4;
    private const int HidMax = 16;

    /// <summary>XInput slots 0–3 first, then every HID gamepad with a DualShock 4 / DualSense report layout
    /// (indices 4+). HID pads are read directly per USB report, so no sampling interval applies.</summary>
    public IReadOnlyList<ControllerInfo> GetControllers()
    {
        CheckDisposed();
        EnsureLibrary();
        var mask = Native.pb_controller_mask();
        var list = Enumerable.Range(0, XInputSlots)
            .Select(i => new ControllerInfo(i, $"XInput {i + 1}", (mask & (1u << i)) != 0)).ToList();
        var devices = new NativeHidInfo[HidMax];
        var count = Math.Min((int)Native.pb_hid_devices(devices, HidMax), HidMax);
        for (var i = 0; i < count; i++)
        {
            var name = Decode(devices[i].Name);
            var layout = devices[i].Layout == 2 ? "DualSense" : "DualShock 4";
            list.Add(new ControllerInfo(XInputSlots + i,
                $"HID 直读 · {(name.Length > 0 ? name : $"{devices[i].VendorId:X4}:{devices[i].ProductId:X4}")}（{layout} 模式）", true)
                { DeviceId = Decode(devices[i].DeviceId) });
        }
        return list;
    }

    public Task<PairingKeys> PairAsync(string host, string accountId, string pin, CancellationToken cancellationToken)
    {
        CheckDisposed();
        AccountStore.ValidateHost(host);
        AccountStore.ValidateAccountId(accountId);
        if (pin.Length != 8 || !pin.All(char.IsAsciiDigit) || !uint.TryParse(pin, out var numericPin))
            throw new ArgumentException("请输入 PS5 配对设备页面显示的 8 位配对码。");
        EnsureLibrary();
        lock (pairingLock)
        {
            CheckDisposed();
            var pairing = PairCoreAsync(host, accountId, numericPin, cancellationToken);
            pairings.Add(pairing);
            _ = pairing.ContinueWith(_ =>
            {
                lock (pairingLock) pairings.Remove(pairing);
            }, CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
            return pairing;
        }
    }

    private async Task<PairingKeys> PairCoreAsync(string host, string accountId, uint numericPin,
        CancellationToken cancellationToken)
    {
        using var linkedCancellation = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, shutdown.Token);
        cancellationToken = linkedCancellation.Token;
        using var pairing = CreateContext(); // Pairing a second account must not stop the active input session.
        cancellationToken.ThrowIfCancellationRequested();
        NativeCredentials result = default;
        var registration = Task.Run(() =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            return Native.pb_register(pairing, host, Convert.FromBase64String(accountId), numericPin, 30000, out result);
        }, CancellationToken.None);
        while (!registration.IsCompleted)
        {
            // Retry cancellation if it arrived just before native registration started.
            // The pairing handle stays alive until both registration and stop have completed.
            if (cancellationToken.IsCancellationRequested && Native.pb_get_status(pairing, out var status) == 0 && status.State == 1)
                await Task.Run(() => Native.pb_stop(pairing), CancellationToken.None).ConfigureAwait(false);
            await Task.WhenAny(registration, Task.Delay(25)).ConfigureAwait(false);
        }
        var code = await registration.ConfigureAwait(false);
        cancellationToken.ThrowIfCancellationRequested();
        ThrowIfFailed(code, pairing, "配对失败");
        return new PairingKeys(EncodeBytes(result.RegistrationKey, 16), EncodeBytes(result.MorningKey, 16),
            result.Target, EncodeBytes(result.Mac, 6));
    }

    public async Task ConnectAsync(AccountProfile profile, BridgeOptions options, CancellationToken cancellationToken)
    {
        AccountStore.Validate(profile);
        RequireHidIdentity(options);
        await lifecycle.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            CheckDisposed();
            EnsureLibrary();
            context ??= CreateContext();
            var handle = context;
            var credentials = new NativeCredentials
            {
                AccountId = profile.AccountId == "" ? new byte[8] : Convert.FromBase64String(profile.AccountId),
                RegistrationKey = Convert.FromHexString(profile.Keys.RegistrationKeyHex),
                MorningKey = Convert.FromHexString(profile.Keys.MorningKeyHex),
                Mac = profile.Keys.ConsoleMacHex == "" ? new byte[6] : Convert.FromHexString(profile.Keys.ConsoleMacHex),
                Reserved = new byte[2],
                Target = profile.Keys.Target,
                Nickname = new byte[32]
            };
            ThrowIfFailed(Native.pb_set_poll_interval(handle, checked((uint)options.PollIntervalMicroseconds)), handle, "采集间隔设置失败");
            ThrowIfFailed(Native.pb_set_back_mapping(handle, BackMapping), handle, "Back 键映射设置失败");
            cancellationToken.ThrowIfCancellationRequested();
            var code = await Task.Run(() => Native.pb_start_device(handle, profile.Host, ref credentials,
                checked((uint)options.ControllerIndex), options.DeviceId), CancellationToken.None).ConfigureAwait(false);
            ThrowIfFailed(code, handle, "无法开始连接");
            await WaitForConnectionAsync(() =>
            {
                ThrowIfFailed(Native.pb_get_status(handle, out var status), handle, "状态读取失败");
                return new ConnectionState(status.State, Decode(status.Message),
                    status.State == 4 ? Native.pb_login_request_serial(handle) : 0);
            }, loginPin => ThrowIfFailed(Native.pb_set_login_pin(handle, loginPin), handle, "主机登录密码提交失败"),
                cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            // Do not register a synchronous native stop callback on a token that UI code cancels.
            // Await cleanup off the UI thread while this lifecycle lock still protects the handle.
            if (context is not null)
                await Task.Run(() => Native.pb_stop(context), CancellationToken.None).ConfigureAwait(false);
            throw;
        }
        finally { lifecycle.Release(); }
    }

    public async Task UpdateOptionsAsync(BridgeOptions options, CancellationToken cancellationToken)
    {
        if (options.ControllerIndex is < 0 or >= BridgeOptions.MaxControllers || options.PollIntervalMicroseconds is not (100 or 500 or 1000))
            throw new ArgumentException("请选择有效手柄和 100、500 或 1000 微秒采集间隔。");
        RequireHidIdentity(options);
        await lifecycle.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            CheckDisposed();
            if (context is not { IsClosed: false } handle) return;
            ThrowIfFailed(Native.pb_get_status(handle, out var status), handle, "状态读取失败");
            if (status.State != 3) return;
            cancellationToken.ThrowIfCancellationRequested();
            ThrowIfFailed(Native.pb_set_controller_device(handle, checked((uint)options.ControllerIndex), options.DeviceId),
                handle, "手柄设置失败");
            ThrowIfFailed(Native.pb_set_poll_interval(handle, checked((uint)options.PollIntervalMicroseconds)),
                handle, "采集间隔设置失败");
        }
        finally { lifecycle.Release(); }
    }

    private static void RequireHidIdentity(BridgeOptions options)
    {
        if (options.ControllerIndex is >= XInputSlots and < BridgeOptions.MaxControllers && string.IsNullOrWhiteSpace(options.DeviceId))
            throw new IOException("所选 HID 手柄缺少设备标识，请刷新手柄列表并重新选择。");
        if (options.DeviceId is not null && Encoding.UTF8.GetByteCount(options.DeviceId) >= 1536)
            throw new ArgumentException("手柄设备标识过长，请刷新列表并重新选择。");
    }

    internal readonly record struct ConnectionState(uint State, string Message, ulong LoginRequestSerial);

    internal async Task WaitForConnectionAsync(Func<ConnectionState> readStatus, Action<string> submitPin,
        CancellationToken cancellationToken)
    {
        var deadline = Stopwatch.StartNew();
        ulong? lastLoginRequest = null;
        while (deadline.Elapsed < TimeSpan.FromSeconds(20))
        {
            cancellationToken.ThrowIfCancellationRequested();
            var status = readStatus();
            if (status.State == 3) return;
            if (status.State is 0 or 5 or 6) throw new IOException("PS5 连接失败：" + status.Message);
            if (status.State == 4 && status.LoginRequestSerial != lastLoginRequest)
            {
                // A rejected PIN can transition connecting -> PIN request between two polls.
                // The native event serial distinguishes that request from a stale status read.
                lastLoginRequest = status.LoginRequestSerial;
                if (LoginPinProvider is null) throw new IOException("此 PS5 账户需要 4 位主机登录密码，请解除主机登录密码后重试。");
                var loginPin = await LoginPinProvider(cancellationToken).ConfigureAwait(false);
                cancellationToken.ThrowIfCancellationRequested();
                if (loginPin is null) throw new OperationCanceledException("已取消主机登录。", cancellationToken);
                if (loginPin.Length != 4 || !loginPin.All(char.IsAsciiDigit)) throw new ArgumentException("主机登录密码需为 4 位数字。");
                submitPin(loginPin);
                deadline.Restart();
            }
            await Task.Delay(25, cancellationToken).ConfigureAwait(false);
        }
        throw new TimeoutException("连接 PS5 超时。请检查主机已开机、Remote Play 已开启，且 PC 和主机位于同一局域网。");
    }

    public async Task StopAsync(CancellationToken cancellationToken)
    {
        await lifecycle.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (context is { IsClosed: false })
                ThrowIfFailed(await Task.Run(() => Native.pb_stop(context), CancellationToken.None).ConfigureAwait(false), context, "停止会话失败");
        }
        finally { lifecycle.Release(); }
    }

    public BackendSnapshot GetSnapshot()
    {
        CheckDisposed();
        var handle = context;
        if (handle is null || handle.IsClosed) return new(BridgeState.Idle, false, 0, double.NaN, double.NaN);
        ThrowIfFailed(Native.pb_get_status(handle, out var status), handle, "状态读取失败");
        ThrowIfFailed(Native.pb_get_timing_stats(handle, out var timing), handle, "统计读取失败");
        ThrowIfFailed(Native.pb_get_latency_stats(handle, out var latency), handle, "延迟分布读取失败");
        ThrowIfFailed(Native.pb_get_controller_status(handle, out var controller), handle, "手柄状态读取失败");
        InputHealthSnapshot? inputHealth = null;
        if (status.ControllerIndex < XInputSlots)
        {
            ThrowIfFailed(Native.pb_get_input_health(handle, out var health), handle, "输入健康状态读取失败");
            inputHealth = new(checked((long)health.ReadCount), checked((long)health.PacketChanges), checked((long)health.ReadFailures),
                health.LastReadAgeMilliseconds, health.LastChangeAgeMilliseconds, health.LastReadDurationMicroseconds,
                health.PacketNumber, health.ReadError, health.LeftX, health.LeftY, health.RightX, health.RightY,
                health.Buttons, health.L2, health.R2) { ReadStalls = health.ReadStalls };
        }
        var state = status.State switch
        {
            1 or 2 or 4 => BridgeState.Connecting,
            3 => BridgeState.Connected,
            6 => BridgeState.Failed,
            _ => BridgeState.Idle
        };
        return new(state, status.ControllerPresent != 0, checked((long)status.InputReports),
            timing.Count == 0 ? double.NaN : timing.MeanMicroseconds,
            timing.Count == 0 ? double.NaN : timing.MaxMicroseconds, Decode(status.Message))
        {
            ControllerMessage = state is BridgeState.Connecting or BridgeState.Connected
                ? DescribeController(controller.State, controller.SystemError, status.ControllerIndex >= XInputSlots) : null,
            ControllerState = controller.State,
            ControllerSystemError = controller.SystemError,
            InputHealth = inputHealth,
            Latency = new(checked((long)latency.SubmitCount), checked((long)latency.IntervalCount),
                latency.SubmitP95, latency.SubmitP99, latency.IntervalMean, latency.IntervalP95,
                latency.IntervalP99, latency.IntervalMax, checked((long)latency.SubmitFailures))
        };
    }

    internal static string? DescribeController(uint state, uint systemError, bool hid) => state switch
    {
        0 => "正在等待所选手柄的输入。",
        1 => null,
        2 when hid => "所选 HID 手柄已离线，请重新插入同一设备，或在手柄列表中选择其他设备。",
        2 => "所选 XInput 手柄未连接，请连接手柄并切换到 Xbox / PC 模式，或重新选择手柄。",
        3 => "无法访问所选 HID 手柄，请关闭独占手柄的其他程序（如 Steam、DS4Windows 或 Chiaki），然后重试。",
        4 when !hid && systemError == 1460 => "XInput 手柄读取超时，已请求释放输入，正在等待驱动恢复。（系统错误 1460）",
        4 => $"读取所选 {(hid ? "HID" : "XInput")} 手柄失败，请检查 USB / 蓝牙连接并重新插入手柄。（系统错误 {systemError}）",
        5 => "手柄持续上报两个摇杆同向顶到底部，已暂停转发以免画面一直旋转；松开摇杆或重新插拔手柄后会自动恢复。",
        _ => "手柄状态未知，请刷新列表并重新选择。"
    };

    private uint backMapping;
    /// <summary>0 = touchpad click (default), 1 = share. Applied live to an active session.</summary>
    public uint BackMapping
    {
        get => Volatile.Read(ref backMapping);
        set
        {
            if (value > 1) throw new ArgumentOutOfRangeException(nameof(value));
            Volatile.Write(ref backMapping, value);
            if (context is { IsClosed: false } handle) ThrowIfFailed(Native.pb_set_back_mapping(handle, value), handle, "Back 键映射设置失败");
        }
    }

    public InputDebugSnapshot GetInputDebug()
    {
        CheckDisposed();
        var handle = context;
        if (handle is null || handle.IsClosed) return new(0, 0, 0, false, 0, 0, 0, 0, 0);
        ThrowIfFailed(Native.pb_get_input_debug(handle, out var status), handle, "输入诊断读取失败");
        return new(status.Buttons, status.L2, status.R2, status.ControllerPresent != 0,
            checked((long)status.ObservedChanges), checked((long)status.SubmitFailures),
            checked((long)status.L1DownCount), checked((long)status.R1DownCount), checked((long)status.L1R1DownCount));
    }

    public async Task SendSpecialButtonAsync(uint flags, CancellationToken cancellationToken = default)
    {
        await lifecycle.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            CheckDisposed();
            if (context is null) throw new InvalidOperationException("请先连接 PS5。");
            ThrowIfFailed(Native.pb_button(context, flags, 1), context, "按键发送失败");
            try { await Task.Delay(50, cancellationToken).ConfigureAwait(false); }
            finally { ThrowIfFailed(Native.pb_button(context, flags, 0), context, "按键释放失败"); }
        }
        finally { lifecycle.Release(); }
    }

    public int RunNativeSelfTest()
    {
        EnsureLibrary();
        return Native.pb_self_test();
    }

    private void EnsureLibrary()
    {
        if (libraryChecked) return;
        try
        {
            if (Marshal.SizeOf<NativeCredentials>() != 84 || Marshal.SizeOf<NativeStatus>() != 288 ||
                Marshal.SizeOf<NativeTiming>() != 24 || Marshal.SizeOf<NativeInputDebug>() != 56 ||
                Marshal.SizeOf<NativeHidInfo>() != 1608 || Marshal.SizeOf<NativeLatency>() != 72 || Marshal.SizeOf<NativeControllerStatus>() != 8 ||
                Marshal.SizeOf<NativeInputHealth>() != 80)
                throw new InvalidOperationException("原生接口结构布局不匹配。");
            if (Native.pb_abi_version() != 6) throw new InvalidOperationException("原生桥接库版本不匹配，请重新解压完整发行包。");
            libraryChecked = true;
        }
        catch (Exception exception) when (exception is DllNotFoundException or BadImageFormatException or EntryPointNotFoundException)
        {
            throw new IOException("无法加载原生桥接库。请将 ps5bridge.dll 和程序放在一起，并使用完整的 Windows x64 发行包。", exception);
        }
    }

    private static BridgeHandle CreateContext()
    {
        var pointer = Native.pb_create();
        if (pointer == IntPtr.Zero) throw new IOException("无法初始化原生桥接引擎。");
        return new BridgeHandle(pointer);
    }

    private static string Decode(byte[]? bytes)
    {
        if (bytes is null) return "";
        var length = Array.IndexOf(bytes, (byte)0);
        return Encoding.UTF8.GetString(bytes, 0, length < 0 ? bytes.Length : length);
    }

    private static string EncodeBytes(byte[]? bytes, int length)
    {
        if (bytes?.Length != length) throw new IOException("原生配对结果无效，请使用匹配的完整发行包。");
        return Convert.ToHexString(bytes);
    }

    private static void ThrowIfFailed(int code, BridgeHandle handle, string operation)
    {
        if (code == 0) return;
        var detail = Native.pb_get_status(handle, out var status) == 0 ? Decode(status.Message) : "";
        if (code == -4) throw new OperationCanceledException(operation + "：已取消。");
        if (code == -3) throw new TimeoutException(operation + "：超时。" + detail);
        if (code == -6) throw new IOException("没有检测到所选 XInput 手柄。请连接手柄并切换到 Xbox / PC 模式。");
        throw new IOException(operation + "：" + (detail.Length > 0 ? detail : $"错误 {code}"));
    }

    private void CheckDisposed() => ObjectDisposedException.ThrowIf(Volatile.Read(ref disposed) != 0, this);

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref disposed, 1) != 0) return;
        shutdown.Cancel();
        Task[] pairingTasks;
        lock (pairingLock) pairingTasks = pairings.ToArray();
        try { await Task.WhenAll(pairingTasks).ConfigureAwait(false); }
        catch (OperationCanceledException) { }
        catch (Exception exception) { Trace.TraceError("Pairing shutdown: {0}", exception.Message); }
        await lifecycle.WaitAsync().ConfigureAwait(false);
        try
        {
            if (context is not null)
            {
                await Task.Run(() => { Native.pb_stop(context); context.Dispose(); }, CancellationToken.None).ConfigureAwait(false);
                context = null;
            }
        }
        finally
        {
            lifecycle.Release();
            shutdown.Dispose();
        }
    }

    private sealed class BridgeHandle : SafeHandleZeroOrMinusOneIsInvalid
    {
        internal BridgeHandle(IntPtr pointer) : base(true) => SetHandle(pointer);
        protected override bool ReleaseHandle() { Native.pb_destroy(handle); return true; }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeCredentials
    {
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)] public byte[] AccountId;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)] public byte[] RegistrationKey;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)] public byte[] MorningKey;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 6)] public byte[] Mac;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 2)] public byte[] Reserved;
        public int Target;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)] public byte[] Nickname;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct NativeStatus
    {
        public uint State, ControllerIndex, ControllerPresent;
        public int ErrorCode;
        public ulong InputReports, InputSamples;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256)] public byte[] Message;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct NativeTiming { public ulong Count; public double MeanMicroseconds, MaxMicroseconds; }
    [StructLayout(LayoutKind.Sequential)]
    private struct NativeLatency
    {
        public ulong SubmitCount, IntervalCount;
        public double SubmitP95, SubmitP99, IntervalMean, IntervalP95, IntervalP99, IntervalMax;
        public ulong SubmitFailures;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct NativeHidInfo
    {
        public ushort VendorId, ProductId;
        public uint Layout;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)] public byte[] Name;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 1536)] public byte[] DeviceId;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct NativeControllerStatus { public uint State, SystemError; }
    [StructLayout(LayoutKind.Sequential)]
    private struct NativeInputDebug
    {
        public uint Buttons, L2, R2, ControllerPresent;
        public ulong ObservedChanges, SubmitFailures, L1DownCount, R1DownCount, L1R1DownCount;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct NativeInputHealth
    {
        public ulong ReadCount, PacketChanges, ReadFailures;
        public double LastReadAgeMilliseconds, LastChangeAgeMilliseconds, LastReadDurationMicroseconds;
        public uint PacketNumber, ReadError;
        public short LeftX, LeftY, RightX, RightY;
        public uint Buttons, L2, R2, ReadStalls;
    }

    private static class Native
    {
        private const string Library = "ps5bridge.dll";
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)] internal static extern uint pb_abi_version();
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)] internal static extern IntPtr pb_create();
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)] internal static extern void pb_destroy(IntPtr context);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)] internal static extern uint pb_controller_mask();
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)] internal static extern uint pb_hid_devices([Out] NativeHidInfo[] devices, uint capacity);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)] internal static extern int pb_self_test();
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int pb_register(BridgeHandle context,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string host, byte[] accountId, uint pin, uint timeout, out NativeCredentials result);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int pb_start_device(BridgeHandle context,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string host, ref NativeCredentials credentials, uint controller,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? deviceId);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int pb_set_controller_device(BridgeHandle context, uint controller,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? deviceId);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)] internal static extern int pb_get_controller_status(BridgeHandle context, out NativeControllerStatus status);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)] internal static extern int pb_stop(BridgeHandle context);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)] internal static extern int pb_get_status(BridgeHandle context, out NativeStatus status);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)] internal static extern int pb_get_timing_stats(BridgeHandle context, out NativeTiming timing);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)] internal static extern int pb_get_latency_stats(BridgeHandle context, out NativeLatency latency);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)] internal static extern int pb_set_poll_interval(BridgeHandle context, uint microseconds);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)] internal static extern int pb_button(BridgeHandle context, uint flags, uint pressed);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)] internal static extern int pb_set_back_mapping(BridgeHandle context, uint mapping);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)] internal static extern int pb_set_login_pin(BridgeHandle context, [MarshalAs(UnmanagedType.LPUTF8Str)] string digits);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)] internal static extern ulong pb_login_request_serial(BridgeHandle context);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)] internal static extern int pb_get_input_debug(BridgeHandle context, out NativeInputDebug status);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)] internal static extern int pb_get_input_health(BridgeHandle context, out NativeInputHealth health);
    }
}

public sealed record InputDebugSnapshot(uint Buttons, uint L2, uint R2, bool Present,
    long ObservedChanges, long SubmitFailures, long L1Presses, long R1Presses, long Chords);
