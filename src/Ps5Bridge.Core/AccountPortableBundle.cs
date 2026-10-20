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
    private const int Version = 1;
    private const int SaltBytes = 16, NonceBytes = 12, TagBytes = 16, KeyBytes = 32, Iterations = 300_000;
    private static readonly byte[] Magic = "PS5BRIDGE-PORTABLE"u8.ToArray();

    public static byte[] Export(IEnumerable<AccountProfile> profiles, string password)
    {
        ArgumentNullException.ThrowIfNull(profiles);
        ValidatePassword(password);
        var plain = JsonSerializer.SerializeToUtf8Bytes(new Envelope
        {
            Version = Version,
            Profiles = profiles.ToList()
        });
        var salt = RandomNumberGenerator.GetBytes(SaltBytes);
        var nonce = RandomNumberGenerator.GetBytes(NonceBytes);
        var cipher = new byte[plain.Length];
        var tag = new byte[TagBytes];
        using var aes = new AesGcm(DeriveKey(password, salt), TagBytes);
        try { aes.Encrypt(nonce, plain, cipher, tag, Magic); }
        finally { CryptographicOperations.ZeroMemory(plain); }
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
        if (file.Length < Magic.Length + 4 + SaltBytes + NonceBytes + TagBytes || !file[..Magic.Length].SequenceEqual(Magic))
            throw new InvalidDataException("这不是本程序导出的账户文件。");
        var iterations = System.Buffers.Binary.BinaryPrimitives.ReadInt32LittleEndian(file.Slice(Magic.Length, 4));
        if (iterations is < 1 or > 10_000_000) throw new InvalidDataException("账户文件的加密参数无效。");
        var offset = Magic.Length + 4;
        var salt = file.Slice(offset, SaltBytes).ToArray(); offset += SaltBytes;
        var nonce = file.Slice(offset, NonceBytes).ToArray(); offset += NonceBytes;
        var tag = file.Slice(offset, TagBytes); offset += TagBytes;
        var cipher = file[offset..].ToArray();
        var plain = new byte[cipher.Length];
        using var aes = new AesGcm(DeriveKey(password, salt, iterations), TagBytes);
        try { aes.Decrypt(nonce, cipher, tag, plain, Magic); }
        catch (CryptographicException) { throw new InvalidDataException("密码不正确，或账户文件已损坏。"); }
        try
        {
            var envelope = JsonSerializer.Deserialize<Envelope>(plain) ?? throw new InvalidDataException("账户文件为空。");
            if (envelope.Version != Version || envelope.Profiles is null) throw new InvalidDataException("不支持的账户文件版本。");
            foreach (var profile in envelope.Profiles) AccountStore.Validate(profile);
            return envelope.Profiles;
        }
        finally { CryptographicOperations.ZeroMemory(plain); }
    }

    private static byte[] DeriveKey(string password, byte[] salt, int iterations = Iterations) =>
        Rfc2898DeriveBytes.Pbkdf2(Encoding.UTF8.GetBytes(password), salt, iterations, HashAlgorithmName.SHA256, KeyBytes);

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
