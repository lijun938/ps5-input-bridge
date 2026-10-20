using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
using Ps5Bridge.Core;

namespace Ps5Bridge.App;

public partial class MainWindow : Window
{
    private readonly ObservableCollection<AccountProfile> _accounts = [];
    private readonly NativeBridgeBackend _backend;
    private readonly SessionCoordinator _sessions;
    private readonly AccountStore _store;
    private readonly CancellationTokenSource _lifetime = new();
    private readonly DispatcherTimer _statusTimer;
    private CancellationTokenSource? _connectRequest;
    private bool _loaded;
    private bool _closing;
    private bool _closed;
    private bool _storeAvailable = true;
    private bool _refreshingControllers;
    private int _statusTicks;

    public MainWindow()
    {
        InitializeComponent();
        _backend = new NativeBridgeBackend();
        _backend.LoginPinProvider = RequestLoginPinAsync;
        _sessions = new SessionCoordinator(_backend);
        _sessions.StatusChanged += Session_StatusChanged;
        var accountPath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "PS5InputBridge", "accounts.dat");
        _store = new AccountStore(accountPath, new WindowsSecretProtector());
        AccountCombo.ItemsSource = _accounts;
        _statusTimer = new DispatcherTimer(DispatcherPriority.Background)
        {
            Interval = TimeSpan.FromMilliseconds(500)
        };
        _statusTimer.Tick += StatusTimer_Tick;
    }

    private async void Window_Loaded(object sender, RoutedEventArgs e)
    {
        try
        {
            foreach (var profile in await _store.LoadAsync(_lifetime.Token))
                _accounts.Add(profile);
            if (_accounts.Count > 0)
                AccountCombo.SelectedIndex = 0;
        }
        catch (OperationCanceledException) when (_closing) { return; }
        catch (Exception ex)
        {
            _storeAvailable = false;
            AddAccountButton.IsEnabled = false;
            ImportHeliosButton.IsEnabled = false;
            ImportBundleButton.IsEnabled = false;
            ExportBundleButton.IsEnabled = false;
            ShowNotice($"无法读取账户文件，已暂停保存以保护现有凭据。{ex.Message}");
        }
        if (_closing) return;
        RefreshControllers();
        _loaded = true;
        UpdateAccountHint();
        UpdateButtons();
        _statusTimer.Start();
    }

    private async void AddAccount_Click(object sender, RoutedEventArgs e)
    {
        if (_closing || !_storeAvailable) return;
        AddAccountButton.IsEnabled = false;
        try
        {
            var dialog = new PairAccountWindow(_backend, _lifetime.Token) { Owner = this };
            if (dialog.ShowDialog() != true || dialog.Profile is not { } profile) return;
            var profiles = _accounts.Append(profile).ToArray();
            await _store.SaveAsync(profiles, _lifetime.Token);
            _accounts.Add(profile);
            // Adding a profile does not interrupt an active session.
            if (AccountCombo.SelectedItem is null)
                AccountCombo.SelectedItem = profile;
            ClearNotice();
            UpdateAccountHint();
        }
        catch (OperationCanceledException) when (_closing) { }
        catch (Exception ex) { ShowNotice($"账户未保存：{ex.Message}"); }
        finally { if (!_closing) AddAccountButton.IsEnabled = _storeAvailable; }
    }

    private async void ImportHelios_Click(object sender, RoutedEventArgs e)
    {
        if (_closing || !_storeAvailable) return;
        ImportHeliosButton.IsEnabled = false;
        try
        {
            var path = HeliosImporter.DefaultCredentialsPath;
            if (!File.Exists(path)) { ShowNotice("没有找到 Helios 的远程游玩账户文件。请确认在这台电脑、这个 Windows 用户下用过 Helios 的 PS Remote Play。"); return; }
            byte[] plain = [];
            HeliosImporter.Result result;
            try
            {
                plain = new WindowsSecretProtector().Unprotect(await File.ReadAllBytesAsync(path, _lifetime.Token));
                result = HeliosImporter.Parse(plain, _accounts);
            }
            finally { Array.Clear(plain); }
            if (result.Imported.Count > 0)
            {
                await _store.SaveAsync(_accounts.Concat(result.Imported).ToArray(), _lifetime.Token);
                foreach (var profile in result.Imported) _accounts.Add(profile);
                AccountCombo.SelectedItem ??= result.Imported[0];
                UpdateAccountHint();
            }
            var skipped = new List<string>();
            if (result.SkippedExisting > 0) skipped.Add($"{result.SkippedExisting} 个已存在");
            if (result.SkippedNotPs5 > 0) skipped.Add($"{result.SkippedNotPs5} 个不是 PS5");
            if (result.SkippedInvalid > 0)
                skipped.Add($"{result.SkippedInvalid} 个数据不完整（无法使用的字段：{string.Join("；", result.InvalidFields)}）");
            ShowNotice($"已从 Helios 导入 {result.Imported.Count} 个账户" +
                (skipped.Count > 0 ? "，跳过 " + string.Join("、", skipped) : "") + "。");
        }
        catch (OperationCanceledException) when (_closing) { }
        catch (Exception ex) { ShowNotice($"Helios 账户导入失败：{ex.Message}"); }
        finally { if (!_closing) ImportHeliosButton.IsEnabled = _storeAvailable; }
    }

    private async void ExportBundle_Click(object sender, RoutedEventArgs e)
    {
        if (_closing || !_storeAvailable) return;
        if (_accounts.Count == 0) { ShowNotice("还没有可导出的账户。"); return; }
        var dialog = new BundlePasswordWindow(exporting: true) { Owner = this };
        if (dialog.ShowDialog() != true || dialog.Password is not { } password) return;
        var save = new Microsoft.Win32.SaveFileDialog
        {
            Title = "导出账户文件",
            FileName = "ps5-accounts.bridge",
            Filter = "PS5 账户文件 (*.bridge)|*.bridge|所有文件 (*.*)|*.*",
            DefaultExt = ".bridge"
        };
        if (save.ShowDialog(this) != true) return;
        try
        {
            var bundle = AccountPortableBundle.Export(_accounts, password);
            await File.WriteAllBytesAsync(save.FileName, bundle, _lifetime.Token);
            ShowNotice($"已导出 {_accounts.Count} 个账户到 {save.FileName}。文件包含配对凭据，请妥善保管。");
        }
        catch (Exception ex) { ShowNotice($"导出失败：{ex.Message}"); }
    }

    private async void ImportBundle_Click(object sender, RoutedEventArgs e)
    {
        if (_closing || !_storeAvailable) return;
        var open = new Microsoft.Win32.OpenFileDialog
        {
            Title = "导入账户文件",
            Filter = "PS5 账户文件 (*.bridge)|*.bridge|所有文件 (*.*)|*.*"
        };
        if (open.ShowDialog(this) != true) return;
        var dialog = new BundlePasswordWindow(exporting: false) { Owner = this };
        if (dialog.ShowDialog() != true || dialog.Password is not { } password) return;
        try
        {
            var imported = AccountPortableBundle.Import(await File.ReadAllBytesAsync(open.FileName, _lifetime.Token), password);
            var known = _accounts.Select(profile => profile.Keys.RegistrationKeyHex).ToHashSet(StringComparer.OrdinalIgnoreCase);
            var added = imported.Where(profile => known.Add(profile.Keys.RegistrationKeyHex)).ToArray();
            if (added.Length > 0)
            {
                await _store.SaveAsync(_accounts.Concat(added).ToArray(), _lifetime.Token);
                foreach (var profile in added) _accounts.Add(profile);
                AccountCombo.SelectedItem ??= added[0];
                UpdateAccountHint();
            }
            ShowNotice($"已导入 {added.Length} 个账户，跳过 {imported.Count - added.Length} 个已存在的账户。");
        }
        catch (Exception ex) { ShowNotice($"导入失败：{ex.Message}"); }
    }

    private async void Connect_Click(object sender, RoutedEventArgs e) => await ConnectSelectedAsync();

    private async void Account_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_loaded || _closing) return;
        UpdateButtons();
        if (IsSessionActive || _connectRequest is not null)
            await ConnectSelectedAsync();
    }

    private async void Options_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!_loaded || _closing || _refreshingControllers) return;
        if (IsSessionActive || _connectRequest is not null)
            await ConnectSelectedAsync();
    }

    private void BackMapping_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_backend is null || BackMappingCombo.SelectedItem is not ComboBoxItem { Tag: string tag }) return;
        try { _backend.BackMapping = uint.Parse(tag); }
        catch (Exception ex) { ShowNotice($"Back 键映射未生效：{ex.Message}"); }
    }

    private bool IsSessionActive => _sessions.Status.State is
        BridgeState.Connecting or BridgeState.Connected or BridgeState.Stopping;

    private async Task ConnectSelectedAsync()
    {
        if (_closing || AccountCombo.SelectedItem is not AccountProfile profile) return;
        var controllerIndex = (ControllerCombo.SelectedItem as ControllerChoice)?.Index ?? 0;
        var interval = PollIntervalCombo.SelectedItem is ComboBoxItem item &&
            int.TryParse(item.Tag?.ToString(), out var selectedInterval) ? selectedInterval : 500;
        var options = new BridgeOptions(controllerIndex, interval);
        var previousRequest = _connectRequest;
        var request = CancellationTokenSource.CreateLinkedTokenSource(_lifetime.Token);
        _connectRequest = request;
        ClearNotice();
        try
        {
            // Cancel native callbacks away from WPF, then issue lifecycle requests in UI order.
            if (previousRequest is not null) _ = previousRequest.CancelAsync();
            request.Token.ThrowIfCancellationRequested();
            await _sessions.ConnectAsync(profile, options, request.Token);
        }
        catch (OperationCanceledException) when (request.IsCancellationRequested) { }
        catch (Exception ex)
        {
            if (ReferenceEquals(_connectRequest, request) && !_closing)
                ShowNotice(ex.Message);
        }
        finally
        {
            if (ReferenceEquals(_connectRequest, request)) _connectRequest = null;
            request.Dispose();
            if (!_closing) UpdateButtons();
        }
    }

    private async void Disconnect_Click(object sender, RoutedEventArgs e) => await DisconnectAsync();

    private async void PsButton_Click(object sender, RoutedEventArgs e) => await SendSpecialButtonAsync(1);

    private async void TouchpadButton_Click(object sender, RoutedEventArgs e) => await SendSpecialButtonAsync(2);

    private async Task SendSpecialButtonAsync(uint flags)
    {
        if (_closing || _sessions.Status.State != BridgeState.Connected) return;
        try { await Task.Run(() => _backend.SendSpecialButtonAsync(flags, _lifetime.Token)); }
        catch (OperationCanceledException) when (_closing) { }
        catch (Exception ex) { ShowNotice($"按键未发送：{ex.Message}"); }
    }

    private Task<string?> RequestLoginPinAsync(CancellationToken cancellationToken) =>
        Dispatcher.InvokeAsync(() =>
        {
            if (_closing || cancellationToken.IsCancellationRequested) return null;
            var dialog = new LoginPinWindow { Owner = this };
            using var registration = cancellationToken.Register(() =>
            {
                if (!Dispatcher.HasShutdownStarted)
                    _ = Dispatcher.BeginInvoke(() => dialog.Cancel());
            });
            return dialog.ShowDialog() == true ? dialog.Pin : null;
        }).Task;

    private async Task DisconnectAsync()
    {
        if (_closing) return;
        try
        {
            var request = _connectRequest;
            if (request is not null) _ = request.CancelAsync();
            await _sessions.StopAsync(_lifetime.Token);
        }
        catch (OperationCanceledException) when (_closing) { }
        catch (Exception ex) { ShowNotice($"断开失败：{ex.Message}"); }
        finally { if (!_closing) UpdateButtons(); }
    }

    private async void Window_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (_closing || Keyboard.Modifiers != ModifierKeys.Control) return;
        if (e.Key == Key.D)
        {
            e.Handled = true;
            await DisconnectAsync();
            return;
        }
        var digit = e.Key >= Key.D1 && e.Key <= Key.D9 ? e.Key - Key.D1 :
            e.Key >= Key.NumPad1 && e.Key <= Key.NumPad9 ? e.Key - Key.NumPad1 : -1;
        if (digit < 0 || digit >= _accounts.Count) return;
        e.Handled = true;
        var target = _accounts[digit];
        // Selection already connects when a session is active; avoid a second request.
        var willSwitch = (IsSessionActive || _connectRequest is not null) &&
            !ReferenceEquals(AccountCombo.SelectedItem, target);
        AccountCombo.SelectedItem = target;
        if (!willSwitch) await ConnectSelectedAsync();
    }

    private void Session_StatusChanged(SessionStatus status)
    {
        if (Dispatcher.HasShutdownStarted) return;
        _ = Dispatcher.BeginInvoke(() =>
        {
            if (_closing) return;
            ApplyStatus(status);
        });
    }

    private void ApplyStatus(SessionStatus status)
    {
        StatusText.Text = status.State switch
        {
            BridgeState.Connecting => "连接中",
            BridgeState.Connected => "已连接",
            BridgeState.Stopping => "正在断开",
            BridgeState.Failed => "连接失败",
            _ => "未连接"
        };
        StatusText.Foreground = status.State switch
        {
            BridgeState.Connected => new SolidColorBrush(Color.FromRgb(31, 122, 77)),
            BridgeState.Failed => new SolidColorBrush(Color.FromRgb(160, 61, 37)),
            _ => (Brush)FindResource("TextBrush")
        };
        StatusDetail.Text = status.State == BridgeState.Idle
            ? "选择账户后点击连接。"
            : string.Join(" · ", new[] { status.AccountName, status.Message }
                .Where(value => !string.IsNullOrWhiteSpace(value)));
        UpdateButtons();
    }

    private void StatusTimer_Tick(object? sender, EventArgs e)
    {
        if (_closing) return;
        try
        {
            var snapshot = _sessions.GetSnapshot();
            ApplyStatus(_sessions.Status);
            ControllerStatusText.Text = snapshot.ControllerConnected ? "手柄：已连接" : "手柄：未连接";
            MetricsText.Text = snapshot.InputReports == 0
                ? "PC 采集 → 提交：—"
                : $"PC 采集 → 提交：平均 {snapshot.MeanSubmitMicroseconds:F1} μs / 最大 {snapshot.MaxSubmitMicroseconds:F1} μs　·　{snapshot.InputReports:N0} 次";
            if (++_statusTicks % 4 == 0) RefreshControllers();
        }
        catch (Exception ex) { ShowNotice($"无法读取桥接状态：{ex.Message}"); }
    }

    private void RefreshControllers()
    {
        _refreshingControllers = true;
        try
        {
            var selected = (ControllerCombo.SelectedItem as ControllerChoice)?.Index ?? 0;
            var detected = _backend.GetControllers();
            var controllers = detected.Select(controller =>
                new ControllerChoice(controller.Index,
                    controller.Index >= NativeBridgeBackend.XInputSlots
                        ? controller.Name
                        : $"{controller.Name} · {(controller.Connected ? "已连接" : "未连接")}")).ToList();
            if (controllers.Count == 0)
                controllers = Enumerable.Range(0, 4)
                    .Select(index => new ControllerChoice(index, $"XInput {index + 1} · 未连接")).ToList();
            ControllerCombo.ItemsSource = controllers;
            // On first load prefer a pad that is actually present (an XInput slot or a HID pad);
            // afterwards keep the user's choice so a refresh never switches the live controller.
            var available = detected.Where(controller => controller.Connected).Select(controller => controller.Index).ToHashSet();
            var preferred = _loaded || available.Contains(selected) ? selected : available.DefaultIfEmpty(selected).First();
            ControllerCombo.SelectedItem = controllers.FirstOrDefault(controller => controller.Index == preferred)
                ?? controllers[0];
        }
        catch (Exception ex)
        {
            ControllerCombo.ItemsSource = Enumerable.Range(0, 4)
                .Select(index => new ControllerChoice(index, $"XInput {index + 1} · 未连接")).ToArray();
            ControllerCombo.SelectedIndex = 0;
            ShowNotice(ex.Message);
        }
        finally { _refreshingControllers = false; }
    }

    private void UpdateAccountHint() => AccountHint.Text = _accounts.Count == 0
        ? "先添加账户并完成一次配对。每个账户单独保存配对凭据。"
        : $"已保存 {_accounts.Count} 个账户。Ctrl + 1…9 按列表顺序快速连接。";

    private void UpdateButtons()
    {
        ConnectButton.IsEnabled = !_closing && AccountCombo.SelectedItem is AccountProfile;
        ConnectButton.Content = IsSessionActive ? "重新连接" : "连接";
        DisconnectButton.IsEnabled = !_closing && _sessions.Status.State != BridgeState.Idle;
        PsButton.IsEnabled = !_closing && _sessions.Status.State == BridgeState.Connected;
        TouchpadButton.IsEnabled = PsButton.IsEnabled;
    }

    private void ShowNotice(string text)
    {
        NoticeText.Text = text;
        NoticeText.Visibility = Visibility.Visible;
    }

    private void ClearNotice()
    {
        NoticeText.Text = "";
        NoticeText.Visibility = Visibility.Collapsed;
    }

    private async void Window_Closing(object? sender, CancelEventArgs e)
    {
        if (_closed) return;
        e.Cancel = true;
        if (_closing) return;
        _closing = true;
        _statusTimer.Stop();
        MainContent.IsEnabled = false;
        StatusText.Text = "正在关闭连接…";
        _sessions.StatusChanged -= Session_StatusChanged;
        try
        {
            await _lifetime.CancelAsync();
            await Task.Run(async () => await _sessions.DisposeAsync());
        }
        catch (Exception ex) { System.Diagnostics.Trace.TraceError($"Bridge shutdown: {ex.Message}"); }
        finally
        {
            _lifetime.Dispose();
            _closed = true;
            Close();
        }
    }

    private sealed record ControllerChoice(int Index, string DisplayName);
}
