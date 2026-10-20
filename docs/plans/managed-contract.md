# Managed application contract

Namespace: `Ps5Bridge.Core`. Projects target .NET 9; desktop project targets `net9.0-windows` with WPF. UI implementation owns `src/Ps5Bridge.App/` EXCEPT `NativeBridgeBackend.cs`, which the integrating agent owns.

Public core types (root implements):

```csharp
public sealed record PairingKeys(string RegistrationKeyHex, string MorningKeyHex, int Target = 0, string ConsoleMacHex = "");
public sealed record AccountProfile(Guid Id, string Name, string Host, string AccountId, PairingKeys Keys);
public sealed record ControllerInfo(int Index, string Name, bool Connected);
public sealed record BridgeOptions(int ControllerIndex = 0, int PollIntervalMicroseconds = 500);
public enum BridgeState { Idle, Connecting, Connected, Stopping, Failed }
public sealed record BackendSnapshot(BridgeState State, bool ControllerConnected, long InputReports,
    double MeanSubmitMicroseconds, double MaxSubmitMicroseconds, string? Message = null);
public sealed record SessionStatus(BridgeState State, string? AccountName = null, string? Message = null);
public interface IBridgeBackend : IAsyncDisposable
{
    IReadOnlyList<ControllerInfo> GetControllers();
    Task<PairingKeys> PairAsync(string host, string accountId, string pin, CancellationToken cancellationToken);
    Task ConnectAsync(AccountProfile profile, BridgeOptions options, CancellationToken cancellationToken);
    Task StopAsync(CancellationToken cancellationToken);
    BackendSnapshot GetSnapshot();
}
public interface ISecretProtector
{
    byte[] Protect(byte[] plainText);
    byte[] Unprotect(byte[] encrypted);
}
public sealed class AccountStore
{
    public AccountStore(string filePath, ISecretProtector protector);
    public Task<IReadOnlyList<AccountProfile>> LoadAsync(CancellationToken cancellationToken = default);
    public Task SaveAsync(IEnumerable<AccountProfile> profiles, CancellationToken cancellationToken = default);
    public static void Validate(AccountProfile profile);
}
public sealed class SessionCoordinator : IAsyncDisposable
{
    public SessionCoordinator(IBridgeBackend backend);
    public SessionStatus Status { get; }
    public event Action<SessionStatus>? StatusChanged;
    public Task ConnectAsync(AccountProfile profile, BridgeOptions options, CancellationToken cancellationToken = default);
    public Task StopAsync(CancellationToken cancellationToken = default);
    public BackendSnapshot GetSnapshot();
}
```

UI creates `WindowsSecretProtector` (root owns `src/Ps5Bridge.App/WindowsSecretProtector.cs`) using current-user Windows DPAPI, `AccountStore`, `NativeBridgeBackend` (root implements), and `SessionCoordinator`. Native backend constructor is parameterless. Store defaults under `%LOCALAPPDATA%\PS5InputBridge\accounts.dat`.

Use profiles in memory; persist only paired profiles. Add-account form takes display name, console IPv4/IP hostname, PSN Account-ID (base64 exactly 8 bytes) and console eight-digit pairing PIN. Do not request PSN passwords. Pairing must happen off UI thread. Saved keys are not displayed. Empty saved-account list is normal first run.

UI: choose profile → connect; choosing another profile while connected switches automatically via coordinator. Serial stop-before-start, no parallel sessions. Ctrl+1 through Ctrl+9 select/connect saved profile, Ctrl+D disconnect. Explicit status “连接中” / “已连接” / “连接失败”; clicking new account never implies logging out of PSN. Clarify one active account at a time.

BackendSnapshot metrics are PC-side capture-to-submit only, not PS5/TV end-to-end latency. Update no faster than 2 Hz. Default poll is 500 μs to prioritize latency; optional 1000 μs lowers CPU cost. XInput controllers initially supported (Xbox/PC mode). PS/Touchpad can use UI buttons if backend ABI supports them; root will report any addition.

Persist changes atomically; failed registration must not add a profile. Stop and dispose on window closing, no game session started automatically at startup. If native DLL is absent, app must start and display actionable backend error when used.
