using System.Security.Cryptography;
using Ps5Bridge.Core;
using Xunit;

namespace Ps5Bridge.Tests;

public sealed class AccountStoreTests : IDisposable
{
    private readonly string directory = Path.Combine(Path.GetTempPath(), "ps5-bridge-test-" + Guid.NewGuid());
    private readonly AesProtector protector = new();
    private string FilePath => Path.Combine(directory, "accounts.dat");

    internal static AccountProfile Profile(string name = "账户一") => new(
        Guid.NewGuid(), name, "192.168.1.20", Convert.ToBase64String([1, 2, 3, 4, 5, 6, 7, 8]),
        new PairingKeys(new string('a', 32), new string('b', 32), 1000, "001122334455"));

    [Fact]
    public async Task FirstRunHasNoAccounts()
    {
        var store = new AccountStore(FilePath, protector);
        Assert.Empty(await store.LoadAsync());
    }

    [Fact]
    public async Task AccountsRoundTripWithoutPlaintextCredentials()
    {
        var first = Profile();
        var second = Profile("账户二");
        var store = new AccountStore(FilePath, protector);
        await store.SaveAsync([first, second]);
        Assert.Equal(new[] { first, second }, await store.LoadAsync());
        var bytes = await File.ReadAllBytesAsync(FilePath);
        Assert.DoesNotContain(first.Keys.RegistrationKeyHex, System.Text.Encoding.UTF8.GetString(bytes));
        Assert.DoesNotContain(first.AccountId, System.Text.Encoding.UTF8.GetString(bytes));
    }

    [Fact]
    public async Task InvalidReplacementDoesNotOverwriteSavedAccounts()
    {
        var store = new AccountStore(FilePath, protector);
        var original = Profile();
        await store.SaveAsync([original]);
        await Assert.ThrowsAsync<ArgumentException>(() => store.SaveAsync([Profile() with { AccountId = "wrong" }]));
        Assert.Equal(original, Assert.Single(await store.LoadAsync()));
    }

    [Fact]
    public async Task ImportedProfileWithoutAccountIdRoundTrips()
    {
        var store = new AccountStore(FilePath, protector);
        var imported = Profile() with { AccountId = "" };
        await store.SaveAsync([imported]);
        Assert.Equal(imported, Assert.Single(await store.LoadAsync()));
    }

    [Fact]
    public async Task CorruptionFailsClearlyAndPreservesFile()
    {
        var store = new AccountStore(FilePath, protector);
        await store.SaveAsync([Profile()]);
        var bytes = await File.ReadAllBytesAsync(FilePath);
        bytes[^1] ^= 0x80;
        await File.WriteAllBytesAsync(FilePath, bytes);
        await Assert.ThrowsAsync<AccountStoreException>(() => store.LoadAsync());
        Assert.Equal(bytes, await File.ReadAllBytesAsync(FilePath));
    }

    [Fact]
    public async Task DuplicateIdsAreRejectedBeforeSaving()
    {
        var profile = Profile();
        var store = new AccountStore(FilePath, protector);
        await Assert.ThrowsAsync<ArgumentException>(() => store.SaveAsync([profile, profile with { Name = "另一个" }]));
        Assert.False(File.Exists(FilePath));
    }

    [Theory]
    [InlineData("")]
    [InlineData("http://192.168.1.2")]
    [InlineData("host name")]
    public void InvalidHostsAreRejected(string host) =>
        Assert.Throws<ArgumentException>(() => AccountStore.Validate(Profile() with { Host = host }));

    public void Dispose()
    {
        if (Directory.Exists(directory)) Directory.Delete(directory, true);
        protector.Dispose();
    }

    private sealed class AesProtector : ISecretProtector, IDisposable
    {
        private readonly byte[] key = RandomNumberGenerator.GetBytes(32);
        public byte[] Protect(byte[] plainText)
        {
            byte[] nonce = RandomNumberGenerator.GetBytes(12), tag = new byte[16], ciphertext = new byte[plainText.Length];
            using var aes = new AesGcm(key, 16);
            aes.Encrypt(nonce, plainText, ciphertext, tag);
            return [.. nonce, .. tag, .. ciphertext];
        }
        public byte[] Unprotect(byte[] encrypted)
        {
            byte[] result = new byte[encrypted.Length - 28];
            using var aes = new AesGcm(key, 16);
            aes.Decrypt(encrypted.AsSpan(0, 12), encrypted.AsSpan(28), encrypted.AsSpan(12, 16), result);
            return result;
        }
        public void Dispose() => CryptographicOperations.ZeroMemory(key);
    }
}
