using Ps5Bridge.App;
using Ps5Bridge.Core;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using Xunit;

namespace Ps5Bridge.Tests;

public sealed class NativeBackendTests
{
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
        // Four XInput slots always come first; any HID gamepads attached to the test machine follow with stable indices.
        Assert.Equal(new[] { 0, 1, 2, 3 }, controllers.Take(4).Select(controller => controller.Index));
        Assert.Equal(Enumerable.Range(4, controllers.Count - 4), controllers.Skip(4).Select(controller => controller.Index));
        Assert.All(controllers.Skip(4), controller => Assert.True(controller.Connected && controller.Name.StartsWith("HID 直读")));
        var snapshot = backend.GetSnapshot();
        Assert.Equal(BridgeState.Idle, snapshot.State);
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
        await backend.StopAsync(CancellationToken.None);
        await backend.StopAsync(CancellationToken.None);
    }
}
