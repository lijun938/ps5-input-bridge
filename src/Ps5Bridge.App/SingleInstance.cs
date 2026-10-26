using System.Security.Principal;

namespace Ps5Bridge.App;

internal sealed class SingleInstance : IDisposable
{
    private readonly Mutex mutex;
    public bool Acquired { get; }
    public SingleInstance(string? name = null)
    {
        name ??= @"Global\PS5InputBridge-" + WindowsIdentity.GetCurrent().User!.Value;
        mutex = new Mutex(false, name);
        try { Acquired = mutex.WaitOne(0); }
        catch (AbandonedMutexException) { Acquired = true; }
    }
    public void Dispose()
    {
        if (Acquired) mutex.ReleaseMutex();
        mutex.Dispose();
    }
}
