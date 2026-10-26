using System.Text;
using Ps5Bridge.Core;
using Xunit;

namespace Ps5Bridge.Tests;

public sealed class AccountPortableBundleTests
{
    private static AccountProfile Profile(string name) => AccountStoreTests.Profile(name) with
    {
        Keys = new PairingKeys(new string('1', 32), new string('2', 32), AccountStoreTests.Profile().Keys.Target,
            AccountStoreTests.Profile().Keys.ConsoleMacHex)
    };

    [Fact]
    public void RoundTripsAccountsAcrossProtectors()
    {
        var accounts = new[] { Profile("账户一"), Profile("账户二") with { AccountId = "" } };
        var file = AccountPortableBundle.Export(accounts, "correct horse");
        // Import never touches DPAPI, so it works on another Windows user or PC.
        Assert.Equal(accounts, AccountPortableBundle.Import(file, "correct horse"));
    }

    [Fact]
    public void WrongPasswordAndTamperingAreRejected()
    {
        var file = AccountPortableBundle.Export([Profile("账户一")], "correct horse");
        Assert.Throws<InvalidDataException>(() => AccountPortableBundle.Import(file, "wrong password"));
        var tampered = (byte[])file.Clone();
        tampered[^1] ^= 0x40;
        Assert.Throws<InvalidDataException>(() => AccountPortableBundle.Import(tampered, "correct horse"));
        Assert.Throws<InvalidDataException>(() => AccountPortableBundle.Import(Encoding.UTF8.GetBytes("{\"profiles\":[]}"), "correct horse"));
    }

    [Fact]
    public void RequiresARealPassword()
    {
        Assert.Throws<ArgumentException>(() => AccountPortableBundle.Export([Profile("账户一")], "short"));
        Assert.Throws<ArgumentException>(() => AccountPortableBundle.Export([Profile("账户一")], ""));
    }

    [Fact]
    public void CiphertextDoesNotContainTheKeys()
    {
        var profile = Profile("账户一");
        var file = AccountPortableBundle.Export([profile], "correct horse");
        var text = Encoding.UTF8.GetString(file);
        Assert.DoesNotContain(profile.Keys.RegistrationKeyHex, text);
        Assert.DoesNotContain(profile.AccountId, text);
        Assert.DoesNotContain("账户一", text);
    }

    [Fact]
    public void BoundsRejectOversizedFilesExcessAccountsAndExpensiveKdfBeforeDecrypting()
    {
        Assert.Throws<InvalidDataException>(() => AccountPortableBundle.Import(new byte[AccountPortableBundle.MaximumFileBytes + 1], "correct horse"));
        Assert.Throws<ArgumentException>(() => AccountPortableBundle.Export(Enumerable.Range(0, 65).Select(i => Profile($"账户{i}")), "correct horse"));
        var bundle = AccountPortableBundle.Export([Profile("账户一")], "correct horse");
        System.Buffers.Binary.BinaryPrimitives.WriteInt32LittleEndian(bundle.AsSpan("PS5BRIDGE-PORTABLE"u8.Length), 10_000_000);
        var error = Assert.Throws<InvalidDataException>(() => AccountPortableBundle.Import(bundle, "correct horse"));
        Assert.Contains("参数", error.Message);
    }

    [Fact]
    public async Task FileImportIsBoundedAndPreservesProfiles()
    {
        var path = Path.GetTempFileName();
        try
        {
            var profile = Profile("朋友");
            await File.WriteAllBytesAsync(path, AccountPortableBundle.Export([profile], "correct horse"));
            Assert.Equal(profile, Assert.Single(await AccountPortableBundle.ImportFileAsync(path, "correct horse")));
            await using (var stream = new FileStream(path, FileMode.Create)) stream.SetLength(AccountPortableBundle.MaximumFileBytes + 1L);
            await Assert.ThrowsAsync<InvalidDataException>(() => AccountPortableBundle.ImportFileAsync(path, "correct horse"));
        }
        finally { File.Delete(path); }
    }
}
