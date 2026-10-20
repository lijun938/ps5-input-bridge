using System.Text;
using Ps5Bridge.Core;
using Xunit;

namespace Ps5Bridge.Tests;

public sealed class HeliosImporterTests
{
    private static string Entry(string account, string host = "192.168.1.50", bool ps5 = true, string regist = "",
        string display = "主机 A") => $$"""
        {"account_name":"{{account}}","display_name":"{{display}}","host_address":"{{host}}","is_ps5":{{(ps5 ? "true" : "false")}},
         "morning_key":"{{new string('B', 32)}}","nickname":"PS5-123","registration_key":"{{(regist == "" ? new string('A', 32) : regist)}}",
         "server_mac":"AABBCCDDEEFF"}
        """;

    private static byte[] File(params string[] entries) =>
        Encoding.UTF8.GetBytes($$"""{"schema_version":1,"profiles":[{{string.Join(",", entries)}}]}""");

    private static readonly string AccountA = Convert.ToBase64String([1, 2, 3, 4, 5, 6, 7, 8]);
    private static readonly string AccountB = Convert.ToBase64String([8, 7, 6, 5, 4, 3, 2, 1]);

    [Fact]
    public void ImportsPs5ProfilesAsValidAccounts()
    {
        var result = HeliosImporter.Parse(File(Entry(AccountA),
            Entry(AccountB, display: "主机 B", regist: new string('D', 32))), []);
        Assert.Equal(2, result.Imported.Count);
        var first = result.Imported[0];
        Assert.Equal("主机 A", first.Name);
        Assert.Equal("192.168.1.50", first.Host);
        Assert.Equal(AccountA, first.AccountId);
        Assert.Equal(new string('a', 32), first.Keys.RegistrationKeyHex);
        Assert.Equal(new string('b', 32), first.Keys.MorningKeyHex);
        Assert.Equal("aabbccddeeff", first.Keys.ConsoleMacHex);
        Assert.Equal(HeliosImporter.Ps5Target, first.Keys.Target);
        foreach (var profile in result.Imported) AccountStore.Validate(profile);
        Assert.NotEqual(result.Imported[0].Id, result.Imported[1].Id);
    }

    [Fact]
    public void SkipsPs4InvalidAndAlreadySavedProfiles()
    {
        var existing = AccountStoreTests.Profile() with
        {
            Keys = new PairingKeys(new string('c', 32), new string('b', 32), HeliosImporter.Ps5Target)
        };
        var result = HeliosImporter.Parse(File(
            Entry(AccountA, ps5: false),
            Entry(AccountA, regist: "not-hex"),
            Entry(AccountA, regist: new string('C', 32)),
            Entry(AccountA, host: "192.168.1.60")), [existing]);
        Assert.Equal((1, 1, 1), (result.SkippedNotPs5, result.SkippedInvalid, result.SkippedExisting));
        Assert.Equal("192.168.1.60", Assert.Single(result.Imported).Host);
        Assert.Equal("registration_key", Assert.Single(result.InvalidFields));
    }

    [Fact]
    public void OnlineIdAccountNameImportsWithoutAccountId()
    {
        var result = HeliosImporter.Parse(File(Entry("PlayerName12")), []);
        var profile = Assert.Single(result.Imported);
        Assert.Equal("", profile.AccountId);
        AccountStore.Validate(profile);
    }

    [Fact]
    public void AcceptsSeparatedMac()
    {
        var withColons = Entry(AccountA).Replace("AABBCCDDEEFF", "AA:BB:CC:DD:EE:FF");
        Assert.Equal("aabbccddeeff", Assert.Single(HeliosImporter.Parse(File(withColons), []).Imported).Keys.ConsoleMacHex);
    }

    [Fact]
    public void ReimportSkipsProfilesAlreadySaved()
    {
        var first = HeliosImporter.Parse(File(Entry("PlayerName12")), []);
        var second = HeliosImporter.Parse(File(Entry("PlayerName12")), first.Imported);
        Assert.Empty(second.Imported);
        Assert.Equal(1, second.SkippedExisting);
    }

    [Fact]
    public void RejectsFileWithoutProfiles()
    {
        Assert.Throws<FormatException>(() => HeliosImporter.Parse("""{"schema_version":1}"""u8, []));
    }
}
