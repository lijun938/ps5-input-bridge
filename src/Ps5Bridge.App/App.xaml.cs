using System.Windows;

namespace Ps5Bridge.App;

public partial class App : Application
{
    private SingleInstance? instance;
    protected override void OnStartup(StartupEventArgs e)
    {
        instance = new SingleInstance();
        if (!instance.Acquired)
        {
            MessageBox.Show("PS5 Input Bridge 已经在运行，请使用已打开的窗口。", "PS5 Input Bridge", MessageBoxButton.OK, MessageBoxImage.Information);
            Shutdown();
            return;
        }
        base.OnStartup(e);
    }

    protected override void OnExit(ExitEventArgs e)
    {
        instance?.Dispose();
        base.OnExit(e);
    }
}
