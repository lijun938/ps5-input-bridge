using System.Globalization;
using System.Text;

namespace Ps5Bridge.Core;

/// <summary>Converts desktop Chiaki / Chiaki-ng Qt settings into bridge accounts.</summary>
public static class ChiakiImporter
{
    public const int MaximumSettingsBytes = 4 * 1024 * 1024;
    private const int MaximumArraySize = 4096;

    public sealed record HostRequest(string Name, PairingKeys Keys)
    {
        public AccountProfile Complete(string host)
        {
            var profile = new AccountProfile(Guid.NewGuid(), Name, host.Trim(), "", Keys);
            AccountStore.Validate(profile);
            return profile;
        }
    }

    public sealed record Result(IReadOnlyList<AccountProfile> Imported, IReadOnlyList<HostRequest> NeedsHost,
        int SkippedNotPs5, int SkippedInvalid, int SkippedExisting, IReadOnlyList<string> InvalidFields);

    /// <summary>Reads a desktop Qt INI/CONF export. Other settings, including PSN tokens and PINs, are ignored.</summary>
    public static Result Parse(ReadOnlySpan<byte> ini, IEnumerable<AccountProfile> existing)
    {
        if (ini.Length > MaximumSettingsBytes) throw new FormatException("Chiaki 配置文件过大（上限 4 MB）。");
        string text;
        try { text = new UTF8Encoding(false, true).GetString(ini); }
        catch (DecoderFallbackException) { throw new FormatException("Chiaki 配置文件需要是 UTF-8 格式的 INI / CONF 文件。"); }
        var settings = new Dictionary<string, object?>(StringComparer.OrdinalIgnoreCase);
        var lines = text.TrimStart('\uFEFF').Replace("\r\n", "\n").Split('\n');
        var section = "";
        for (int i = 0; i < lines.Length; i++)
        {
            var line = lines[i].Trim();
            if (line.Length == 0 || line[0] is ';' or '#') continue;
            if (line.StartsWith('[') && line.EndsWith(']'))
            {
                section = line[1..^1].Replace('\\', '/');
                if (section.Equals("General", StringComparison.OrdinalIgnoreCase)) section = "";
                continue;
            }
            var equals = line.IndexOf('=');
            if (equals < 0) continue;
            var key = (section.Length == 0 ? "" : section + "/") + line[..equals].Trim().Replace('\\', '/');
            if (!IsHostSetting(key)) continue;
            var raw = line[(equals + 1)..];
            // Qt joins a line ending in an unescaped backslash with the next line.
            while (HasContinuation(raw) && i + 1 < lines.Length) raw = raw[..^1] + lines[++i];
            settings[key] = DecodeIniValue(raw);
        }
        return ParseSettings(settings, existing);
    }

    /// <param name="settings">Flattened Qt keys separated by '/', with QByteArray values already decoded to byte[].</param>
    public static Result ParseSettings(IReadOnlyDictionary<string, object?> settings, IEnumerable<AccountProfile> existing)
    {
        var count = ArraySize(settings, "registered_hosts", required: true);
        var addresses = ReadAddresses(settings);
        var known = existing.Select(profile => profile.Keys.RegistrationKeyHex).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var imported = new List<AccountProfile>();
        var needsHost = new List<HostRequest>();
        var invalidFields = new List<string>();
        int notPs5 = 0, invalid = 0, duplicate = 0;
        for (int i = 1; i <= count; i++)
        {
            var prefix = $"registered_hosts/{i}/";
            var target = Number(settings, prefix + "target");
            // Pre-PS5 Chiaki settings may not have a target field at all.
            if (target is null or < 1000000) { notPs5++; continue; }
            var registration = Bytes(settings, prefix + "rp_regist_key", 16);
            var morning = Bytes(settings, prefix + "rp_key", 16);
            var mac = Bytes(settings, prefix + "server_mac", 6);
            var bad = new List<string>();
            if (registration is null) bad.Add("rp_regist_key");
            if (morning is null) bad.Add("rp_key");
            if (mac is null) bad.Add("server_mac");
            if (bad.Count > 0)
            {
                invalid++;
                invalidFields.Add(string.Join("+", bad));
                continue;
            }
            var registrationHex = Convert.ToHexStringLower(registration!);
            if (!known.Add(registrationHex)) { duplicate++; continue; }
            var macHex = Convert.ToHexStringLower(mac!);
            var name = Text(settings, prefix + "server_nickname");
            if (string.IsNullOrWhiteSpace(name)) name = "Chiaki PS5";
            if (name.Length > 80) name = name[..80];
            var request = new HostRequest(name, new PairingKeys(registrationHex, Convert.ToHexStringLower(morning!),
                target.Value == 1000000 ? 1000100 : target.Value, macHex));
            if (addresses.TryGetValue(macHex, out var address)) imported.Add(request.Complete(address));
            else needsHost.Add(request);
        }
        return new Result(imported, needsHost, notPs5, invalid, duplicate, invalidFields);
    }

