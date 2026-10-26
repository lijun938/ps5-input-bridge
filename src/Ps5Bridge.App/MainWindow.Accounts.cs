using System.Windows;
using Ps5Bridge.Core;

namespace Ps5Bridge.App;

public partial class MainWindow
{
    private bool _changingSelection;
    private Guid? _sessionAccountId;

    private async void EditAccount_Click(object sender, RoutedEventArgs e)
    {
        if (AccountCombo.SelectedItem is not AccountProfile original || !TryBeginAccountChange()) return;
        try
        {
            var dialog = new EditAccountWindow(original) { Owner = this };
            if (dialog.ShowDialog() != true || dialog.Profile is not { } updated) return;
            await SaveEditedAccountAsync(updated);
            ShowNotice(original.Host == updated.Host ? "账户名称已保存，连接地址和配对资料保持不变。" : "地址已保存，请点击连接。无需重新配对。");
        }
        catch (OperationCanceledException) when (_closing) { }
        catch (Exception ex) { ShowNotice($"账户未修改：{ex.Message}"); }
        finally { EndAccountChange(); }
    }

    private async Task SaveEditedAccountAsync(AccountProfile updated)
    {
        var original = _accounts.Single(p => p.Id == updated.Id);
        var replacement = AccountEditing.Replace(_accounts, updated);
        if (_sessionAccountId == updated.Id && original.Host != updated.Host)
            await StopForAccountChangeAsync();
        await _store.SaveAsync(replacement, _lifetime.Token);
        _changingSelection = true;
        try
        {
            var selected = (AccountCombo.SelectedItem as AccountProfile)?.Id;
            var index = _accounts.IndexOf(original);
            _accounts[index] = replacement[index];
            AccountCombo.SelectedItem = _accounts.FirstOrDefault(p => p.Id == selected);
        }
        finally { _changingSelection = false; }
        UpdateAccountHint();
        UpdateButtons();
        await SavePreferencesAsync();
    }

    private async void DeleteAccount_Click(object sender, RoutedEventArgs e)
    {
        if (AccountCombo.SelectedItem is not AccountProfile profile || !TryBeginAccountChange()) return;
        try
        {
            if (MessageBox.Show(this, $"删除“{profile.Name}”（{profile.Host}）？\n\n只删除本程序保存的账户，Chiaki 和 PS5 上的资料不受影响。之后可以重新导入。",
                    "删除账户", MessageBoxButton.YesNo, MessageBoxImage.Question, MessageBoxResult.No) != MessageBoxResult.Yes) return;
            await DeleteAccountAsync(profile.Id);
            ShowNotice("账户已删除，可以重新导入。其他账户不会自动连接。");
        }
        catch (OperationCanceledException) when (_closing) { }
        catch (Exception ex) { ShowNotice($"账户未删除：{ex.Message}"); }
        finally { EndAccountChange(); }
    }

    private async Task DeleteAccountAsync(Guid id)
    {
        var remaining = AccountEditing.Remove(_accounts, id);
        if (_sessionAccountId == id) await StopForAccountChangeAsync();
        await _store.SaveAsync(remaining, _lifetime.Token);
        _changingSelection = true;
        try
        {
            var selected = (AccountCombo.SelectedItem as AccountProfile)?.Id;
            var profile = _accounts.FirstOrDefault(p => p.Id == id);
            if (profile is not null) _accounts.Remove(profile);
            AccountCombo.SelectedItem = _accounts.FirstOrDefault(p => p.Id == selected) ?? _accounts.FirstOrDefault();
        }
        finally { _changingSelection = false; }
        UpdateAccountHint();
        UpdateButtons();
        await SavePreferencesAsync();
    }

    private async Task StopForAccountChangeAsync()
    {
        if (_connectRequest is { } request) _ = request.CancelAsync();
        await _sessions.StopAsync(_lifetime.Token);
        _sessionAccountId = null;
    }
}
