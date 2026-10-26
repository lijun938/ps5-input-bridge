using System.Collections.ObjectModel;
using System.IO;
using System.Windows;
using Ps5Bridge.Core;

namespace Ps5Bridge.App;

public partial class ChiakiImportWindow : Window
{
    private readonly AccountProfile[] _existing;
    private readonly CancellationToken _lifetime;
    private readonly CancellationTokenRegistration _closingRegistration;
    private readonly ObservableCollection<ImportEntry> _entries = [];
    private bool _reading;

    public IReadOnlyList<AccountProfile> Profiles { get; private set; } = [];
    public int SkippedCount { get; private set; }

    public ChiakiImportWindow(IEnumerable<AccountProfile> existing, CancellationToken lifetime)
    {
        InitializeComponent();
        _existing = existing.ToArray();
        _lifetime = lifetime;
        ProfilesList.ItemsSource = _entries;
        _closingRegistration = lifetime.Register(() => Dispatcher.InvokeAsync(Close));
        Closed += (_, _) => _closingRegistration.Dispose();
    }

    private void Window_Loaded(object sender, RoutedEventArgs e) => ReadLocal();
    private void ReadLocal_Click(object sender, RoutedEventArgs e) => ReadLocal();

    private void ReadLocal()
    {
        if (_reading || _lifetime.IsCancellationRequested) return;
        ClearEntries();
        try
        {
            var result = ChiakiSettingsReader.ReadLocal(_existing);
            if (result is null)
                SourceText.Text = "没有找到本机 Chiaki 的配对记录。可以选择从 Chiaki / Chiaki-ng 导出的 INI 文件或原有的 CONF 配置文件。";
            else ApplyResult(result, "本机 Chiaki / Chiaki-ng 配置");
        }
        catch (Exception ex)
        {
            SourceText.Text = "无法读取本机配置，可以改为选择导出的 INI / CONF 文件。";
            ShowError(ex.Message);
        }
    }

    private async void ReadFile_Click(object sender, RoutedEventArgs e)
    {
        if (_reading || _lifetime.IsCancellationRequested) return;
        var open = new Microsoft.Win32.OpenFileDialog
        {
            Title = "选择 Chiaki / Chiaki-ng 配置文件",
            Filter = "Chiaki 配置 (*.ini;*.conf)|*.ini;*.conf|所有文件 (*.*)|*.*"
        };
        if (open.ShowDialog(this) != true) return;
        ClearEntries();
        SetReading(true);
        byte[] plain = [];
        try
        {
            SourceText.Text = "正在读取配置文件…";
            if (new FileInfo(open.FileName).Length > ChiakiImporter.MaximumSettingsBytes)
                throw new FormatException("Chiaki 配置文件过大（上限 4 MB）。");
            plain = await File.ReadAllBytesAsync(open.FileName, _lifetime);
            _lifetime.ThrowIfCancellationRequested();
            ApplyResult(ChiakiImporter.Parse(plain, _existing), Path.GetFileName(open.FileName));
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested) { }
        catch (Exception ex)
        {
            SourceText.Text = "配置文件未导入。请确认它包含桌面版 Chiaki / Chiaki-ng 的 PS5 配对记录。";
            ShowError(ex.Message);
        }
        finally
        {
            Array.Clear(plain);
            SetReading(false);
        }
    }

    private void ApplyResult(ChiakiImporter.Result result, string source)
    {
        foreach (var profile in result.Imported) _entries.Add(new ImportEntry(profile.Name, profile.Keys, profile.Host));
        foreach (var request in result.NeedsHost) _entries.Add(new ImportEntry(request.Name, request.Keys, ""));
        SkippedCount = result.SkippedExisting + result.SkippedNotPs5 + result.SkippedInvalid;
        var skipped = new List<string>();
        if (result.SkippedExisting > 0) skipped.Add($"{result.SkippedExisting} 个已存在");
        if (result.SkippedNotPs5 > 0) skipped.Add($"{result.SkippedNotPs5} 个不是 PS5");
        if (result.SkippedInvalid > 0) skipped.Add($"{result.SkippedInvalid} 个配对数据不完整");
        SourceText.Text = $"{source}：可导入 {_entries.Count} 个账户" +
            (result.NeedsHost.Count > 0 ? $"，其中 {result.NeedsHost.Count} 个需要补填主机地址" : "") +
            (skipped.Count > 0 ? "；跳过 " + string.Join("、", skipped) : "") + "。";
        if (_entries.Count == 0 && SkippedCount == 0) SourceText.Text += "可以改选其他配置文件。";
        ImportButton.IsEnabled = !_reading && _entries.Count > 0;
    }

    private void Import_Click(object sender, RoutedEventArgs e)
    {
        if (_reading || _entries.Count == 0 || _lifetime.IsCancellationRequested) return;
        var profiles = new List<AccountProfile>();
        foreach (var entry in _entries)
        {
            try { profiles.Add(new ChiakiImporter.HostRequest(entry.Name.Trim(), entry.Keys).Complete(entry.Host)); }
            catch (ArgumentException ex)
            {
                ShowError($"第 {profiles.Count + 1} 个账户：{ex.Message}");
                return;
            }
        }
        Profiles = profiles;
        DialogResult = true;
    }

    private void ClearEntries()
    {
        _entries.Clear();
        Profiles = [];
        SkippedCount = 0;
        ImportButton.IsEnabled = false;
        ErrorText.Visibility = Visibility.Collapsed;
    }

    private void SetReading(bool reading)
    {
        _reading = reading;
        ReadLocalButton.IsEnabled = ReadFileButton.IsEnabled = !reading;
        ImportButton.IsEnabled = !reading && _entries.Count > 0;
    }

    private void ShowError(string message)
    {
        ErrorText.Text = message;
        ErrorText.Visibility = Visibility.Visible;
    }

    private sealed class ImportEntry(string name, PairingKeys keys, string host)
    {
        public string Name { get; set; } = name;
        public PairingKeys Keys { get; } = keys;
        public string Host { get; set; } = host;
    }
}
