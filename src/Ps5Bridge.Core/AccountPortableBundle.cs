using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Ps5Bridge.Core;

/// <summary>
/// Password-protected account bundle for copying paired accounts to another PC.
/// The bundle is meant to be stored on a USB stick, so the password is stretched
/// with PBKDF2 and the payload is authenticated with AES-GCM.
/// </summary>
public static class AccountPortableBundle
{
    public const int MaximumFileBytes = 1024 * 1024;
    public const int MaximumAccounts = 64;
    private const int Version = 1;
    private const int SaltBytes = 16, NonceBytes = 12, TagBytes = 16, KeyBytes = 32, Iterations = 300_000;
    private static readonly byte[] Magic = "PS5BRIDGE-PORTABLE"u8.ToArray();

    public static byte[] Export(IEnumerable<AccountProfile> profiles, string password)
    {
        ArgumentNullException.ThrowIfNull(profiles);
        ValidatePassword(password);
        var accounts = profiles.Take(MaximumAccounts + 1).ToList();
        ValidateAccounts(accounts);
        var plain = JsonSerializer.SerializeToUtf8Bytes(new Envelope
        {
            Version = Version,
            Profiles = accounts
        });
        var salt = RandomNumberGenerator.GetBytes(SaltBytes);
        var nonce = RandomNumberGenerator.GetBytes(NonceBytes);
        var cipher = new byte[plain.Length];
        var tag = new byte[TagBytes];
        var key = DeriveKey(password, salt);
        try
        {
            if (plain.Length + Magic.Length + 4 + SaltBytes + NonceBytes + TagBytes > MaximumFileBytes)
                throw new InvalidDataException("账户文件过大（上限 1 MB）。");
            using var aes = new AesGcm(key, TagBytes);
            aes.Encrypt(nonce, plain, cipher, tag, Magic);
        }
        finally { CryptographicOperations.ZeroMemory(plain); CryptographicOperations.ZeroMemory(key); }
        var file = new byte[Magic.Length + 4 + SaltBytes + NonceBytes + TagBytes + cipher.Length];
        var span = file.AsSpan();
        Magic.CopyTo(span); span = span[Magic.Length..];
        // Iterations follow the magic as a little-endian int32.
        System.Buffers.Binary.BinaryPrimitives.WriteInt32LittleEndian(span, Iterations); span = span[4..];
        salt.CopyTo(span); span = span[SaltBytes..];
        nonce.CopyTo(span); span = span[NonceBytes..];
        tag.CopyTo(span); span = span[TagBytes..];
        cipher.CopyTo(span);
        return file;
    }

    public static IReadOnlyList<AccountProfile> Import(ReadOnlySpan<byte> file, string password)
    {
        ValidatePassword(password);
        if (file.Length > MaximumFileBytes) throw new InvalidDataException("账户文件过大（上限 1 MB）。");
        if (file.Length < Magic.Length + 4 + SaltBytes + NonceBytes + TagBytes || !file[..Magic.Length].SequenceEqual(Magic))
            throw new InvalidDataException("这不是本程序导出的账户文件。");
        var iterations = System.Buffers.Binary.BinaryPrimitives.ReadInt32LittleEndian(file.Slice(Magic.Length, 4));
        if (iterations != Iterations) throw new InvalidDataException("账户文件的加密参数无效。");
        var offset = Magic.Length + 4;
        var salt = file.Slice(offset, SaltBytes).ToArray(); offset += SaltBytes;
        var nonce = file.Slice(offset, NonceBytes).ToArray(); offset += NonceBytes;
        var tag = file.Slice(offset, TagBytes); offset += TagBytes;
        var cipher = file[offset..].ToArray();
        var plain = new byte[cipher.Length];
        var key = DeriveKey(password, salt, iterations);
        try
        {
            using var aes = new AesGcm(key, TagBytes);
            try { aes.Decrypt(nonce, cipher, tag, plain, Magic); }
            catch (CryptographicException) { throw new InvalidDataException("密码不正确，或账户文件已损坏。"); }
            var envelope = JsonSerializer.Deserialize<Envelope>(plain) ?? throw new InvalidDataException("账户文件为空。");
            if (envelope.Version != Version || envelope.Profiles is null) throw new InvalidDataException("不支持的账户文件版本。");
            ValidateAccounts(envelope.Profiles);
            return envelope.Profiles;
        }
        finally { CryptographicOperations.ZeroMemory(plain); CryptographicOperations.ZeroMemory(key); }
    }

    public static async Task<IReadOnlyList<AccountProfile>> ImportFileAsync(string path, string password, CancellationToken token = default)
    {
        await using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 4096, FileOptions.Asynchronous);
        if (stream.Length > MaximumFileBytes) throw new InvalidDataException("账户文件过大（上限 1 MB）。");
        var file = new byte[checked((int)stream.Length)];
        await stream.ReadExactlyAsync(file, token);
        // PBKDF2 itself cannot be interrupted, but it never occupies the UI thread.
        var result = await Task.Run(() => Import(file, password), token);
        token.ThrowIfCancellationRequested();
        return result;
    }

    private static void ValidateAccounts(IReadOnlyList<AccountProfile> profiles)
    {
        if (profiles.Count > MaximumAccounts) throw new ArgumentException("最多保存 64 个账户。");
        var ids = new HashSet<Guid>();
        foreach (var profile in profiles)
        {
            AccountStore.Validate(profile);
            if (!ids.Add(profile.Id)) throw new ArgumentException("账户标识不能重复。");
        }
    }

    private static byte[] DeriveKey(string password, byte[] salt, int iterations = Iterations)
    {
        var bytes = Encoding.UTF8.GetBytes(password);
        try { return Rfc2898DeriveBytes.Pbkdf2(bytes, salt, iterations, HashAlgorithmName.SHA256, KeyBytes); }
        finally { CryptographicOperations.ZeroMemory(bytes); }
    }

    private static void ValidatePassword(string password)
    {
        if (string.IsNullOrEmpty(password) || password.Length < 8 || password.Length > 200)
            throw new ArgumentException("请设置至少 8 个字符的密码。");
    }

    private sealed class Envelope
    {
        public int Version { get; set; }
        public List<AccountProfile>? Profiles { get; set; }
    }
}
