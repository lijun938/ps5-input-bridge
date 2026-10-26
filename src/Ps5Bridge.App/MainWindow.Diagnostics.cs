using System.IO;
using System.Windows;
using Ps5Bridge.Core;

namespace Ps5Bridge.App;

public partial class MainWindow
{
    private readonly DiagnosticLog _diagnostics = new();

    private async void ExportDiagnostics_Click(object sender, RoutedEventArgs e)
    {
        if (_closing) return;
        var save = new Microsoft.Win32.SaveFileDialog
        {
            Title = "导出诊断（不含账户、IP、配对密钥）", FileName = "ps5-bridge-diagnostics.json",
            Filter = "诊断文件 (*.json)|*.json", DefaultExt = ".json"
        };
        if (save.ShowDialog(this) != true) return;
        try
        {
            var snapshot = _sessions.GetSnapshot();
            _diagnostics.Observe(snapshot);
            var input = _backend.GetInputDebug();
            var json = _diagnostics.Export(snapshot, SelectedOptions(), _backend.BackMapping,
                typeof(MainWindow).Assembly.GetName().Version?.ToString() ?? "unknown",
                new(input.ObservedChanges, input.SubmitFailures, input.L1Presses, input.R1Presses, input.Chords));
            await File.WriteAllTextAsync(save.FileName, json, _lifetime.Token);
            ShowNotice("诊断已导出：包含近期连接状态和输入统计，不含账户名称、IP、设备路径或配对密钥。");
        }
        catch (OperationCanceledException) when (_closing) { }
        catch (Exception ex) { ShowNotice($"诊断导出失败：{ex.Message}"); }
    }
}
