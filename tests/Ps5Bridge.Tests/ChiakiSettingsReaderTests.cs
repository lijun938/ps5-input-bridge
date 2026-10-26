using System.Text;
using System.Runtime.Versioning;
using Microsoft.Win32;
using Ps5Bridge.App;
using Ps5Bridge.Core;
using Xunit;

namespace Ps5Bridge.Tests;

[SupportedOSPlatform("windows")]
public sealed class ChiakiSettingsReaderTests : IDisposable
{
    private readonly string testPath = @"Software\PS5InputBridge.Tests\" + Guid.NewGuid().ToString("N");
    private RegistryKey Organization => Registry.CurrentUser.CreateSubKey(testPath);
    private const string RegistrationHex = "31323334353637380000000000000000";

    private static void WriteProfile(RegistryKey organization, string name, string registration = RegistrationHex,
        string host = "192.168.1.50")
    {
        using var profile = organization.CreateSubKey(name);
        using var hosts = profile.CreateSubKey("registered_hosts");
        hosts.SetValue("size", 1, RegistryValueKind.DWord);
        using var record = hosts.CreateSubKey("1");
        record.SetValue("target", 1000100, RegistryValueKind.DWord);
        record.SetValue("server_nickname", "PS5 注册表");
        // Qt's REG_BINARY contains UTF-16 @ByteArray(...) text, not the raw secret bytes.
        static byte[] QtBinary(string hex) => Encoding.Unicode.GetBytes("@ByteArray(" +
            Encoding.Latin1.GetString(Convert.FromHexString(hex)) + ")\0");
        record.SetValue("rp_regist_key", QtBinary(registration), RegistryValueKind.Binary);
        record.SetValue("rp_key", QtBinary("000102035c78343208090a0b0c0d0e0f"), RegistryValueKind.Binary);
        record.SetValue("server_mac", "@ByteArray(ABCDEF)", RegistryValueKind.String);
        // Deliberately include unrelated credentials; the reader must not load them.
        using var settings = profile.CreateSubKey("settings");
        settings.SetValue("psn_auth_token", "ignored-synthetic-token");
        using var manuals = profile.CreateSubKey("manual_hosts");
        manuals.SetValue("size", 1, RegistryValueKind.DWord);
        using var address = manuals.CreateSubKey("1");
        address.SetValue("registered", "true");
        address.SetValue("registered_mac", "@ByteArray(ABCDEF)");
        address.SetValue("host", host);
    }

    [Fact]
    public void ReadsQtNativeBinaryWithoutApplyingIniEscapesOrModifyingSource()
    {
        using var organization = Organization;
        WriteProfile(organization, "Chiaki");
        var result = ChiakiSettingsReader.ReadFrom(organization, []);
        var profile = Assert.Single(Assert.IsType<ChiakiImporter.Result>(result).Imported);
        Assert.Equal(RegistrationHex, profile.Keys.RegistrationKeyHex);
        Assert.Equal("000102035c78343208090a0b0c0d0e0f", profile.Keys.MorningKeyHex);
        Assert.Equal("414243444546", profile.Keys.ConsoleMacHex);
        Assert.Equal("192.168.1.50", profile.Host);
        AccountStore.Validate(profile);
        using var record = organization.OpenSubKey(@"Chiaki\registered_hosts\1");
        Assert.Equal(RegistryValueKind.Binary, record!.GetValueKind("rp_regist_key"));
        using var settings = organization.OpenSubKey(@"Chiaki\settings");
        Assert.Equal("ignored-synthetic-token", settings!.GetValue("psn_auth_token"));
    }

    [Fact]
    public void ImportsNamedProfilesAndDeduplicatesAcrossThem()
    {
        using var organization = Organization;
        WriteProfile(organization, "Chiaki");
        WriteProfile(organization, "Chiaki-另一个配置");
        WriteProfile(organization, "Chiaki-账户二", "41424344454647480000000000000000");
        WriteProfile(organization, "pl_render_params", "51525354555657580000000000000000");
        var result = Assert.IsType<ChiakiImporter.Result>(ChiakiSettingsReader.ReadFrom(organization, []));
        Assert.Equal(2, result.Imported.Count);
        Assert.Equal(1, result.SkippedExisting);
    }

    [Fact]
    public void PrefersCurrentNamedProfileAddressWhenCredentialsAreShared()
    {
        using var organization = Organization;
        WriteProfile(organization, "Chiaki", host: "192.168.1.1");
        WriteProfile(organization, "Chiaki-当前", host: "192.168.1.2");
        using (var settings = organization.CreateSubKey(@"Chiaki\settings")) settings.SetValue("current_profile", "当前");
        var result = Assert.IsType<ChiakiImporter.Result>(ChiakiSettingsReader.ReadFrom(organization, []));
        Assert.Equal("192.168.1.2", Assert.Single(result.Imported).Host);
    }

    [Fact]
    public void ReturnsNoSourceWhenDesktopSettingsAreNotPresent()
    {
        using var organization = Organization;
        Assert.Null(ChiakiSettingsReader.ReadFrom(organization, []));
    }

    public void Dispose() => Registry.CurrentUser.DeleteSubKeyTree(testPath, throwOnMissingSubKey: false);
}
