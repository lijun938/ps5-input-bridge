using System.Windows.Controls;
using Ps5Bridge.Core;

namespace Ps5Bridge.App;

public partial class MainWindow
{
    private readonly PreferencesStore _preferencesStore;
    private UserPreferences _preferences = new();
    private Task _preferencesSave = Task.CompletedTask;

    private BridgeOptions SelectedOptions()
    {
        var controller = ControllerCombo.SelectedItem as ControllerChoice;
        var interval = PollIntervalCombo.SelectedItem is ComboBoxItem item && int.TryParse(item.Tag?.ToString(), out var value) ? value : 500;
        return new(controller?.Index ?? 0, interval) { DeviceId = controller?.DeviceId };
    }

    private void RestoreInputPreferences()
    {
        // Restore the choice before asynchronous discovery. A blocked driver must
        // neither delay startup nor temporarily replace a saved HID identity.
        var controllers = Enumerable.Range(0, NativeBridgeBackend.XInputSlots)
            .Select(index => new ControllerChoice(index, $"XInput {index + 1} · 正在检查连接")).ToList();
        var selected = _preferences.DeviceId is not null
            ? new ControllerChoice(_preferences.ControllerIndex, "所选 HID 手柄 · 等待设备列表", _preferences.DeviceId)
            : controllers.FirstOrDefault(controller => controller.Index == _preferences.ControllerIndex) ?? controllers[0];
        if (selected.DeviceId is not null) controllers.Add(selected);
        ControllerCombo.ItemsSource = controllers;
        ControllerCombo.SelectedItem = selected;
        PollIntervalCombo.SelectedItem = PollIntervalCombo.Items.Cast<ComboBoxItem>()
            .First(item => item.Tag?.ToString() == _preferences.PollIntervalMicroseconds.ToString());
        BackMappingCombo.SelectedIndex = (int)_preferences.BackMapping;
        _backend.BackMapping = _preferences.BackMapping;
    }

    private Task SavePreferencesAsync()
    {
        if (!_loaded || _closing || _refreshingControllers) return Task.CompletedTask;
        var options = SelectedOptions();
        _preferences = new(AccountId: (AccountCombo.SelectedItem as AccountProfile)?.Id, ControllerIndex: options.ControllerIndex,
            DeviceId: options.DeviceId, PollIntervalMicroseconds: options.PollIntervalMicroseconds, BackMapping: _backend.BackMapping);
        // Enqueue on the UI thread so a slow older write cannot overwrite newer choices.
        return _preferencesSave = WritePreferencesAsync(_preferencesSave, _preferences);
    }

    private async Task WritePreferencesAsync(Task previous, UserPreferences preferences)
    {
        try { await previous; await _preferencesStore.SaveAsync(preferences); }
        catch (Exception) { if (!_closing) ShowNotice("本次设置已生效，但无法保存使用偏好，请检查文件夹写入权限。"); }
    }

    private void UpdateInputOptionsState()
    {
        var connecting = _sessions.Status.State is BridgeState.Connecting or BridgeState.Stopping || _connectRequest is not null;
        ControllerCombo.IsEnabled = !_closing && !connecting;
        var hid = ControllerCombo.SelectedItem is ControllerChoice { Index: >= 4 };
        PollIntervalCombo.IsEnabled = !_closing && !connecting && !hid;
        PollIntervalHint.Text = hid ? "HID 按报告到达处理，采样档不适用。已保留你的 XInput 档位。"
            : "更改采样档会立即生效，无需重连 PS5。100 μs 档更耗 CPU。";
    }
}
