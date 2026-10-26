using System.Text.Json;

namespace Ps5Bridge.Core;

/// <summary>Allowlisted diagnostics: never accepts account text, hostnames, paths, PINs or keys.</summary>
public sealed class DiagnosticLog
{
    public const int Capacity = 256;
    private readonly TimeProvider clock;
    private readonly Queue<DiagnosticEvent> events = new();
    private readonly Queue<InputHealthSample> inputHealthSamples = new();
    private Guid sessionId = Guid.NewGuid();
    private (BridgeState, bool, long, uint, uint, long, uint)? previous;
    private long? lastHealthTimestamp;

    public DiagnosticLog(TimeProvider? timeProvider = null) { clock = timeProvider ?? TimeProvider.System; }

    public void BeginSession() { sessionId = Guid.NewGuid(); previous = null; lastHealthTimestamp = null; }

    public void Observe(BackendSnapshot snapshot)
    {
        var now = clock.GetUtcNow();
        var timestamp = clock.GetTimestamp();
        // Sample before event deduplication: a stuck input can leave every status flag unchanged.
        if (snapshot.State is BridgeState.Connecting or BridgeState.Connected && snapshot.InputHealth is { } health &&
            (lastHealthTimestamp is not { } last || clock.GetElapsedTime(last, timestamp) >= TimeSpan.FromSeconds(1)))
        {
            inputHealthSamples.Enqueue(new(now, sessionId, snapshot.State, snapshot.ControllerConnected,
                snapshot.InputReports, snapshot.ControllerState, snapshot.ControllerSystemError, health));
            while (inputHealthSamples.Count > Capacity) inputHealthSamples.Dequeue();
            lastHealthTimestamp = timestamp;
        }
        var signature = (snapshot.State, snapshot.ControllerConnected, snapshot.Latency?.SubmitFailures ?? 0,
            snapshot.ControllerState, snapshot.ControllerSystemError, snapshot.InputHealth?.ReadFailures ?? 0,
            snapshot.InputHealth?.ReadStalls ?? 0);
        if (previous == signature) return;
        previous = signature;
        events.Enqueue(new(now, sessionId, snapshot.State, snapshot.ControllerConnected, signature.Item3,
            snapshot.ControllerState, snapshot.ControllerSystemError, signature.Item6, signature.Item7));
        while (events.Count > Capacity) events.Dequeue();
    }

    public string Export(BackendSnapshot snapshot, BridgeOptions options, uint backMapping, string version, InputDiagnosticCounters? input = null)
    {
        // Explicit fields rather than serializing product models, which also contain user data.
        return JsonSerializer.Serialize(new
        {
            FormatVersion = 2, ApplicationVersion = version, CreatedUtc = clock.GetUtcNow(), SessionId = sessionId,
            Settings = new { InputMode = options.ControllerIndex >= 4 ? "HID" : "XInput", options.PollIntervalMicroseconds, BackMapping = backMapping },
            Current = new
            {
                snapshot.State, snapshot.ControllerConnected, snapshot.InputReports, snapshot.ControllerState, snapshot.ControllerSystemError,
                MeanSubmitMicroseconds = double.IsFinite(snapshot.MeanSubmitMicroseconds) ? (double?)snapshot.MeanSubmitMicroseconds : null,
                MaxSubmitMicroseconds = double.IsFinite(snapshot.MaxSubmitMicroseconds) ? (double?)snapshot.MaxSubmitMicroseconds : null,
                snapshot.Latency, snapshot.InputHealth
            },
            Input = input, Events = events.ToArray(), InputHealthSamples = inputHealthSamples.ToArray()
        }, new JsonSerializerOptions { WriteIndented = true });
    }

    private sealed record DiagnosticEvent(DateTimeOffset TimeUtc, Guid SessionId, BridgeState State, bool ControllerConnected,
        long SubmitFailures, uint ControllerState, uint ControllerSystemError, long ReadFailures, uint ReadStalls);
    private sealed record InputHealthSample(DateTimeOffset TimeUtc, Guid SessionId, BridgeState State,
        bool ControllerConnected, long InputReports, uint ControllerState, uint ControllerSystemError, InputHealthSnapshot InputHealth);
}

public sealed record InputDiagnosticCounters(long ObservedChanges, long SubmitFailures, long L1Presses, long R1Presses, long Chords);
