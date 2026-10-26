using System.Text;
using Ps5Bridge.Core;
using Xunit;

namespace Ps5Bridge.Tests;

public sealed class ChiakiImporterTests
{
    // Qt desktop Chiaki/Chiaki-ng settings schema, using synthetic credentials only.
    private const string Registration = "@ByteArray(12345678\\0\\0\\0\\0\\0\\0\\0\\0)";
    private const string Morning = "@ByteArray(\\0\\x1\\x2\\x3\\x4\\x5\\x6\\a\\b\\t\\n\\v\\f\\r\\xe\\xf)";
    private const string Mac = "@ByteArray(\\xaa\\xbb\\xcc\\xdd\\xee\\xff)";
    private const string RegistrationHex = "31323334353637380000000000000000";

    private static string Registered(int index = 1, string target = "1000100", string registration = Registration,
        string morning = Morning, string mac = Mac, string name = "PS5 客厅") => $$"""
        {{index}}\target={{target}}
        {{index}}\server_nickname={{name}}
        {{index}}\server_mac={{mac}}
        {{index}}\rp_regist_key={{registration}}
        {{index}}\rp_key={{morning}}
        """;

    private static string Manual(int index = 1, string host = "192.168.1.50", string mac = Mac,
        string registered = "true") => $$"""
        {{index}}\host={{host}}
        {{index}}\registered={{registered}}
        {{index}}\registered_mac={{mac}}
        """;

    private static byte[] Ini(string records, string manuals = "", int count = 1, int manualCount = 1) =>
        Encoding.UTF8.GetBytes($"[General]\nversion=2\n[registered_hosts]\nsize={count}\n{records}\n[manual_hosts]\nsize={manualCount}\n{manuals}\n");

    [Fact]
    public void ImportsQtDesktopSettingsWithExactCredentialBytes()
    {
        var result = ChiakiImporter.Parse(Ini(Registered(), Manual()), []);
        var profile = Assert.Single(result.Imported);
        Assert.Empty(result.NeedsHost);
        Assert.Equal("PS5 客厅", profile.Name);
        Assert.Equal("192.168.1.50", profile.Host);
        Assert.Equal("", profile.AccountId);
        Assert.Equal(RegistrationHex, profile.Keys.RegistrationKeyHex);
        Assert.Equal("000102030405060708090a0b0c0d0e0f", profile.Keys.MorningKeyHex);
        Assert.Equal("aabbccddeeff", profile.Keys.ConsoleMacHex);
        Assert.Equal(1000100, profile.Keys.Target);
        AccountStore.Validate(profile);
    }

    [Fact]
    public void MatchesManualAddressByMacRatherThanArrayPosition()
    {
        var manuals = Manual(1, "192.168.1.99", "@ByteArray(ABCDEF)") + "\n" + Manual(2, "ps5.lan");
        var result = ChiakiImporter.Parse(Ini(Registered(), manuals, manualCount: 2), []);
        Assert.Equal("ps5.lan", Assert.Single(result.Imported).Host);
    }

    [Fact]
    public void PreservesQuotedCommaSemicolonBackslashAndUnicodeNames()
    {
        var name = "\"PS5, \\x4e3b\\x673a; \\\"A\\\" \\\\ B\"";
        var ini = Encoding.UTF8.GetString(Ini(Registered(name: name), Manual()))
            .Replace("[General]", "\uFEFF[General]").Replace("\n", "\r\n");
        var profile = Assert.Single(ChiakiImporter.Parse(Encoding.UTF8.GetBytes(ini), []).Imported);
        Assert.Equal("PS5, 主机; \"A\" \\ B", profile.Name);
    }

    [Fact]
    public void AcceptsQuotedByteArraysWithLiteralDelimitersAndEscapedHexDigits()
    {
        // Sixteen exact bytes, including quotes, parentheses, semicolon, comma, slash and null.
        var key = "\"@ByteArray(\\x1\\x61\\\"),;\\\\\\0ABCDEFGH)\"";
        var result = ChiakiImporter.Parse(Ini(Registered(morning: key), Manual()), []);
        Assert.Equal("016122292c3b5c004142434445464748", Assert.Single(result.Imported).Keys.MorningKeyHex);
    }

    [Theory]
    [InlineData("")]
    [InlineData("http://192.168.1.50")]
    public void RequestsHostWhenManualAddressIsMissingOrInvalid(string host)
    {
        var result = ChiakiImporter.Parse(Ini(Registered(), Manual(host: host)), []);
        Assert.Empty(result.Imported);
        var request = Assert.Single(result.NeedsHost);
        Assert.Equal("PS5 客厅", request.Name);
        var profile = request.Complete("192.168.1.60");
        Assert.Equal("192.168.1.60", profile.Host);
        Assert.Equal(RegistrationHex, profile.Keys.RegistrationKeyHex);
        AccountStore.Validate(profile);
        Assert.Throws<ArgumentException>(() => request.Complete("http://bad"));
    }