    private static Dictionary<string, string> ReadAddresses(IReadOnlyDictionary<string, object?> settings)
    {
        var addresses = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var count = ArraySize(settings, "manual_hosts", required: false);
        for (int i = 1; i <= count; i++)
        {
            var prefix = $"manual_hosts/{i}/";
            var registered = Text(settings, prefix + "registered");
            if (registered is not "true" and not "1") continue;
            var mac = Bytes(settings, prefix + "registered_mac", 6);
            var host = Text(settings, prefix + "host");
            if (mac is null || host is null) continue;
            try { AccountStore.ValidateHost(host); }
            catch (ArgumentException) { continue; }
            addresses.TryAdd(Convert.ToHexStringLower(mac), host);
        }
        return addresses;
    }

    private static int ArraySize(IReadOnlyDictionary<string, object?> settings, string array, bool required)
    {
        var count = Number(settings, array + "/size");
        if (count is >= 0 and <= MaximumArraySize) return count.Value;
        if (!required) return 0;
        throw new FormatException("Chiaki 配置中没有有效的已配对主机列表。请选择桌面版 Chiaki / Chiaki-ng 导出的 INI / CONF 文件。");
    }

    private static int? Number(IReadOnlyDictionary<string, object?> settings, string key) =>
        int.TryParse(Text(settings, key), NumberStyles.Integer, CultureInfo.InvariantCulture, out var number) ? number : null;

    private static string? Text(IReadOnlyDictionary<string, object?> settings, string key) =>
        settings.TryGetValue(key, out var value) ? Convert.ToString(value, CultureInfo.InvariantCulture)?.Trim() : null;

    private static byte[]? Bytes(IReadOnlyDictionary<string, object?> settings, string key, int length) =>
        settings.TryGetValue(key, out var value) && value is byte[] bytes && bytes.Length == length && bytes.Any(b => b != 0)
            ? bytes : null;

    private static bool IsHostSetting(string key) =>
        key.StartsWith("registered_hosts/", StringComparison.OrdinalIgnoreCase) ||
        key.StartsWith("manual_hosts/", StringComparison.OrdinalIgnoreCase);

    private static bool HasContinuation(string value)
    {
        int slashes = 0;
        for (int i = value.Length - 1; i >= 0 && value[i] == '\\'; i--) slashes++;
        return slashes % 2 == 1;
    }

    private static object? DecodeIniValue(string raw)
    {
        var decoded = new StringBuilder();
        bool quoted = false;
        raw = raw.Trim();
        for (int i = 0; i < raw.Length; i++)
        {
            var c = raw[i];
            if (c == '"') { quoted = !quoted; continue; }
            if (!quoted && c == ';') break;
            // An unquoted comma denotes a Qt string list, not a scalar host field.
            if (!quoted && c == ',') return null;
            if (c != '\\') { decoded.Append(c); continue; }
            if (++i == raw.Length) return null;
            c = raw[i];
            var escaped = c switch
            {
                'a' => '\a',
                'b' => '\b',
                'f' => '\f',
                'n' => '\n',
                'r' => '\r',
                't' => '\t',
                'v' => '\v',
                '\\' => '\\',
                '"' => '"',
                _ => '\0'
            };
            if (c is 'a' or 'b' or 'f' or 'n' or 'r' or 't' or 'v' or '\\' or '"')
            {
                decoded.Append(escaped);
                continue;
            }
            int radix;
            if (c == 'x') { radix = 16; i++; }
            else if (c is >= '0' and <= '7') radix = 8;
            else return null;
            int start = i, value = 0;
            while (i < raw.Length)
            {
                var digit = raw[i] switch
                {
                    >= '0' and <= '9' => raw[i] - '0',
                    >= 'a' and <= 'f' => raw[i] - 'a' + 10,
                    >= 'A' and <= 'F' => raw[i] - 'A' + 10,
                    _ => -1
                };
                if (digit < 0 || digit >= radix) break;
                value = (value * radix + digit) & 0xffff;
                i++;
            }
            if (i == start) return null;
            i--;
            decoded.Append((char)value);
        }
        if (quoted) return null;
        var result = decoded.ToString().Trim();
        if (result.StartsWith("@ByteArray(", StringComparison.Ordinal) && result.EndsWith(')'))
        {
            var payload = result[11..^1];
            return payload.All(c => c <= byte.MaxValue) ? Encoding.Latin1.GetBytes(payload) : null;
        }
        if (result.StartsWith("@@", StringComparison.Ordinal)) return result[1..];
        if (result.StartsWith("@String(", StringComparison.Ordinal) && result.EndsWith(')')) return result[8..^1];
        return result;
    }
}
