using Ps5Bridge.App;
using Xunit;

namespace Ps5Bridge.Tests;

public sealed class WindowsSecretProtectorTests
{
    [Fact]
    public void WindowsDpapiRoundTripsWithAnotherProtectorInstance()
    {
        byte[] secret = "registration-credentials-test"u8.ToArray();
        var encrypted = new WindowsSecretProtector().Protect(secret);
        Assert.NotEqual(secret, encrypted);
        Assert.Equal(secret, new WindowsSecretProtector().Unprotect(encrypted));
    }

    [Fact]
    public void WindowsDpapiRejectsCorruptedCiphertext()
    {
        var protector = new WindowsSecretProtector();
        var encrypted = protector.Protect("test"u8.ToArray());
        encrypted[^1] ^= 0x80;
        Assert.Throws<System.Security.Cryptography.CryptographicException>(() => protector.Unprotect(encrypted));
    }
}
