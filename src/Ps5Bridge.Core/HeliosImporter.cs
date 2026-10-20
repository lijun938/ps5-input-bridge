using System.Text.Json;

namespace Ps5Bridge.Core;

/// <summary>Converts Helios "PS Remote Play" profiles into bridge accounts. Decryption is done by the caller.</summary>
public static class HeliosImporter
{
    public const int Ps5Target = 1000100; // CHIAKI_TARGET_PS5_1

    public static string DefaultCredentialsPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "HeliosProject", "Helios", "remote_play", "playstation", "credentials.dat");

    /// <param name="InvalidFields">For each skipped incomplete profile, the Helios field names that could not be used (no values).</param>
    public sealed record Result(IReadOnlyList<AccountProfile> Imported, int SkippedNotPs5, int SkippedInvalid, int SkippedExisting,
        IReadOnlyList<string> InvalidFields);

    /// <param name="json">Decrypted Helios credentials JSON.</param>
    /// <param name="existing">Already saved accounts; a profile with the same registration key is skipped.</param>
    public static Result Parse(ReadOnlySpan<byte> json, IEnumerable<AccountProfile> existing)
    {
        var known = existing.Select(profile => profile.Keys.RegistrationKeyHex).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var imported = new List<AccountProfile>();
        int notPs5 = 0, invalid = 0, duplicate = 0;
        var invalidFields = new List<string>();
        using var document = JsonDocument.Parse(json.ToArray());
        if (!document.RootElement.TryGetProperty("profiles", out var profiles) || profiles.ValueKind != JsonValueKind.Array)
            throw new FormatException("Helios 凭据文件中没有账户列表。");
        foreach (var item in profiles.EnumerateArray())
        {
            if (!(item.TryGetProperty("is_ps5", out var ps5) && ps5.ValueKind == JsonValueKind.True)) { notPs5++; continue; }
            var name = Text(item, "display_name") ?? Text(item, "nickname") ?? "Helios 账户";
            if (name.Length > 80) name = name[..80];
            // Helios' account_name is not a Base64 Account-ID; LAN connections do not need one.
            var accountId = Text(item, "account_name") ?? "";
            if (!IsAccountId(accountId)) accountId = "";
            var profile = new AccountProfile(Guid.NewGuid(), name, Text(item, "host_address") ?? "", accountId,
                new PairingKeys(Hex(item, "registration_key"), Hex(item, "morning_key"), Ps5Target, Hex(item, "server_mac")));
            try { AccountStore.Validate(profile); }
            catch (ArgumentException)
            {
                invalid++;
                invalidFields.Add(string.Join("+", BadFields(profile)));
                continue;
            }
            if (!known.Add(profile.Keys.RegistrationKeyHex)) { duplicate++; continue; }
            imported.Add(profile);
        }
        return new Result(imported, notPs5, invalid, duplicate, invalidFields);
    }

    private static IEnumerable<string> BadFields(AccountProfile profile)
    {
        var bad = new List<string>();
        void Check(string field, Action validate)
        {
            try { validate(); } catch (ArgumentException) { bad.Add(field); }
        }
        Check("host_address", () => AccountStore.ValidateHost(profile.Host));
        if (!IsHex(profile.Keys.RegistrationKeyHex, 32)) bad.Add("registration_key");
        if (!IsHex(profile.Keys.MorningKeyHex, 32)) bad.Add("morning_key");
        if (profile.Keys.ConsoleMacHex != "" && !IsHex(profile.Keys.ConsoleMacHex, 12)) bad.Add("server_mac");
        if (bad.Count == 0) bad.Add("display_name");
        return bad;
    }

    private static bool IsHex(string value, int length) => value.Length == length && value.All(Uri.IsHexDigit);

    /// <summary>Hex text normalized to lowercase without ':'/'-'/space separators.</summary>
    private static string Hex(JsonElement item, string name) =>
        new string((Text(item, name) ?? "").Where(c => c is not (':' or '-' or ' ')).ToArray()).ToLowerInvariant();

    private static bool IsAccountId(string value)
    {
        try { AccountStore.ValidateAccountId(value); return true; }
        catch (ArgumentException) { return false; }
    }

    private static string? Text(JsonElement item, string name) =>
        item.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString()?.Trim() : null;
}
