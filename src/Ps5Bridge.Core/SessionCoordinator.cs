namespace Ps5Bridge.Core;

public sealed class SessionCoordinator(IBridgeBackend backend) : IAsyncDisposable
{
    private readonly SemaphoreSlim gate = new(1, 1);
    private readonly object cancellationLock = new();
    private CancellationTokenSource? connecting;
    private SessionStatus status = new(BridgeState.Idle);
    private bool hasSession;
    private int disposed;
    private long requestVersion;
    private long optionsVersion;
    public SessionStatus Status => Volatile.Read(ref status);
    public event Action<SessionStatus>? StatusChanged;

    public async Task ConnectAsync(AccountProfile profile, BridgeOptions options, CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref disposed) != 0, this);
        cancellationToken.ThrowIfCancellationRequested();
        AccountStore.Validate(profile);
        ValidateOptions(options);
        var version = Interlocked.Increment(ref requestVersion);
        CancelConnecting();
        await gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            ObjectDisposedException.ThrowIf(Volatile.Read(ref disposed) != 0, this);
            if (version != Volatile.Read(ref requestVersion)) throw new OperationCanceledException("已选择另一个账户。");
            if (hasSession)
            {
                SetStatus(new(BridgeState.Stopping, Status.AccountName));
                try { await backend.StopAsync(CancellationToken.None).ConfigureAwait(false); hasSession = false; }
                catch (Exception exception) { SetStatus(new(BridgeState.Failed, Status.AccountName, exception.Message)); throw; }
            }
            using var cancellation = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            lock (cancellationLock) connecting = cancellation;
            // A newer request could arrive while the previous session was stopping.
            if (version != Volatile.Read(ref requestVersion)) cancellation.Cancel();
            try
            {
                cancellation.Token.ThrowIfCancellationRequested();
                SetStatus(new(BridgeState.Connecting, profile.Name));
                hasSession = true; // A failed start can still own partially initialized native resources.
                await backend.ConnectAsync(profile, options, cancellation.Token).ConfigureAwait(false);
                cancellation.Token.ThrowIfCancellationRequested();
                SetStatus(new(BridgeState.Connected, profile.Name));
            }
            catch (Exception exception)
            {
                try
                {
                    if (hasSession) await backend.StopAsync(CancellationToken.None).ConfigureAwait(false);
                    hasSession = false;
                }
                catch (Exception stopException)
                {
                    SetStatus(new(BridgeState.Failed, profile.Name, "无法停止旧会话，请重新启动程序。"));
                    throw new AggregateException(exception, stopException);
                }
                SetStatus(exception is OperationCanceledException
                    ? new(BridgeState.Idle, Message: "已取消连接")
                    : new(BridgeState.Failed, profile.Name, exception.Message));
                throw;
            }
            finally
            {
                lock (cancellationLock) if (ReferenceEquals(connecting, cancellation)) connecting = null;
            }
        }
        finally { gate.Release(); }
    }

    /// <summary>Updates the current session in place. Queued settings belong only to the
    /// connection that was current when requested; newer settings supersede older ones.</summary>
    public async Task UpdateOptionsAsync(BridgeOptions options, CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref disposed) != 0, this);
        cancellationToken.ThrowIfCancellationRequested();
        ValidateOptions(options);
        var sessionVersion = Volatile.Read(ref requestVersion);
        var updateVersion = Interlocked.Increment(ref optionsVersion);
        await gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            ObjectDisposedException.ThrowIf(Volatile.Read(ref disposed) != 0, this);
            if (sessionVersion != Volatile.Read(ref requestVersion) || updateVersion != Volatile.Read(ref optionsVersion)
                || !hasSession || Status.State != BridgeState.Connected) return;
            await backend.UpdateOptionsAsync(options, cancellationToken).ConfigureAwait(false);
        }
        finally { gate.Release(); }
    }

    private static void ValidateOptions(BridgeOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);
        if (options.ControllerIndex is < 0 or >= BridgeOptions.MaxControllers || options.PollIntervalMicroseconds is not (100 or 500 or 1000))
            throw new ArgumentException($"手柄序号需为 0–{BridgeOptions.MaxControllers - 1}，采集间隔需为 100、500 或 1000 微秒。");
    }

    public async Task StopAsync(CancellationToken cancellationToken = default)
    {
        if (Volatile.Read(ref disposed) != 0) return;
        cancellationToken.ThrowIfCancellationRequested();
        Interlocked.Increment(ref requestVersion);
        CancelConnecting();
        await gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (hasSession)
            {
                SetStatus(new(BridgeState.Stopping, Status.AccountName));
                try { await backend.StopAsync(CancellationToken.None).ConfigureAwait(false); hasSession = false; }
                catch (Exception exception) { SetStatus(new(BridgeState.Failed, Status.AccountName, exception.Message)); throw; }
            }
            SetStatus(new(BridgeState.Idle));
        }
        finally { gate.Release(); }
    }

    public BackendSnapshot GetSnapshot()
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref disposed) != 0, this);
        var observed = Status;
        var snapshot = backend.GetSnapshot();
        if (observed.State == BridgeState.Connected && snapshot.State is BridgeState.Failed or BridgeState.Idle)
        {
            var failure = new SessionStatus(BridgeState.Failed, observed.AccountName,
                string.IsNullOrWhiteSpace(snapshot.Message) ? "PS5 会话已断开，请重新连接。" : snapshot.Message);
            // Status reads can race an account switch; an old failure belongs only to its session.
            if (ReferenceEquals(Interlocked.CompareExchange(ref status, failure, observed), observed))
                NotifyStatus(failure);
        }
        return snapshot;
    }

    private void CancelConnecting()
    {
        lock (cancellationLock)
        {
            // CancelAsync keeps native cancellation callbacks off the caller's UI thread.
            if (connecting is not null) _ = connecting.CancelAsync();
        }
    }

    private void SetStatus(SessionStatus value)
    {
        Volatile.Write(ref status, value);
        NotifyStatus(value);
    }

    private void NotifyStatus(SessionStatus value)
    {
        if (StatusChanged is not { } observers) return;
        foreach (Action<SessionStatus> observer in observers.GetInvocationList())
        {
            try { observer(value); }
            catch (Exception exception)
            {
                // Observers are UI notifications, never part of the resource-control path.
                System.Diagnostics.Trace.TraceError("会话状态观察者异常：{0}", exception.GetType().Name);
            }
        }
    }

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref disposed, 1) != 0) return;
        Interlocked.Increment(ref requestVersion);
        CancelConnecting();
        await gate.WaitAsync().ConfigureAwait(false);
        try
        {
            try { if (hasSession) await backend.StopAsync(CancellationToken.None).ConfigureAwait(false); }
            finally { await backend.DisposeAsync().ConfigureAwait(false); hasSession = false; StatusChanged = null; }
        }
        finally { gate.Release(); }
    }
}
