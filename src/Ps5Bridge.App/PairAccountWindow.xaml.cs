using System.ComponentModel;
using System.Diagnostics;
using System.Windows;
using System.Windows.Media;
using System.Windows.Navigation;
using Ps5Bridge.Core;

namespace Ps5Bridge.App;

public partial class PairAccountWindow : Window
{
    private readonly IBridgeBackend _backend;
    private readonly CancellationTokenSource _cancellation;
    private bool _pairing;
    private bool _cancelAndClose;
    private bool _finished;

    public AccountProfile? Profile { get; private set; }

    public PairAccountWindow(IBridgeBackend backend, CancellationToken lifetime)
    {
        InitializeComponent();
        _backend = backend;
        _cancellation = CancellationTokenSource.CreateLinkedTokenSource(lifetime);
        Loaded += (_, _) => NameInput.Focus();
        Closed += (_, _) => _cancellation.Dispose();
    }

    private async void Pair_Click(object sender, RoutedEventArgs e)
    {
        if (_pairing) return;
        var name = NameInput.Text.Trim();
        var host = HostInput.Text.Trim();
        var accountId = AccountIdInput.Text.Trim();
        var pin = PinInput.Text.Trim();
        if (string.IsNullOrWhiteSpace(name)) { ShowError("请填写一个账户名称。"); NameInput.Focus(); return; }
        if (string.IsNullOrWhiteSpace(host)) { ShowError("请填写 PS5 的局域网 IP 或主机名。"); HostInput.Focus(); return; }
        if (!IsValidAccountId(accountId)) { ShowError("Account-ID 必须是 8 字节账户 ID 的 base64 编码。"); AccountIdInput.Focus(); return; }
        if (pin.Length != 8 || pin.Any(character => character is < '0' or > '9'))
        { ShowError("请输入 PS5 上显示的 8 位数字配对码。"); PinInput.Focus(); return; }

        _pairing = true;
        Fields.IsEnabled = false;
        PairButton.IsEnabled = false;
        ProgressText.Foreground = (Brush)FindResource("MutedBrush");
        ProgressText.Text = "正在与 PS5 配对…";
        ProgressText.Visibility = Visibility.Visible;
        try
        {
            var keys = await Task.Run(() => _backend.PairAsync(host, accountId, pin, _cancellation.Token));
            _cancellation.Token.ThrowIfCancellationRequested();
            var profile = new AccountProfile(Guid.NewGuid(), name, host, accountId, keys);
            AccountStore.Validate(profile);
            Profile = profile;
            _finished = true;
            DialogResult = true;
        }
        catch (OperationCanceledException) when (_cancellation.IsCancellationRequested)
        {
            _finished = true;
            DialogResult = false;
        }
        catch (Exception ex)
        {
            if (_cancelAndClose)
            {
                _finished = true;
                DialogResult = false;
            }
            else ShowError($"配对失败：{ex.Message}");
        }
        finally
        {
            _pairing = false;
            if (!_finished)
            {
                Fields.IsEnabled = true;
                PairButton.IsEnabled = true;
            }
        }
    }

    private static bool IsValidAccountId(string value)
    {
        try { return Convert.FromBase64String(value).Length == 8; }
        catch (FormatException) { return false; }
    }

    private void AccountIdHelp_RequestNavigate(object sender, RequestNavigateEventArgs e)
    {
        e.Handled = true;
        try { Process.Start(new ProcessStartInfo(e.Uri.AbsoluteUri) { UseShellExecute = true }); }
        catch (Exception ex) { ShowError($"无法打开浏览器：{ex.Message}"); }
    }

    private void ShowError(string text)
    {
        ProgressText.Text = text;
        ProgressText.Foreground = new SolidColorBrush(Color.FromRgb(160, 61, 37));
        ProgressText.Visibility = Visibility.Visible;
    }

    private void Cancel_Click(object sender, RoutedEventArgs e)
    {
        if (_pairing) CancelPairing();
        else DialogResult = false;
    }

    private void Window_Closing(object? sender, CancelEventArgs e)
    {
        if (_finished) return;
        if (_pairing)
        {
            e.Cancel = true;
            CancelPairing();
        }
    }

    private void CancelPairing()
    {
        _cancelAndClose = true;
        _cancellation.Cancel();
        CancelButton.IsEnabled = false;
        ProgressText.Text = "正在取消配对…";
        ProgressText.Visibility = Visibility.Visible;
    }
}
