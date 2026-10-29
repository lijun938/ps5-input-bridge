using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;

internal static class Program
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int Run(ulong slots,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string profile,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string raw,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string result, int uiMode);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int Poll(out uint state, out ulong reports, out double mean, out double p99);

    [STAThread]
    public static int Main(string[] args)
    {
        if (args.Length != 5 || !ulong.TryParse(args[1], out var slots)) return 2;
        var handle = NativeLibrary.Load(Path.GetFullPath(args[0]));
        var run = Marshal.GetDelegateForFunctionPointer<Run>(NativeLibrary.GetExport(handle, "bench_run"));
        var poll = Marshal.GetDelegateForFunctionPointer<Poll>(NativeLibrary.GetExport(handle, "bench_poll_ui"));
        var text = new TextBlock { Margin = new Thickness(16), Text = "Starting controlled input benchmark…" };
        var window = new Window
        {
            Title = "Input latency benchmark", Content = text,
            Width = 460, Height = 140, ShowActivated = false, ShowInTaskbar = false
        };
        var app = new Application { ShutdownMode = ShutdownMode.OnExplicitShutdown };
        var timer = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromMilliseconds(500) };
        var ticks = new List<double>();
        var stopwatch = Stopwatch.StartNew();
        var lastTick = 0.0;
        var successfulReads = 0;
        var exitCode = 8;
        timer.Tick += (_, _) =>
        {
            var now = stopwatch.Elapsed.TotalMilliseconds;
            if (lastTick != 0) ticks.Add(now - lastTick);
            lastTick = now;
            if (poll(out var state, out var reports, out var mean, out var p99) == 0) return;
            successfulReads++;
            text.Text = $"State: {state}; reports: {reports:N0}\nLocal submit mean: {mean:F3} μs; P99: {p99:F3} μs";
        };
        window.Loaded += async (_, _) =>
        {
            timer.Start();
            string? error = null;
            try { exitCode = await Task.Run(() => run(slots, args[2], args[3], args[4], 3)); }
            catch (Exception exception) { error = exception.ToString(); exitCode = 10; }
            finally
            {
                timer.Stop();
                File.WriteAllText(Path.ChangeExtension(args[4], ".ui.json"), JsonSerializer.Serialize(new
                {
                    kind = "small WPF statistics panel, not the production application",
                    dispatcherPriority = "Background", requestedIntervalMs = 500,
                    successfulReads, tickIntervalsMs = ticks, exitCode, error
                }, new JsonSerializerOptions { WriteIndented = true }));
                window.Close();
                app.Shutdown(exitCode);
            }
        };
        app.Run(window);
        // A failed native setup may have left a test worker alive. Let process
        // teardown reclaim that DLL instead of unmapping executing code.
        if (exitCode == 0) NativeLibrary.Free(handle);
        return exitCode;
    }
}
