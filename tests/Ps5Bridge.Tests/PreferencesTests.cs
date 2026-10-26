using Ps5Bridge.Core;
using Xunit;

namespace Ps5Bridge.Tests;

public sealed class PreferencesTests
{
    [Fact]
    public async Task PreferencesRoundTripAndCorruptionFallsBackWithoutTouchingAccounts()
    {
        var directory = Path.Combine(Path.GetTempPath(), "bridge-prefs-" + Guid.NewGuid());
        Directory.CreateDirectory(directory);
        try
        {
            var path = Path.Combine(directory, "preferences.json");
            var store = new PreferencesStore(path);
            Assert.Equal(new UserPreferences(), await store.LoadAsync());
            await store.SaveAsync(new(ControllerIndex: 0, DeviceId: "wrong-mode"));
            Assert.Null((await store.LoadAsync()).DeviceId);
            await store.SaveAsync(new(ControllerIndex: 4, DeviceId: null));
            Assert.Equal(0, (await store.LoadAsync()).ControllerIndex);
            var saved = new UserPreferences(AccountId: Guid.NewGuid(), ControllerIndex: 4, DeviceId: "synthetic-device", PollIntervalMicroseconds: 100, BackMapping: 1);
            await store.SaveAsync(saved);
            Assert.Equal(saved, await store.LoadAsync());
            await File.WriteAllTextAsync(path, "broken");
            Assert.Equal(new UserPreferences(), await store.LoadAsync());
            await store.SaveAsync(new(Version: 99));
            Assert.Equal(new UserPreferences(), await store.LoadAsync());
            await store.SaveAsync(new(ControllerIndex: -1, PollIntervalMicroseconds: 1, BackMapping: 9));
            Assert.Equal(new UserPreferences(), await store.LoadAsync());
            Assert.Single(Directory.GetFiles(directory));
        }
        finally { Directory.Delete(directory, true); }
    }
}
