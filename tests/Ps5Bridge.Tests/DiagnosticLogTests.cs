using System.Text.Json;
using Ps5Bridge.Core;
using Xunit;

namespace Ps5Bridge.Tests;

public sealed class DiagnosticLogTests
{
    private static readonly InputHealthSnapshot Health = new(123, 7, 2, 3.5, 12.5, 6.25, 42, 1167,
        -32768, 32767, -1234, 5678, 0x1000, 128, 255);

    [Fact]
    public void RecoveredReadFailuresAndStallsRemainVisibleWithoutStatusChanges()
    {
        var log = new DiagnosticLog();
        var snapshot = new BackendSnapshot(BridgeState.Connected, true, 5, 1, 2) { InputHealth = Health };
        log.Observe(snapshot);
        snapshot = snapshot with { InputHealth = Health with { ReadFailures = 3, ReadStalls = 1 } };
        log.Observe(snapshot);
        log.Observe(snapshot);
        using var document = JsonDocument.Parse(log.Export(snapshot, new(), 0, "test"));
        var events = document.RootElement.GetProperty("Events");
        Assert.Equal(2, events.GetArrayLength());
        Assert.Equal(3, events[1].GetProperty("ReadFailures").GetInt64());
        Assert.Equal(1, events[1].GetProperty("ReadStalls").GetInt32());
    }

    [Fact]
    public void ExportIsBoundedAndOmitsUntrustedStatusAndDeviceIdentity()
    {
        var log = new DiagnosticLog();
        var snapshot = new BackendSnapshot(BridgeState.Connected, true, 5, double.NaN, double.NaN, "secret-key PIN-1234 192.168.1.20 account-name") { ControllerState = 3, ControllerSystemError = 5 };
        for (int i = 0; i < 300; i++) log.Observe(snapshot with { ControllerConnected = i % 2 == 0 });
        var json = log.Export(snapshot, new(4, 100) { DeviceId = "private-device-path" }, 1, "test", new(10, 2, 1, 1, 0));
        using var document = JsonDocument.Parse(json);
        Assert.Equal(2, document.RootElement.GetProperty("FormatVersion").GetInt32());
        Assert.Equal(DiagnosticLog.Capacity, document.RootElement.GetProperty("Events").GetArrayLength());
        foreach (var secret in new[] { "secret-key", "PIN-1234", "192.168.1.20", "account-name", "private-device-path" })
            Assert.DoesNotContain(secret, json);
        Assert.Equal(JsonValueKind.Null, document.RootElement.GetProperty("Current").GetProperty("MeanSubmitMicroseconds").ValueKind);
        Assert.Equal(3, document.RootElement.GetProperty("Current").GetProperty("ControllerState").GetInt32());
        Assert.Equal(10, document.RootElement.GetProperty("Input").GetProperty("ObservedChanges").GetInt32());
    }

    [Fact]
    public void UnchangedStatusStillRecordsHealthOncePerSecondAndStartsNewSessionsImmediately()
    {
        var clock = new ManualTimeProvider();
        var log = new DiagnosticLog(clock);
        var snapshot = new BackendSnapshot(BridgeState.Connected, true, 5, 1, 2) { InputHealth = Health };
        log.Observe(snapshot);
        clock.Advance(TimeSpan.FromMilliseconds(999));
        log.Observe(snapshot with { InputReports = 6, InputHealth = Health with { ReadCount = 124 } });
        clock.Advance(TimeSpan.FromMilliseconds(1));
        log.Observe(snapshot with { InputReports = 7, InputHealth = Health with { ReadCount = 125 } });
        log.BeginSession();
        log.Observe(snapshot with { InputReports = 0, InputHealth = Health with { ReadCount = 1 } });

        using var document = JsonDocument.Parse(log.Export(snapshot, new(), 0, "test"));
        var samples = document.RootElement.GetProperty("InputHealthSamples");
        Assert.Equal(3, samples.GetArrayLength());
        Assert.Equal(123, samples[0].GetProperty("InputHealth").GetProperty("ReadCount").GetInt64());
        Assert.Equal(125, samples[1].GetProperty("InputHealth").GetProperty("ReadCount").GetInt64());
        Assert.Equal(7, samples[1].GetProperty("InputReports").GetInt64());
        Assert.Equal(TimeSpan.FromSeconds(1), samples[1].GetProperty("TimeUtc").GetDateTimeOffset() - samples[0].GetProperty("TimeUtc").GetDateTimeOffset());
        Assert.Equal(samples[0].GetProperty("SessionId").GetGuid(), samples[1].GetProperty("SessionId").GetGuid());
        Assert.NotEqual(samples[1].GetProperty("SessionId").GetGuid(), samples[2].GetProperty("SessionId").GetGuid());
        Assert.Equal(2, document.RootElement.GetProperty("Events").GetArrayLength());
    }

