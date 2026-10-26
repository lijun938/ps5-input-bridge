using Ps5Bridge.App;
using Ps5Bridge.Core;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using Xunit;

namespace Ps5Bridge.Tests;

public sealed class NativeBackendTests
{
    [Theory]
    [InlineData(2u, true, "同一设备")]
    [InlineData(2u, false, "Xbox / PC")]
    [InlineData(3u, true, "独占手柄")]
    [InlineData(4u, true, "USB / 蓝牙")]
    [InlineData(4u, false, "XInput")]
    [InlineData(5u, false, "一直旋转")]
    [InlineData(5u, true, "一直旋转")]
    public void ControllerFailuresGiveRecoveryStepsSeparateFromTheSession(uint state, bool hid, string expected)
    {
        Assert.Contains(expected, NativeBridgeBackend.DescribeController(state, 5, hid));
        Assert.Null(NativeBridgeBackend.DescribeController(1, 0, hid));
    }

    [Fact]
    public async Task HidSelectionRequiresStableIdentityBeforeAnyNetworkConnection()
    {
        await using var backend = new NativeBridgeBackend();
        var error = await Assert.ThrowsAsync<IOException>(() => backend.ConnectAsync(AccountStoreTests.Profile(), new(4), CancellationToken.None));
        Assert.Contains("设备标识", error.Message);
        await Assert.ThrowsAsync<IOException>(() => backend.UpdateOptionsAsync(new(4), CancellationToken.None));
        Assert.Equal(BridgeState.Idle, backend.GetSnapshot().State);
    }

    [Fact]
    public async Task IncorrectLoginPinCanBeRetriedEvenIfTheConnectingTransitionWasMissed()
    {
        await using var backend = new NativeBridgeBackend();
        var submissions = new List<string>();
        backend.LoginPinProvider = _ => Task.FromResult<string?>(submissions.Count == 0 ? "1111" : "2222");
        var state = new NativeBridgeBackend.ConnectionState(4, "Console login PIN required.", 1);
        await backend.WaitForConnectionAsync(() => state, pin =>
        {
            submissions.Add(pin);
            // The native callback can reject a PIN between two managed status reads.
            state = submissions.Count == 1
                ? new(4, "Console login PIN incorrect.", 2)
                : new(3, "Connected", 2);
        }, CancellationToken.None).WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal(new[] { "1111", "2222" }, submissions);
    }

    [Fact]
    public async Task LoginRequestIsPromptedOnlyOnceUntilTheConsoleRequestsAnotherPin()
    {
        await using var backend = new NativeBridgeBackend();
        var prompts = 0;
        var statusReadsAfterSubmission = 0;
        backend.LoginPinProvider = _ => { prompts++; return Task.FromResult<string?>("1234"); };
        var submitted = false;
        await backend.WaitForConnectionAsync(() => submitted && ++statusReadsAfterSubmission >= 3
            ? new(3, "Connected", 1)
            : new(4, "Console login PIN required.", 1), _ => submitted = true, CancellationToken.None)
            .WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal(1, prompts);
    }

    [Fact]
    public async Task AStoppedConnectionFailsImmediatelyInsteadOfWaitingForTheTimeout()
    {
        await using var backend = new NativeBridgeBackend();
        await Assert.ThrowsAsync<IOException>(() => backend.WaitForConnectionAsync(
            () => new(5, "Console stopped the session.", 0), _ => { }, CancellationToken.None)
            .WaitAsync(TimeSpan.FromSeconds(2)));
    }

    [Fact]
    public async Task CancellingRealNativeConnectionDoesNotJoinThreadsOnTheCaller()
    {
        await using var backend = new NativeBridgeBackend();
        using var cancellation = new CancellationTokenSource();
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        using var listener = new TcpListener(IPAddress.Loopback, 9295);
        listener.Start();
        var accepting = listener.AcceptTcpClientAsync(deadline.Token).AsTask();
        var profile = AccountStoreTests.Profile() with { Host = "127.0.0.1", Keys = new(new string('a', 32), new string('b', 32), 1000100) };
        var connecting = backend.ConnectAsync(profile, new(), cancellation.Token);
        using var peer = await accepting;
        var handshake = backend.GetSnapshot();
        Assert.Equal(BridgeState.Connecting, handshake.State);
        Assert.Equal(0, handshake.InputReports);
        Assert.True(double.IsNaN(handshake.MeanSubmitMicroseconds));
        var latency = Assert.IsType<LatencySnapshot>(handshake.Latency);
        Assert.Equal(new LatencySnapshot(0, 0, 0, 0, 0, 0, 0, 0, 0), latency);
        var watch = Stopwatch.StartNew();
        cancellation.Cancel();
        Assert.True(watch.Elapsed < TimeSpan.FromMilliseconds(100), "Cancel must not synchronously join native threads on the caller.");
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => connecting.WaitAsync(deadline.Token));
        await backend.StopAsync(deadline.Token);
        Assert.Equal(BridgeState.Idle, backend.GetSnapshot().State);
    }

    [Fact]
    public async Task RealNativeAbiLoadsAndPassesItsOfflineSelfTest()
    {
        await using var backend = new NativeBridgeBackend();
        Assert.Equal(0, backend.RunNativeSelfTest());
        var controllers = backend.GetControllers();
        // XInput slots remain numbered; HID paths identify devices independently of enumeration order.
        Assert.Equal(new[] { 0, 1, 2, 3 }, controllers.Take(4).Select(controller => controller.Index));
        Assert.Equal(Enumerable.Range(4, controllers.Count - 4), controllers.Skip(4).Select(controller => controller.Index));
        Assert.All(controllers.Skip(4), controller => Assert.True(controller.Connected && controller.Name.StartsWith("HID 直读")));
        Assert.All(controllers.Skip(4), controller => Assert.False(string.IsNullOrWhiteSpace(controller.DeviceId)));
        var snapshot = backend.GetSnapshot();
        Assert.Equal(BridgeState.Idle, snapshot.State);
        Assert.Null(snapshot.InputHealth);
        Assert.True(double.IsNaN(snapshot.MeanSubmitMicroseconds));
    }

    [Theory]
    [InlineData("incorrect", "12345678")]
    [InlineData("AQIDBAUGBwg=", "123")]
    [InlineData("AQIDBAUGBwg=", "abcd1234")]
    public async Task PairingRejectsInvalidIdentityOrPinBeforeNetworking(string accountId, string pin)
    {
        await using var backend = new NativeBridgeBackend();
        await Assert.ThrowsAsync<ArgumentException>(() => backend.PairAsync("127.0.0.1", accountId, pin, CancellationToken.None));
    }

    [Fact]
    public async Task RealNativeBackendRejectsInvalidControllerAndStillStopsCleanly()
    {
        await using var backend = new NativeBridgeBackend();
        await Assert.ThrowsAsync<IOException>(() => backend.ConnectAsync(AccountStoreTests.Profile() with { Host = "127.0.0.1" },
            new BridgeOptions(42, 1000), CancellationToken.None));
        // A context exists, but invalid selection prevented the input worker from starting.
        // This also exercises the native ABI's sentinel ages before any XInput read.
        Assert.Equal(new InputHealthSnapshot(0, 0, 0, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
            backend.GetSnapshot().InputHealth);
        await backend.StopAsync(CancellationToken.None);
        await backend.StopAsync(CancellationToken.None);
    }
}
