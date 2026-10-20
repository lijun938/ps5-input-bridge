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
}