    [Fact]
    public void HealthHistoryIsBoundedAndOnlyExportsNumericInputData()
    {
        var clock = new ManualTimeProvider();
        var log = new DiagnosticLog(clock);
        var snapshot = new BackendSnapshot(BridgeState.Connected, true, 5, 1, 2, "private-session-message")
        {
            InputHealth = Health,
            ControllerMessage = "private-controller-message",
            ControllerState = 4,
            ControllerSystemError = 1167
        };
        for (int i = 0; i < 300; i++)
        {
            log.Observe(snapshot with { InputReports = i, InputHealth = Health with { ReadCount = i } });
            clock.Advance(TimeSpan.FromSeconds(1));
        }
        var json = log.Export(snapshot, new(0) { DeviceId = "private-device-path" }, 0, "test");
        using var document = JsonDocument.Parse(json);
        var samples = document.RootElement.GetProperty("InputHealthSamples");
        Assert.Equal(DiagnosticLog.Capacity, samples.GetArrayLength());
        Assert.Equal(44, samples[0].GetProperty("InputReports").GetInt64());
        Assert.Equal(299, samples[DiagnosticLog.Capacity - 1].GetProperty("InputHealth").GetProperty("ReadCount").GetInt64());
        Assert.Equal(1, document.RootElement.GetProperty("Events").GetArrayLength());
        Assert.Equal(4, samples[0].GetProperty("ControllerState").GetInt32());
        Assert.Equal(1167, samples[0].GetProperty("ControllerSystemError").GetInt32());
        Assert.Equal(BridgeState.Connected, (BridgeState)samples[0].GetProperty("State").GetInt32());
        var current = document.RootElement.GetProperty("Current").GetProperty("InputHealth");
        Assert.Equal(Health, current.Deserialize<InputHealthSnapshot>());
        foreach (var secret in new[] { "private-session-message", "private-controller-message", "private-device-path" })
            Assert.DoesNotContain(secret, json);
    }

    [Fact]
    public void HealthSamplesRequireAnActiveSessionAndAnXInputHealthSnapshot()
    {
        var clock = new ManualTimeProvider();
        var log = new DiagnosticLog(clock);
        var snapshot = new BackendSnapshot(BridgeState.Connected, true, 5, 1, 2) { InputHealth = Health };
        foreach (var state in new[] { BridgeState.Idle, BridgeState.Stopping, BridgeState.Failed })
        {
            log.Observe(snapshot with { State = state });
            clock.Advance(TimeSpan.FromSeconds(1));
        }
        log.Observe(snapshot with { InputHealth = null });
        clock.Advance(TimeSpan.FromSeconds(1));
        log.Observe(snapshot with { State = BridgeState.Connecting });
        clock.Advance(TimeSpan.FromSeconds(1));
        log.Observe(snapshot);
        using var document = JsonDocument.Parse(log.Export(snapshot, new(), 0, "test"));
        var samples = document.RootElement.GetProperty("InputHealthSamples");
        Assert.Equal(2, samples.GetArrayLength());
        Assert.Equal(BridgeState.Connecting, (BridgeState)samples[0].GetProperty("State").GetInt32());
        Assert.Equal(BridgeState.Connected, (BridgeState)samples[1].GetProperty("State").GetInt32());
    }

    private sealed class ManualTimeProvider : TimeProvider
    {
        private DateTimeOffset now = new(2026, 10, 25, 12, 0, 0, TimeSpan.Zero);
        private long timestamp;
        public override long TimestampFrequency => TimeSpan.TicksPerSecond;
        public override long GetTimestamp() => timestamp;
        public override DateTimeOffset GetUtcNow() => now;
        public void Advance(TimeSpan duration) { now += duration; timestamp += duration.Ticks; }
    }
}