    [Fact]
    public void DoesNotUseAnUnregisteredOrDifferentConsolesAddress()
    {
        var manuals = Manual(1, registered: "false") + "\n" + Manual(2, mac: "@ByteArray(ABCDEF)");
        var result = ChiakiImporter.Parse(Ini(Registered(), manuals, manualCount: 2), []);
        Assert.Single(result.NeedsHost);
        Assert.Empty(result.Imported);
    }

    [Fact]
    public void SkipsExistingHeliosCredentialsWithoutPromptingOrChangingExistingProfile()
    {
        var original = AccountStoreTests.Profile() with
        {
            Keys = new PairingKeys(RegistrationHex.ToUpperInvariant(), new string('b', 32), 1000100)
        };
        var result = ChiakiImporter.Parse(Ini(Registered()), [original]);
        Assert.Empty(result.Imported);
        Assert.Empty(result.NeedsHost);
        Assert.Equal(1, result.SkippedExisting);
        Assert.Equal("192.168.1.20", original.Host);
    }

    [Fact]
    public void SkipsDuplicateRecordsInsideOneFile()
    {
        var result = ChiakiImporter.Parse(Ini(Registered() + "\n" + Registered(2), Manual(), count: 2), []);
        Assert.Single(result.Imported);
        Assert.Equal(1, result.SkippedExisting);
    }

    [Fact]
    public void SkipsPs4AndMalformedCredentialsWhileImportingOtherRecords()
    {
        var records = string.Join("\n", Registered(), Registered(2, target: "1000"),
            Registered(3, registration: "@ByteArray(short)"),
            Registered(4, morning: "@ByteArray(\\xGG)"),
            Registered(5, mac: "@ByteArray(short)"));
        var result = ChiakiImporter.Parse(Ini(records, Manual(), count: 5), []);
        Assert.Single(result.Imported);
        Assert.Equal(1, result.SkippedNotPs5);
        Assert.Equal(3, result.SkippedInvalid);
        Assert.Contains("rp_regist_key", result.InvalidFields);
        Assert.Contains("rp_key", result.InvalidFields);
        Assert.Contains("server_mac", result.InvalidFields);
        Assert.DoesNotContain(RegistrationHex, string.Join(",", result.InvalidFields));
    }

    [Fact]
    public void AcceptsNormalizedRegistryValuesWithoutReinterpretingBinaryBytes()
    {
        var settings = new Dictionary<string, object?>
        {
            ["registered_hosts/size"] = 1,
            ["registered_hosts/1/target"] = 1000100,
            ["registered_hosts/1/server_nickname"] = "PS5 Registry",
            ["registered_hosts/1/server_mac"] = Convert.FromHexString("aabbccddeeff"),
            ["registered_hosts/1/rp_regist_key"] = Convert.FromHexString(RegistrationHex),
            ["registered_hosts/1/rp_key"] = Convert.FromHexString("000102030405060708090a0b0c0d0e0f"),
            ["manual_hosts/size"] = 1,
            ["manual_hosts/1/host"] = "192.168.1.70",
            ["manual_hosts/1/registered"] = "true",
            ["manual_hosts/1/registered_mac"] = Convert.FromHexString("aabbccddeeff")
        };
        var result = ChiakiImporter.ParseSettings(settings, []);
        Assert.Equal("192.168.1.70", Assert.Single(result.Imported).Host);
    }

    [Theory]
    [InlineData("[General]\nversion=2")]
    [InlineData("[registered_hosts]\nsize=abc")]
    [InlineData("[registered_hosts]\nsize=999999999")]
    [InlineData("{\"profiles\":[]}")]
    public void RejectsUnrelatedOrInvalidSettingsWithoutCredentialValues(string input)
    {
        Assert.Throws<FormatException>(() => ChiakiImporter.Parse(Encoding.UTF8.GetBytes(input), []));
    }

    [Fact]
    public void EmptyRegisteredHostListIsAValidEmptyImport()
    {
        var result = ChiakiImporter.Parse(Ini("", count: 0, manualCount: 0), []);
        Assert.Empty(result.Imported);
        Assert.Empty(result.NeedsHost);
    }

    [Fact]
    public void RejectsOversizedSettingsBeforeParsing()
    {
        Assert.Throws<FormatException>(() => ChiakiImporter.Parse(new byte[4 * 1024 * 1024 + 1], []));
    }
}
