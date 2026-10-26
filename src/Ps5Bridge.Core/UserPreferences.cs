using System.Text.Json;

namespace Ps5Bridge.Core;

public sealed record UserPreferences(int Version = 1, Guid? AccountId = null, int ControllerIndex = 0,
    string? DeviceId = null, int PollIntervalMicroseconds = 500, uint BackMapping = 0);

/// <summary>Non-secret UI choices, kept separately from encrypted pairing credentials.</summary>
public sealed class PreferencesStore(string path)
{
    private readonly SemaphoreSlim gate = new(1, 1);
    public bool Exists => File.Exists(path);
    public async Task<UserPreferences> LoadAsync(CancellationToken token = default)
    {
        try
        {
            if (!File.Exists(path)) return new();
            if (new FileInfo(path).Length > 16384) return new();
            var value = JsonSerializer.Deserialize<UserPreferences>(await File.ReadAllBytesAsync(path, token));
            return value is null ? new() : Normalize(value);
        }
        catch (JsonException) { return new(); }
    }

    public async Task SaveAsync(UserPreferences value, CancellationToken token = default)
    {
        await gate.WaitAsync(token);
        var temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
            await File.WriteAllBytesAsync(temporary, JsonSerializer.SerializeToUtf8Bytes(Normalize(value)), token);
            token.ThrowIfCancellationRequested();
            File.Move(temporary, path, true);
        }
        finally
        {
            try { if (File.Exists(temporary)) File.Delete(temporary); }
            finally { gate.Release(); }
        }
    }

    private static UserPreferences Normalize(UserPreferences value)
    {
        if (value.Version != 1) return new();
        var index = value.ControllerIndex is >= 0 and < 20 ? value.ControllerIndex : 0;
        var identity = index >= 4 && value.DeviceId is { Length: > 0 } && !value.DeviceId.Contains('\0')
            && System.Text.Encoding.UTF8.GetByteCount(value.DeviceId) < 1536 ? value.DeviceId : null;
        if (index >= 4 && identity is null) index = 0;
        return value with
        {
            AccountId = value.AccountId == Guid.Empty ? null : value.AccountId,
            ControllerIndex = index, DeviceId = identity,
            PollIntervalMicroseconds = value.PollIntervalMicroseconds is 100 or 500 or 1000 ? value.PollIntervalMicroseconds : 500,
            BackMapping = value.BackMapping <= 1 ? value.BackMapping : 0
        };
    }
}
