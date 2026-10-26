using System.Runtime.Versioning;
using System.Text;
using Microsoft.Win32;
using Ps5Bridge.Core;

namespace Ps5Bridge.App;

/// <summary>Read-only access to the shared desktop Chiaki / Chiaki-ng Windows settings.</summary>
[SupportedOSPlatform("windows")]
internal static class ChiakiSettingsReader
{
    internal static ChiakiImporter.Result? ReadLocal(IEnumerable<AccountProfile> existing)
    {
        using var organization = Registry.CurrentUser.OpenSubKey(@"Software\Chiaki");
        return organization is null ? null : ReadFrom(organization, existing);
    }

    internal static ChiakiImporter.Result? ReadFrom(RegistryKey organization, IEnumerable<AccountProfile> existing)
    {
        var saved = existing.ToArray();
        using var defaults = organization.OpenSubKey(@"Chiaki\settings");
        var current = defaults?.GetValue("current_profile") as string;
        var currentKey = string.IsNullOrEmpty(current) ? "Chiaki" : "Chiaki-" + current;
        var names = organization.GetSubKeyNames()
            .Where(name => name.Equals("Chiaki", StringComparison.OrdinalIgnoreCase) ||
                name.StartsWith("Chiaki-", StringComparison.OrdinalIgnoreCase))
            .OrderBy(name => name.Equals(currentKey, StringComparison.OrdinalIgnoreCase) ? 0 :
                name.Equals("Chiaki", StringComparison.OrdinalIgnoreCase) ? 1 : 2)
            .ThenBy(name => name, StringComparer.OrdinalIgnoreCase);
        var results = new List<ChiakiImporter.Result>();
        foreach (var name in names)
        {
            using var profile = organization.OpenSubKey(name);
            if (profile is null) continue;
            var settings = new Dictionary<string, object?>(StringComparer.OrdinalIgnoreCase);
            ReadArray(profile, "registered_hosts", ["target", "server_nickname", "server_mac", "rp_regist_key", "rp_key"], settings);
            if (!settings.ContainsKey("registered_hosts/size")) continue;
            ReadArray(profile, "manual_hosts", ["host", "registered", "registered_mac"], settings);
            results.Add(ChiakiImporter.ParseSettings(settings, saved));
        }
        if (results.Count == 0) return null;
        var known = saved.Select(profile => profile.Keys.RegistrationKeyHex).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var imported = new List<AccountProfile>();
        var needsHost = new List<ChiakiImporter.HostRequest>();
        int duplicate = results.Sum(result => result.SkippedExisting);
        // Prefer a record with a usable address to an otherwise identical record without one.
        foreach (var profile in results.SelectMany(result => result.Imported))
        {
            if (known.Add(profile.Keys.RegistrationKeyHex)) imported.Add(profile);
            else duplicate++;
        }
        foreach (var request in results.SelectMany(result => result.NeedsHost))
        {
            if (known.Add(request.Keys.RegistrationKeyHex)) needsHost.Add(request);
            else duplicate++;
        }
        return new ChiakiImporter.Result(imported, needsHost, results.Sum(result => result.SkippedNotPs5),
            results.Sum(result => result.SkippedInvalid), duplicate, results.SelectMany(result => result.InvalidFields).ToArray());
    }

    private static void ReadArray(RegistryKey profile, string name, string[] fields, Dictionary<string, object?> settings)
    {
        using var array = profile.OpenSubKey(name);
        if (array?.GetValue("size") is not { } size) return;
        settings[name + "/size"] = size;
        if (!int.TryParse(size.ToString(), out var count) || count is < 0 or > 4096) return;
        for (int i = 1; i <= count; i++)
        {
            using var record = array.OpenSubKey(i.ToString(System.Globalization.CultureInfo.InvariantCulture));
            if (record is null) continue;
            foreach (var field in fields)
                settings[$"{name}/{i}/{field}"] = DecodeNativeValue(record.GetValue(field, null, RegistryValueOptions.DoNotExpandEnvironmentNames));
        }
    }

    private static object? DecodeNativeValue(object? value)
    {
        // QSettings stores QByteArray as UTF-16 @ByteArray(...) text, including in REG_BINARY.
        // Unlike INI exports, the payload here contains literal bytes represented as Latin-1 characters.
        if (value is byte[] bytes)
        {
            if (bytes.Length % 2 != 0) return null;
            value = Encoding.Unicode.GetString(bytes).TrimEnd('\0');
        }
        if (value is not string text) return value;
        if (text.StartsWith("@ByteArray(", StringComparison.Ordinal) && text.EndsWith(')'))
        {
            var payload = text[11..^1];
            return payload.All(c => c <= byte.MaxValue) ? Encoding.Latin1.GetBytes(payload) : null;
        }
        if (text.StartsWith("@@", StringComparison.Ordinal)) return text[1..];
        if (text.StartsWith("@String(", StringComparison.Ordinal) && text.EndsWith(')')) return text[8..^1];
        return text;
    }
}
