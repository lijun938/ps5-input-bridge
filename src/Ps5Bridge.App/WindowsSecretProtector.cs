using System.Runtime.InteropServices;
using System.Security.Cryptography;
using Ps5Bridge.Core;

namespace Ps5Bridge.App;

public sealed class WindowsSecretProtector : ISecretProtector
{
    public byte[] Protect(byte[] plainText) => Transform(plainText, true);
    public byte[] Unprotect(byte[] encrypted) => Transform(encrypted, false);

    private static byte[] Transform(byte[] input, bool protect)
    {
        ArgumentNullException.ThrowIfNull(input);
        if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException("账户加密需要 Windows DPAPI。");
        var source = new DataBlob { Length = input.Length, Data = Marshal.AllocHGlobal(Math.Max(1, input.Length)) };
        var destination = new DataBlob();
        try
        {
            Marshal.Copy(input, 0, source.Data, input.Length);
            var success = protect
                ? CryptProtectData(ref source, null, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, 1, out destination)
                : CryptUnprotectData(ref source, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, 1, out destination);
            if (!success) throw new CryptographicException("Windows 无法处理账户密文：" +
                new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error()).Message);
            var result = new byte[destination.Length];
            Marshal.Copy(destination.Data, result, 0, result.Length);
            return result;
        }
        finally
        {
            ClearAndFree(source, false);
            ClearAndFree(destination, true);
        }
    }

    private static void ClearAndFree(DataBlob blob, bool localHeap)
    {
        if (blob.Data == IntPtr.Zero) return;
        Marshal.Copy(new byte[blob.Length], 0, blob.Data, blob.Length);
        if (localHeap) _ = LocalFree(blob.Data);
        else Marshal.FreeHGlobal(blob.Data);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DataBlob { public int Length; public IntPtr Data; }

    [DllImport("crypt32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CryptProtectData(ref DataBlob source, string? description, IntPtr entropy,
        IntPtr reserved, IntPtr prompt, uint flags, out DataBlob result);
    [DllImport("crypt32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CryptUnprotectData(ref DataBlob source, IntPtr description, IntPtr entropy,
        IntPtr reserved, IntPtr prompt, uint flags, out DataBlob result);
    [DllImport("kernel32.dll")]
    private static extern IntPtr LocalFree(IntPtr memory);
}
