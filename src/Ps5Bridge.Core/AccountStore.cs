using System.Security.Cryptography;
using System.Text.Json;

namespace Ps5Bridge.Core;

public sealed class AccountStore(string filePath, ISecretProtector protector)
{
    private const int MaximumFileBytes = 1024 * 1024;
    private readonly SemaphoreSlim gate = new(1, 1);
    private sealed record Envelope(int Version, AccountProfile[] Profiles);

    public async Task<IReadOnlyList<AccountProfile>> LoadAsync(CancellationToken cancellationToken = default)
    {
        await gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        byte[]? plain = null;
        try
        {
            if (!File.Exists(filePath)) return Array.Empty<AccountProfile>();
            if (new FileInfo(filePath).Length > MaximumFileBytes) throw new InvalidDataException("账户文件过大。");
            var encrypted = await File.ReadAllBytesAsync(filePath, cancellationToken).ConfigureAwait(false);
            plain = protector.Unprotect(encrypted);
            var envelope = JsonSerializer.Deserialize<Envelope>(plain) ?? throw new InvalidDataException("账户文件为空。");
            if (envelope.Version != 1 || envelope.Profiles is null) throw new InvalidDataException("不支持的账户文件版本。");
            ValidateAll(envelope.Profiles);
            return envelope.Profiles;
        }
        catch (Exception exception) when (exception is not OperationCanceledException)
        {
            throw new AccountStoreException("无法读取账户文件。文件可能损坏，或由另一个 Windows 用户加密；原文件已保留。", exception);
        }
        finally
        {
            if (plain is not null) CryptographicOperations.ZeroMemory(plain);
            gate.Release();
        }
    }

    public async Task SaveAsync(IEnumerable<AccountProfile> profiles, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(profiles);
        var array = profiles.ToArray();
        ValidateAll(array);
        await gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        byte[]? plain = null;
        string? temporary = null;
        try
        {
            var destination = Path.GetFullPath(filePath);
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            plain = JsonSerializer.SerializeToUtf8Bytes(new Envelope(1, array));
            var encrypted = protector.Protect(plain);
            if (encrypted.Length > MaximumFileBytes) throw new InvalidDataException("账户文件过大。");
            temporary = destination + "." + Guid.NewGuid().ToString("N") + ".tmp";
            await using (var stream = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None,
                4096, FileOptions.Asynchronous | FileOptions.WriteThrough))
            {
                await stream.WriteAsync(encrypted, cancellationToken).ConfigureAwait(false);
                await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
            }
            cancellationToken.ThrowIfCancellationRequested();
            File.Move(temporary, destination, true);
            temporary = null;
        }
        finally
        {
            if (plain is not null) CryptographicOperations.ZeroMemory(plain);
            try
            {
                if (temporary is not null)
                {
                    try { File.Delete(temporary); }
                    catch (IOException) { /* Preserve the original write failure and allow later retry. */ }
                    catch (UnauthorizedAccessException) { /* Antivirus or another process may hold the temporary file. */ }
                }
            }
            finally { gate.Release(); }
        }
    }

    private static void ValidateAll(AccountProfile[] profiles)
    {
        if (profiles.Length > 64) throw new ArgumentException("最多保存 64 个账户。");
        var ids = new HashSet<Guid>();
        foreach (var profile in profiles)
        {
            Validate(profile);
            if (!ids.Add(profile.Id)) throw new ArgumentException("账户标识不能重复。");
        }
    }

    public static void Validate(AccountProfile profile)
    {
        ArgumentNullException.ThrowIfNull(profile);
        if (profile.Id == Guid.Empty) throw new ArgumentException("账户标识无效。");
        if (string.IsNullOrWhiteSpace(profile.Name) || profile.Name.Length > 80) throw new ArgumentException("账户名称需为 1–80 个字符。");
        ValidateHost(profile.Host);
        if (profile.AccountId != "") ValidateAccountId(profile.AccountId);
        if (profile.Keys is null || !IsHex(profile.Keys.RegistrationKeyHex, 32) || !IsHex(profile.Keys.MorningKeyHex, 32))
            throw new ArgumentException("配对凭据无效，需要重新配对。");
        if (profile.Keys.Target < 0 || (profile.Keys.ConsoleMacHex != "" && !IsHex(profile.Keys.ConsoleMacHex, 12)))
            throw new ArgumentException("主机配对信息无效。");
    }

    public static void ValidateHost(string host)
    {
        if (string.IsNullOrWhiteSpace(host) || host.Length > 253 || host.Trim() != host || Uri.CheckHostName(host) == UriHostNameType.Unknown)
            throw new ArgumentException("请输入主机的局域网 IP 地址或有效主机名，不要填写网址。");
    }

    public static void ValidateAccountId(string accountId)
    {
        try
        {
            if (string.IsNullOrWhiteSpace(accountId) || Convert.FromBase64String(accountId).Length != 8)
                throw new FormatException();
        }
        catch (FormatException) { throw new ArgumentException("PSN Account-ID 必须是 8 字节账户编号的 Base64 编码，不是在线昵称。"); }
    }

    private static bool IsHex(string? value, int length) => value?.Length == length && value.All(Uri.IsHexDigit);
}
