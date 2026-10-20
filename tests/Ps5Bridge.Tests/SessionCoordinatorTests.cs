using Ps5Bridge.Core;
using Xunit;

namespace Ps5Bridge.Tests;

public sealed class SessionCoordinatorTests
{
    [Fact]
    public async Task AlreadyCancelledStopCannotCancelTheCurrentConnection()
    {
        var backend = new RecordingBackend { HoldConnect = true };
        await using var coordinator = new SessionCoordinator(backend);
        var connecting = coordinator.ConnectAsync(AccountStoreTests.Profile("current"), new());
        await backend.Started.Task;
        using var cancelled = new CancellationTokenSource();
        cancelled.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => coordinator.StopAsync(cancelled.Token));
        await Task.Delay(20);
        Assert.False(connecting.IsCompleted);
        await coordinator.StopAsync();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => connecting);
    }

    [Fact]
    public async Task UnexpectedBackendStopCannotLeaveTheUiConnected()
    {
        var backend = new RecordingBackend();
        await using var coordinator = new SessionCoordinator(backend);
        await coordinator.ConnectAsync(AccountStoreTests.Profile("current"), new());
        backend.SnapshotState = BridgeState.Idle;
        var snapshot = coordinator.GetSnapshot();
        Assert.Equal(BridgeState.Idle, snapshot.State);
        Assert.Equal(BridgeState.Failed, coordinator.Status.State);
        Assert.False(string.IsNullOrWhiteSpace(coordinator.Status.Message));
        await coordinator.StopAsync();
        Assert.False(backend.Active);
    }

    [Fact]
    public async Task AnOldSnapshotCannotOverwriteTheNewAccountsConnectionState()
    {
        var backend = new RecordingBackend();
        await using var coordinator = new SessionCoordinator(backend);
        await coordinator.ConnectAsync(AccountStoreTests.Profile("old"), new());
        backend.SnapshotState = BridgeState.Failed;
        backend.SnapshotReadAction = () => coordinator.ConnectAsync(AccountStoreTests.Profile("new"), new()).GetAwaiter().GetResult();
        coordinator.GetSnapshot();
        Assert.Equal("new", coordinator.Status.AccountName);
        Assert.Equal(BridgeState.Connected, coordinator.Status.State);
    }

    [Fact]
    public async Task AlreadyCancelledSelectionCannotCancelTheCurrentConnection()
    {
        var backend = new RecordingBackend { HoldConnect = true };
        await using var coordinator = new SessionCoordinator(backend);
        var connecting = coordinator.ConnectAsync(AccountStoreTests.Profile("current"), new());
        await backend.Started.Task;
        using var cancelled = new CancellationTokenSource();
        cancelled.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            coordinator.ConnectAsync(AccountStoreTests.Profile("obsolete"), new(), cancelled.Token));
        await Task.Delay(20);
        Assert.False(connecting.IsCompleted);
        await coordinator.StopAsync();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => connecting);
    }

    [Fact]
    public async Task StateObserverFailureCannotPreventStoppingOrSwitching()
    {
        var backend = new RecordingBackend();
        await using var coordinator = new SessionCoordinator(backend);
        coordinator.StatusChanged += status =>
        {
            if (status.State == BridgeState.Stopping) throw new InvalidOperationException("observer failure");
        };
        await coordinator.ConnectAsync(AccountStoreTests.Profile("A"), new());
        await coordinator.ConnectAsync(AccountStoreTests.Profile("B"), new());
        await coordinator.StopAsync();
        Assert.False(backend.Active);
        Assert.Contains("start:B", backend.Events);
        Assert.Equal(BridgeState.Idle, coordinator.Status.State);
    }

    [Fact]
    public async Task SwitchingStopsOldSessionBeforeStartingNewAccount()
    {
        var backend = new RecordingBackend();
        await using var coordinator = new SessionCoordinator(backend);
        await coordinator.ConnectAsync(AccountStoreTests.Profile("A"), new());
        await coordinator.ConnectAsync(AccountStoreTests.Profile("B"), new());
        Assert.Equal(new[] { "start:A", "stop", "start:B" }, backend.Events);
        Assert.Equal(1, backend.MaxConcurrentSessions);
        Assert.Equal("B", coordinator.Status.AccountName);
    }

    [Fact]
    public async Task ConnectionFailureDoesNotLeaveAnActiveSession()
    {
        var backend = new RecordingBackend { FailConnect = true };
        await using var coordinator = new SessionCoordinator(backend);
        await Assert.ThrowsAsync<IOException>(() => coordinator.ConnectAsync(AccountStoreTests.Profile(), new()));
        Assert.Equal(BridgeState.Failed, coordinator.Status.State);
        Assert.False(backend.Active);
        Assert.Contains("stop", backend.Events);
    }

    [Fact]
    public async Task StopFailurePreventsStartingAnotherAccount()
    {
        var backend = new RecordingBackend();
        await using var coordinator = new SessionCoordinator(backend);
        await coordinator.ConnectAsync(AccountStoreTests.Profile("A"), new());
        backend.FailStop = true;
        await Assert.ThrowsAsync<IOException>(() => coordinator.ConnectAsync(AccountStoreTests.Profile("B"), new()));
        Assert.DoesNotContain("start:B", backend.Events);
        backend.FailStop = false;
    }

    [Fact]
    public async Task StopCancelsAnInFlightConnection()
    {
        var backend = new RecordingBackend { HoldConnect = true };
        await using var coordinator = new SessionCoordinator(backend);
        var connect = coordinator.ConnectAsync(AccountStoreTests.Profile(), new());
        await backend.Started.Task.WaitAsync(TimeSpan.FromSeconds(2));
        var stop = coordinator.StopAsync();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => connect);
        await stop.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal(BridgeState.Idle, coordinator.Status.State);
        Assert.False(backend.Active);
    }

    [Fact]
    public async Task ConcurrentSelectionsCannotOpenParallelSessions()
    {
        var backend = new RecordingBackend();
        await using var coordinator = new SessionCoordinator(backend);
        await Task.WhenAll(Enumerable.Range(0, 6).Select(i => coordinator.ConnectAsync(AccountStoreTests.Profile(i.ToString()), new())));
        Assert.Equal(1, backend.MaxConcurrentSessions);
    }

    [Fact]
    public async Task DisconnectIsIdempotentAndDisposeStopsActiveSession()
    {
        var backend = new RecordingBackend();
        var coordinator = new SessionCoordinator(backend);
        await coordinator.StopAsync();
        await coordinator.ConnectAsync(AccountStoreTests.Profile(), new());
        await coordinator.DisposeAsync();
        Assert.False(backend.Active);
        Assert.True(backend.Disposed);
        await Assert.ThrowsAsync<ObjectDisposedException>(() => coordinator.ConnectAsync(AccountStoreTests.Profile(), new()));
    }

    private sealed class RecordingBackend : IBridgeBackend
    {
        public List<string> Events { get; } = [];
        public bool FailConnect { get; set; }
        public bool FailStop { get; set; }
        public bool HoldConnect { get; set; }
        public bool Active { get; private set; }
        public bool Disposed { get; private set; }
        public int MaxConcurrentSessions { get; private set; }
        public BridgeState? SnapshotState { get; set; }
        public Action? SnapshotReadAction { get; set; }
        public TaskCompletionSource Started { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public IReadOnlyList<ControllerInfo> GetControllers() => [new(0, "Test", true)];
        public Task<PairingKeys> PairAsync(string host, string accountId, string pin, CancellationToken token) => throw new NotSupportedException();
        public async Task ConnectAsync(AccountProfile profile, BridgeOptions options, CancellationToken token)
        {
            Events.Add("start:" + profile.Name);
            if (Active) MaxConcurrentSessions = 2;
            Active = true;
            MaxConcurrentSessions = Math.Max(MaxConcurrentSessions, 1);
            Started.TrySetResult();
            if (HoldConnect) await Task.Delay(Timeout.Infinite, token);
            if (FailConnect) throw new IOException("test failure");
        }
        public Task StopAsync(CancellationToken token)
        {
            Events.Add("stop");
            if (FailStop) throw new IOException("test stop failure");
            Active = false;
            return Task.CompletedTask;
        }
        public BackendSnapshot GetSnapshot()
        {
            var snapshot = new BackendSnapshot(SnapshotState ?? (Active ? BridgeState.Connected : BridgeState.Idle), true, 0, 0, 0);
            SnapshotReadAction?.Invoke();
            return snapshot;
        }
        public ValueTask DisposeAsync() { Disposed = true; return ValueTask.CompletedTask; }
    }
}
