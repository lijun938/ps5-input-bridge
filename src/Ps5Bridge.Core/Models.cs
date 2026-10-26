namespace Ps5Bridge.Core;

public sealed record PairingKeys(string RegistrationKeyHex, string MorningKeyHex, int Target = 0, string ConsoleMacHex = "");
/// <param name="AccountId">Base64 PSN Account-ID; empty for imported LAN-only profiles (not needed to connect).</param>
public sealed record AccountProfile(Guid Id, string Name, string Host, string AccountId, PairingKeys Keys);
public sealed record ControllerInfo(int Index, string Name, bool Connected)
{
    /// <summary>Stable HID device path; null for XInput slots.</summary>
    public string? DeviceId { get; init; }
}
/// <param name="ControllerIndex">0–3 XInput slots; 4 and above are HID gamepads read directly (see IBridgeBackend.GetControllers).</param>
/// <param name="PollIntervalMicroseconds">XInput sampling interval; HID gamepads are event driven and ignore it.</param>
public sealed record BridgeOptions(int ControllerIndex = 0, int PollIntervalMicroseconds = 500)
{
    public const int MaxControllers = 20;
    public string? DeviceId { get; init; }
}
public enum BridgeState { Idle, Connecting, Connected, Stopping, Failed }
public sealed record BackendSnapshot(BridgeState State, bool ControllerConnected, long InputReports,
    double MeanSubmitMicroseconds, double MaxSubmitMicroseconds, string? Message = null)
{
    public LatencySnapshot? Latency { get; init; }
    /// <summary>XInput read health and raw input values; null for HID selections or an absent native context.</summary>
    public InputHealthSnapshot? InputHealth { get; init; }
    public string? ControllerMessage { get; init; }
    public uint ControllerState { get; init; }
    public uint ControllerSystemError { get; init; }
}
/// <summary>Recent native sample windows; local processing, not PS5/TV latency.</summary>
public sealed record LatencySnapshot(long SubmitSamples, long IntervalSamples,
    double SubmitP95Microseconds, double SubmitP99Microseconds,
    double IntervalMeanMicroseconds, double IntervalP95Microseconds,
    double IntervalP99Microseconds, double IntervalMaxMicroseconds, long SubmitFailures);
/// <summary>Read ages are -1 before the first completed read / successful packet change respectively.
/// A stationary XInput packet is valid while a control is held; its age alone does not indicate failure.</summary>
public sealed record InputHealthSnapshot(long ReadCount, long PacketChanges, long ReadFailures,
    double LastReadAgeMilliseconds, double LastChangeAgeMilliseconds, double LastReadDurationMicroseconds,
    uint PacketNumber, uint ReadError, short LeftX, short LeftY, short RightX, short RightY,
    uint Buttons, uint L2, uint R2)
{
    public uint ReadStalls { get; init; }
}
public sealed record SessionStatus(BridgeState State, string? AccountName = null, string? Message = null);

public interface IBridgeBackend : IAsyncDisposable
{
    IReadOnlyList<ControllerInfo> GetControllers();
    Task<PairingKeys> PairAsync(string host, string accountId, string pin, CancellationToken cancellationToken);
    Task ConnectAsync(AccountProfile profile, BridgeOptions options, CancellationToken cancellationToken);
    Task UpdateOptionsAsync(BridgeOptions options, CancellationToken cancellationToken);
    Task StopAsync(CancellationToken cancellationToken);
    BackendSnapshot GetSnapshot();
}

public interface ISecretProtector
{
    byte[] Protect(byte[] plainText);
    byte[] Unprotect(byte[] encrypted);
}

public sealed class AccountStoreException(string message, Exception innerException) : Exception(message, innerException);
