namespace Ps5Bridge.Core;

public sealed record PairingKeys(string RegistrationKeyHex, string MorningKeyHex, int Target = 0, string ConsoleMacHex = "");
/// <param name="AccountId">Base64 PSN Account-ID; empty for imported LAN-only profiles (not needed to connect).</param>
public sealed record AccountProfile(Guid Id, string Name, string Host, string AccountId, PairingKeys Keys);
public sealed record ControllerInfo(int Index, string Name, bool Connected);
/// <param name="ControllerIndex">0–3 XInput slots; 4 and above are HID gamepads read directly (see IBridgeBackend.GetControllers).</param>
/// <param name="PollIntervalMicroseconds">XInput sampling interval; HID gamepads are event driven and ignore it.</param>
public sealed record BridgeOptions(int ControllerIndex = 0, int PollIntervalMicroseconds = 500)
{
    public const int MaxControllers = 20;
}
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

public sealed class AccountStoreException(string message, Exception innerException) : Exception(message, innerException);
