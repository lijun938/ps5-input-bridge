using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Ps5Bridge.App;
using Ps5Bridge.Core;

internal static class Program
{
    private static readonly string Output = Path.GetFullPath("artifacts/chiaki-ui-validation");
    private static readonly PairingKeys KeyA = new("31323334353637380000000000000000", "000102030405060708090a0b0c0d0e0f", 1000100, "aabbccddeeff");
    private static readonly PairingKeys KeyB = new("41424344454647480000000000000000", "000102030405060708090a0b0c0d0e0f", 1000100, "112233445566");
    private static int checks;

    [STAThread]
    private static int Main()
    {
        var app = new App();
        app.InitializeComponent();
        SynchronizationContext.SetSynchronizationContext(new DispatcherSynchronizationContext(Dispatcher.CurrentDispatcher));
        app.ShutdownMode = ShutdownMode.OnExplicitShutdown;
        var bindingLog = new StringWriter();
        PresentationTraceSources.DataBindingSource.Switch.Level = SourceLevels.Warning;
        PresentationTraceSources.DataBindingSource.Listeners.Add(new TextWriterTraceListener(bindingLog));
        try
        {
            Directory.CreateDirectory(Output);
            CheckMainLayout();
            CheckLatencyLayout();
            CheckAccountMutationGuard();
            CheckIndependentImportNames();
            CheckEditDialog();
            CheckSavedAccountManagement();
            CheckRestoredPreferences();
            CheckControllerRefreshStaysResponsive();
            CheckSingleInstance();
            CheckPreviewAndConfirmation();
            CheckCancellation();
            CheckLifetimeCancellation();
            Assert(!bindingLog.ToString().Contains("Error", StringComparison.OrdinalIgnoreCase), "WPF binding errors: " + bindingLog);
            Console.WriteLine($"PASS: {checks} offline WPF checks; screenshots: {Output}");
            app.Shutdown();
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception);
            app.Shutdown(1);
            return 1;
        }
    }

    private static void CheckMainLayout()
    {
        var main = new MainWindow();
        // Never show the real main window: its Loaded handler would read the user's account file.
        ((Expander)main.FindName("AccountTools")).IsExpanded = true;
        var root = (FrameworkElement)main.Content;
        foreach (var width in new[] { 760, 620 })
        {
            root.Measure(new Size(width, 800));
            root.Arrange(new Rect(0, 0, width, 800));
            root.UpdateLayout();
            foreach (var name in new[] { "ImportBundleButton", "ExportBundleButton", "ImportHeliosButton", "ImportChiakiButton", "AddAccountButton", "EditAccountButton", "DeleteAccountButton" })
            {
                var button = (Button)main.FindName(name);
                var point = button.TranslatePoint(new Point(), root);
                Assert(button.ActualWidth > 0 && point.X >= 0 && point.X + button.ActualWidth <= width, $"{name} clipped at width {width}");
            }
            Render(root, $"main-{width}.png");
        }
    }

    private static void CheckLatencyLayout()
    {
        var main = new MainWindow();
        var root = (ScrollViewer)main.Content;
        var poll = (ComboBox)main.FindName("PollIntervalCombo");
        Assert(((ComboBoxItem)poll.SelectedItem).Tag.ToString() == "500", "Default sampling must remain 500 us");
        Assert(poll.Items.Cast<ComboBoxItem>().Select(item => item.Tag.ToString()).Order().SequenceEqual(new[] { "100", "1000", "500" }), "Keep all three sampling options, including 100 us");
        poll.SelectedIndex = 1;
        Assert(((ComboBoxItem)poll.SelectedItem).Tag.ToString() == "100", "100 us must remain selectable");
        var apply = typeof(MainWindow).GetMethod("ApplyMetrics", BindingFlags.Instance | BindingFlags.NonPublic)!;
        var metrics = (TextBlock)main.FindName("MetricsText");
        var latency = (TextBlock)main.FindName("LatencyText");
        var sampling = (TextBlock)main.FindName("SamplingText");
        apply.Invoke(main, [new BackendSnapshot(BridgeState.Connecting, false, 0, double.NaN, double.NaN)]);
        Assert(latency.Text.Contains('—') && sampling.Text.Contains('—'), "No samples must display dashes instead of zero latency");
        apply.Invoke(main, [new BackendSnapshot(BridgeState.Connected, true, 12345, double.NaN, double.NaN)
        {
            Latency = new(0, 0, 0, 0, 0, 0, 0, 0, 7)
        }]);
        Assert(metrics.Text.Contains('—') && metrics.Text.Contains($"{12345:N0} 次")
            && !metrics.Text.Contains("NaN") && !metrics.Text.Contains("μs"),
            "Reports without timing must retain the count and show a dash, never NaN or zero latency");
        Assert(latency.Text.Contains('—') && latency.Text.Contains("7 次") && sampling.Text.Contains('—'),
            "HID without timing must retain failure counts and show empty latency windows");
        apply.Invoke(main, [new BackendSnapshot(BridgeState.Connected, true, 123456789, 123.4, 123456.7)
        {
            Latency = new(1024, 1024, 234.5, 345.6, 501, 701, 1001, 123456, 12345)
        }]);
        Assert(latency.Text.Contains("P95") && latency.Text.Contains("P99") && latency.Text.Contains("提交失败"), "Latency distribution and failures must be visible");
        foreach (var width in new[] { 760, 620 })
        {
            root.Measure(new Size(width, 640));
            root.Arrange(new Rect(0, 0, width, 640));
            root.UpdateLayout();
            root.ScrollToBottom();
            root.UpdateLayout();
            foreach (var name in new[] { "MetricsText", "LatencyText", "SamplingText" })
            {
                var label = (TextBlock)main.FindName(name);
                var point = label.TranslatePoint(new Point(), root);
                Assert(label.TextWrapping == TextWrapping.Wrap && label.ActualHeight > 0
                    && point.X >= 0 && point.X + label.ActualWidth <= width
                    && point.Y >= 0 && point.Y + label.ActualHeight <= 640,
                    $"{name} must be fully visible after scrolling at width {width}");
            }
            Render(root, $"latency-{width}.png");
        }
    }

    private static ChiakiImportWindow NewDialog(CancellationToken lifetime = default)
    {
        var dialog = new ChiakiImportWindow([], lifetime);
        // Work exclusively with synthetic data, bypassing the production local-settings load.
        var loaded = typeof(ChiakiImportWindow).GetMethod("Window_Loaded", BindingFlags.Instance | BindingFlags.NonPublic)!;
        dialog.Loaded -= (RoutedEventHandler)loaded.CreateDelegate(typeof(RoutedEventHandler), dialog);
        dialog.WindowStartupLocation = WindowStartupLocation.Manual;
        dialog.Left = -10000;
        dialog.Top = -10000;
        dialog.ShowInTaskbar = false;
        dialog.ShowActivated = false;
        return dialog;
    }

    private static void CheckAccountMutationGuard()
    {
        var main = new MainWindow();
        var begin = typeof(MainWindow).GetMethod("TryBeginAccountChange", BindingFlags.Instance | BindingFlags.NonPublic);
        var end = typeof(MainWindow).GetMethod("EndAccountChange", BindingFlags.Instance | BindingFlags.NonPublic);
        Assert(begin is not null && end is not null, "All account mutations need a shared busy guard");
        var actions = (FrameworkElement)main.FindName("AccountActions");
        Assert(!(bool)begin!.Invoke(main, null)!, "Changes must wait until the initial account load completes");
        typeof(MainWindow).GetField("_loaded", BindingFlags.Instance | BindingFlags.NonPublic)!.SetValue(main, true);
        Assert((bool)begin.Invoke(main, null)!, "First mutation should acquire the guard");
        Assert(!actions.IsEnabled, "Account actions should be disabled while a mutation is pending");
        Assert(!(bool)begin.Invoke(main, null)!, "A second mutation must be rejected while the first is awaiting I/O");
        foreach (var handler in new[] { "AddAccount_Click", "ImportHelios_Click", "ImportBundle_Click", "ImportChiaki_Click" })
        {
            // A programmatically raised handler must also respect the guard; no file/modal is opened.
            typeof(MainWindow).GetMethod(handler, BindingFlags.Instance | BindingFlags.NonPublic)!
                .Invoke(main, [main, new RoutedEventArgs()]);
            Assert(!actions.IsEnabled, handler + " must not release another mutation's guard");
        }
        end!.Invoke(main, null);
        Assert(actions.IsEnabled, "Actions should recover after completion/cancellation");
        Assert((bool)begin.Invoke(main, null)!, "Later imports should still be possible");
        end.Invoke(main, null);
        typeof(MainWindow).GetField("_storeAvailable", BindingFlags.Instance | BindingFlags.NonPublic)!.SetValue(main, false);
        Assert(!(bool)begin.Invoke(main, null)!, "A failed account-store load must block changes");
    }

    private static void Populate(ChiakiImportWindow dialog)
    {
        var result = new ChiakiImporter.Result(
            [new ChiakiImporter.HostRequest("PS5 客厅（合成测试数据）", KeyA).Complete("192.168.1.50")],
            [new ChiakiImporter.HostRequest("PS5 书房（需要填写地址）", KeyB)], 1, 1, 1, ["rp_key"]);
        typeof(ChiakiImportWindow).GetMethod("ApplyResult", BindingFlags.Instance | BindingFlags.NonPublic)!
            .Invoke(dialog, [result, "合成测试配置"]);
        dialog.UpdateLayout();
    }

    private static void RunDialog(Window dialog, Action action)
    {
        Exception? failure = null;
        dialog.Dispatcher.BeginInvoke(DispatcherPriority.ApplicationIdle, new Action(() =>
        {
            try { action(); }
            catch (Exception exception) { failure = exception; dialog.Close(); }
        }));
        dialog.ShowDialog();
        if (failure is not null) throw failure;
    }

    private static void CheckPreviewAndConfirmation()
    {
        var dialog = NewDialog();
        RunDialog(dialog, () =>
        {
            Populate(dialog);
            var import = (Button)dialog.FindName("ImportButton");
            Assert(import.IsEnabled, "Confirm should be enabled for candidates");
            Render(dialog, "preview-660.png");
            dialog.Width = 540;
            dialog.UpdateLayout();
            Render(dialog, "preview-540.png");
            var importPosition = import.TranslatePoint(new Point(), dialog);
            Assert(importPosition.X + import.ActualWidth <= dialog.ActualWidth && importPosition.Y + import.ActualHeight <= dialog.ActualHeight,
                "Confirmation button must remain visible at minimum width");
            import.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            Assert(((TextBlock)dialog.FindName("ErrorText")).Visibility == Visibility.Visible, "Missing address should be rejected");
            Assert(dialog.Profiles.Count == 0, "Invalid confirmation must not produce accounts");
            var inputs = Descendants<TextBox>((DependencyObject)dialog.FindName("ProfilesList"))
                .Where(input => input.GetBindingExpression(TextBox.TextProperty)?.ParentBinding.Path.Path == "Host").ToArray();
            Assert(inputs.Length == 2, "Two address inputs expected");
            inputs[1].Text = "http://invalid";
            inputs[1].GetBindingExpression(TextBox.TextProperty)!.UpdateSource();
            import.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            Assert(dialog.Profiles.Count == 0, "URL address should be rejected");
            inputs[1].Text = "192.168.1.60";
            inputs[1].GetBindingExpression(TextBox.TextProperty)!.UpdateSource();
            import.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        });
        Assert(dialog.DialogResult == true && dialog.Profiles.Count == 2, "Confirm should return two profiles");
        Assert(dialog.Profiles[1].Host == "192.168.1.60", "Text binding must update imported host");
        Assert(dialog.SkippedCount == 3, "Skip counts must be retained");
        foreach (var profile in dialog.Profiles) AccountStore.Validate(profile);
    }

    private static void CheckIndependentImportNames()
    {
        var dialog = NewDialog();
        RunDialog(dialog, () =>
        {
            var profiles = Enumerable.Range(0, 5).Select(i => new AccountProfile(Guid.NewGuid(), $"原账户 {i}", "192.168.1.50", "",
                KeyA with { RegistrationKeyHex = i.ToString("x32") })).ToArray();
            typeof(ChiakiImportWindow).GetMethod("ApplyResult", BindingFlags.Instance | BindingFlags.NonPublic)!
                .Invoke(dialog, [new ChiakiImporter.Result(profiles, [], 0, 0, 0, []), "合成五账户"]);
            dialog.UpdateLayout();
            var inputs = Descendants<TextBox>((DependencyObject)dialog.FindName("ProfilesList")).ToArray();
            var names = inputs.Where(input => input.GetBindingExpression(TextBox.TextProperty)?.ParentBinding.Path.Path == "Name").ToArray();
            Assert(names.Length == 5, "Each imported account needs its own name field, separate from Host");
            names[0].Text = " ";
            names[0].GetBindingExpression(TextBox.TextProperty)!.UpdateSource();
            ((Button)dialog.FindName("ImportButton")).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            Assert(((TextBlock)dialog.FindName("ErrorText")).Text.Contains("名称"), "Empty name must not be reported as an address error");
            for (int i = 0; i < names.Length; i++)
            {
                names[i].Text = $"朋友的账户 {i + 1}";
                names[i].GetBindingExpression(TextBox.TextProperty)!.UpdateSource();
            }
            ((Button)dialog.FindName("ImportButton")).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        });
        Assert(dialog.Profiles.Count == 5 && dialog.Profiles.Select(p => p.Name).Distinct().Count() == 5, "Five independent names must import");
        Assert(dialog.Profiles.All(p => p.Host == "192.168.1.50"), "Naming must preserve the PS5 address");
        Assert(dialog.Profiles.Select(p => p.Keys.RegistrationKeyHex).SequenceEqual(Enumerable.Range(0, 5).Select(i => i.ToString("x32"))), "Naming must preserve every registration key");
    }

    private static void CheckEditDialog()
    {
        var original = new AccountProfile(Guid.NewGuid(), "原名称", "192.168.1.50", "", KeyA);
        var dialog = new EditAccountWindow(original) { Left = -10000, Top = -10000, WindowStartupLocation = WindowStartupLocation.Manual, ShowInTaskbar = false, ShowActivated = false };
        RunDialog(dialog, () =>
        {
            ((TextBox)dialog.FindName("NameInput")).Text = "朋友的账户";
            Render(dialog, "edit-account.png");
            ((Button)dialog.FindName("SaveButton")).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        });
        Assert(dialog.Profile == original with { Name = "朋友的账户" }, "Edit dialog must change only name, preserving host/id/keys");
        var cancel = new EditAccountWindow(original) { Left = -10000, Top = -10000, WindowStartupLocation = WindowStartupLocation.Manual, ShowInTaskbar = false, ShowActivated = false };
        RunDialog(cancel, () => { ((TextBox)cancel.FindName("HostInput")).Text = "192.168.1.99"; cancel.Close(); });
        Assert(cancel.Profile is null, "Cancel must not return edited data");
    }

    private static void CheckSavedAccountManagement()
    {
        var directory = Path.Combine(Path.GetTempPath(), "bridge-account-ui-" + Guid.NewGuid());
        Directory.CreateDirectory(directory);
        var protector = new TestProtector();
        var store = new AccountStore(Path.Combine(directory, "accounts.dat"), protector);
        var backend = new TestBackend();
        var sessions = new SessionCoordinator(backend);
        var main = (MainWindow)Activator.CreateInstance(typeof(MainWindow), BindingFlags.Instance | BindingFlags.NonPublic,
            null, [store, new PreferencesStore(Path.Combine(directory, "preferences.json")), sessions], null)!;
        var accounts = (System.Collections.ObjectModel.ObservableCollection<AccountProfile>)typeof(MainWindow)
            .GetField("_accounts", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(main)!;
        var first = new AccountProfile(Guid.NewGuid(), "相同名称", "192.168.1.50", "", KeyA);
        var second = first with { Id = Guid.NewGuid(), Keys = KeyB };
        var combo = (ComboBox)main.FindName("AccountCombo");
        void Session(Guid id) => typeof(MainWindow).GetField("_sessionAccountId", BindingFlags.Instance | BindingFlags.NonPublic)!.SetValue(main, id);
        void Invoke(string method, object value) => Await((Task)typeof(MainWindow).GetMethod(method, BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(main, [value])!);
        try
        {
            accounts.Add(first); accounts.Add(second); combo.SelectedItem = first;
            typeof(MainWindow).GetField("_loaded", BindingFlags.Instance | BindingFlags.NonPublic)!.SetValue(main, true);
            Await(store.SaveAsync(accounts));
            Await(sessions.ConnectAsync(first, new())); Session(first.Id);
            Invoke("SaveEditedAccountAsync", first with { Name = "已改名" });
            Assert(backend.Connects == 1 && backend.Stops == 0, "Renaming a connected account must not reconnect");
            Assert(accounts[0].Host == first.Host && accounts[0].Keys == first.Keys && accounts[1] == second, "Rename must preserve address, credentials and other account");
            Invoke("SaveEditedAccountAsync", accounts[0] with { Host = "192.168.1.99" });
            Assert(backend.Stops == 1 && backend.Connects == 1 && sessions.Status.State == BridgeState.Idle, "Editing active host must stop once without automatic reconnect");
            var saved = Await(store.LoadAsync());
            Assert(saved[0] == accounts[0], "Edited address must persist");
            protector.FailWrites = true;
            var before = accounts.ToArray();
            bool failed = false;
            try { Invoke("DeleteAccountAsync", first.Id); } catch (IOException) { failed = true; }
            Assert(failed && accounts.SequenceEqual(before) && Await(store.LoadAsync()).SequenceEqual(before), "Failed save must preserve disk and visible accounts");
            protector.FailWrites = false;
            Await(sessions.ConnectAsync(accounts[0], new())); Session(first.Id);
            Invoke("DeleteAccountAsync", first.Id);
            Assert(backend.Stops == 2 && backend.Connects == 2 && accounts.SequenceEqual(new[] { second }), "Deleting connected account must stop and remove only its ID");
            Assert(Equals(combo.SelectedItem, second) && sessions.Status.State == BridgeState.Idle, "Selecting remaining account must not connect it");
            Assert(Await(store.LoadAsync()).SequenceEqual(new[] { second }), "Deletion must persist");
            Invoke("DeleteAccountAsync", second.Id);
            Assert(accounts.Count == 0 && combo.SelectedItem is null && Await(store.LoadAsync()).Count == 0, "Deleting final account leaves empty list");
            accounts.Add(first); combo.SelectedItem = first; Await(store.SaveAsync(accounts));
            backend.HoldConnect = true;
            var connecting = sessions.ConnectAsync(first, new()); Session(first.Id);
            Assert(sessions.Status.State == BridgeState.Connecting, "Synthetic connection must be pending");
            Invoke("DeleteAccountAsync", first.Id);
            bool cancelled = false;
            try { Await(connecting); } catch (OperationCanceledException) { cancelled = true; }
            Assert(cancelled && accounts.Count == 0 && sessions.Status.State == BridgeState.Idle, "Deleting during handshake cancels connection before removing credentials");
        }
        finally
        {
            Await((Task)typeof(MainWindow).GetField("_preferencesSave", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(main)!);
            Await(sessions.DisposeAsync().AsTask()); Directory.Delete(directory, true);
        }
    }

    private static void Await(Task task)
    {
        if (!task.IsCompleted)
        {
            var frame = new DispatcherFrame();
            var dispatcher = Dispatcher.CurrentDispatcher;
            _ = task.ContinueWith(_ => dispatcher.BeginInvoke(new Action(() => frame.Continue = false)), TaskScheduler.Default);
            Dispatcher.PushFrame(frame);
        }
        task.GetAwaiter().GetResult();
    }
    private static T Await<T>(Task<T> task) { Await((Task)task); return task.GetAwaiter().GetResult(); }

    private static void CheckRestoredPreferences()
    {
        var directory = Path.Combine(Path.GetTempPath(), "bridge-startup-ui-" + Guid.NewGuid());
        Directory.CreateDirectory(directory);
        var store = new AccountStore(Path.Combine(directory, "accounts.dat"), new TestProtector());
        var preferences = new PreferencesStore(Path.Combine(directory, "preferences.json"));
        var a = new AccountProfile(Guid.NewGuid(), "账户A", "192.168.1.50", "", KeyA);
        var b = a with { Id = Guid.NewGuid(), Name = "账户B", Keys = KeyB };
        Await(store.SaveAsync([a, b]));
        Await(preferences.SaveAsync(new(AccountId: b.Id, ControllerIndex: 5, DeviceId: "synthetic-absent-hid", PollIntervalMicroseconds: 100, BackMapping: 1)));
        var backend = new TestBackend();
        var sessions = new SessionCoordinator(backend);
        using var startupProbe = new ManualResetEventSlim();
        Func<IReadOnlyList<ControllerInfo>> enumerate = () =>
        {
            startupProbe.Wait(TimeSpan.FromSeconds(3));
            return [new(0, "XInput 1", true)];
        };
        var main = (MainWindow)Activator.CreateInstance(typeof(MainWindow), BindingFlags.Instance | BindingFlags.NonPublic,
            null, [store, preferences, sessions, enumerate], null)!;
        try
        {
            typeof(MainWindow).GetMethod("Window_Loaded", BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(main, [main, new RoutedEventArgs()]);
            var loaded = typeof(MainWindow).GetField("_loaded", BindingFlags.Instance | BindingFlags.NonPublic)!;
            var frame = new DispatcherFrame();
            var watch = Stopwatch.StartNew();
            var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(5) };
            timer.Tick += (_, _) => { if ((bool)loaded.GetValue(main)! || watch.Elapsed > TimeSpan.FromSeconds(3)) frame.Continue = false; };
            timer.Start(); Dispatcher.PushFrame(frame); timer.Stop();
            ((DispatcherTimer)typeof(MainWindow).GetField("_statusTimer", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(main)!).Stop();
            Assert((bool)loaded.GetValue(main)!, "Startup should finish loading preferences");
            var refresh = (Task)typeof(MainWindow).GetField("_controllerRefresh", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(main)!;
            Assert(!refresh.IsCompleted, "Startup must finish while the driver enumeration is still blocked");
            Assert(Equals(((ComboBox)main.FindName("AccountCombo")).SelectedItem, b) && backend.Connects == 0, "Restore selected account without connecting");
            var poll = (ComboBox)main.FindName("PollIntervalCombo");
            Assert(((ComboBoxItem)poll.SelectedItem).Tag.ToString() == "100" && !poll.IsEnabled, "Remember 100 us but disable it for HID");
            Assert(((ComboBox)main.FindName("BackMappingCombo")).SelectedIndex == 1, "Remember Back mapping");
            var choice = ((ComboBox)main.FindName("ControllerCombo")).SelectedItem;
            Assert((string?)choice.GetType().GetProperty("DeviceId")!.GetValue(choice) == "synthetic-absent-hid", "Absent saved HID must remain selected");
            Assert(((TextBlock)main.FindName("PollIntervalHint")).Text.Contains("HID"), "Explain why sampling is disabled");
            startupProbe.Set();
            Await(refresh);
            choice = ((ComboBox)main.FindName("ControllerCombo")).SelectedItem;
            Assert((string?)choice.GetType().GetProperty("DeviceId")!.GetValue(choice) == "synthetic-absent-hid",
                "A completed startup probe must retain the absent saved HID instead of selecting an available XInput pad");
        }
        finally { startupProbe.Set(); Await(sessions.DisposeAsync().AsTask()); Directory.Delete(directory, true); }
    }

    private static void CheckControllerRefreshStaysResponsive()
    {
        var directory = Path.Combine(Path.GetTempPath(), "bridge-controller-ui-" + Guid.NewGuid());
        Directory.CreateDirectory(directory);
        var sessions = new SessionCoordinator(new TestBackend());
        using var release = new ManualResetEventSlim();
        var reads = 0;
        var enumerationThread = 0;
        Func<IReadOnlyList<ControllerInfo>> enumerate = () =>
        {
            enumerationThread = Environment.CurrentManagedThreadId;
            var read = Interlocked.Increment(ref reads);
            if (read > 1) release.Wait(TimeSpan.FromSeconds(3));
            if (read == 3) throw new IOException("Synthetic enumeration failure after close");
            return [new(0, "XInput 1", true), new(1, "XInput 2", true)];
        };
        var main = (MainWindow)Activator.CreateInstance(typeof(MainWindow), BindingFlags.Instance | BindingFlags.NonPublic,
            null, [new AccountStore(Path.Combine(directory, "accounts.dat"), new TestProtector()),
                new PreferencesStore(Path.Combine(directory, "preferences.json")), sessions, enumerate], null)!;
        var flags = BindingFlags.Instance | BindingFlags.NonPublic;
        var refresh = typeof(MainWindow).GetMethod("RefreshControllers", flags)!;
        var combo = (ComboBox)main.FindName("ControllerCombo");
        Task Refresh() => (Task?)refresh.Invoke(main, null) ?? Task.CompletedTask;
        try
        {
            Await(Refresh());
            typeof(MainWindow).GetField("_loaded", flags)!.SetValue(main, true);
            var watch = Stopwatch.StartNew();
            var pending = Refresh();
            Assert(watch.Elapsed < TimeSpan.FromMilliseconds(250), "A blocked controller probe must not block the WPF thread");
            Assert(enumerationThread != Environment.CurrentManagedThreadId, "Controller enumeration must run away from WPF");
            var tick = typeof(MainWindow).GetMethod("StatusTimer_Tick", flags)!;
            for (var i = 0; i < 12; i++) tick.Invoke(main, [null, EventArgs.Empty]);
            Assert(ReferenceEquals(pending, Refresh()), "Timer ticks must reuse the pending probe instead of queuing more probes");
            combo.SelectedIndex = 1;
            // The dispatcher remains usable while enumeration is still blocked.
            Await(Task.Delay(25));
            Assert(Volatile.Read(ref reads) == 2 && !pending.IsCompleted, "Only one slow controller probe may be in flight");
            release.Set();
            Await(pending);
            Assert((int)combo.SelectedItem.GetType().GetProperty("Index")!.GetValue(combo.SelectedItem)! == 1,
                "A completed refresh must preserve a selection changed while the probe was pending");

            release.Reset();
            var closingProbe = Refresh();
            var notice = ((TextBlock)main.FindName("NoticeText")).Text;
            typeof(MainWindow).GetMethod("Window_Closing", flags)!.Invoke(main, [main, new System.ComponentModel.CancelEventArgs()]);
            var closed = typeof(MainWindow).GetField("_closed", flags)!;
            var deadline = Stopwatch.StartNew();
            while (!(bool)closed.GetValue(main)! && deadline.Elapsed < TimeSpan.FromSeconds(1)) Await(Task.Delay(5));
            Assert((bool)closed.GetValue(main)! && !closingProbe.IsCompleted, "Window shutdown must not wait for a blocked device probe");
            release.Set();
            Await(closingProbe);
            Assert(((TextBlock)main.FindName("NoticeText")).Text == notice, "Late probe failures must not update a closed window");
        }
        finally
        {
            release.Set();
            Await((Task)typeof(MainWindow).GetField("_preferencesSave", flags)!.GetValue(main)!);
            Await(sessions.DisposeAsync().AsTask());
            Directory.Delete(directory, true);
        }
    }

    private static void CheckSingleInstance()
    {
        var type = typeof(MainWindow).Assembly.GetType("Ps5Bridge.App.SingleInstance")!;
        var name = @"Global\PS5InputBridge-test-" + Guid.NewGuid();
        IDisposable Create() => (IDisposable)Activator.CreateInstance(type, [name])!;
        bool Acquired(object instance) => (bool)type.GetProperty("Acquired")!.GetValue(instance)!;
        using (var first = Create())
        {
            Assert(Acquired(first), "First instance should acquire the account-store guard");
            var rejected = Task.Run(() => { using var second = Create(); return !Acquired(second); }).GetAwaiter().GetResult();
            Assert(rejected, "Another thread/process must not acquire an owned instance guard");
        }
        using var later = Create();
        Assert(Acquired(later), "Closing the first instance must release the guard");
    }

    private sealed class TestProtector : ISecretProtector
    {
        public bool FailWrites { get; set; }
        public byte[] Protect(byte[] data) => FailWrites ? throw new IOException("Synthetic write failure") : data.ToArray();
        public byte[] Unprotect(byte[] data) => data.ToArray();
    }
    private sealed class TestBackend : IBridgeBackend
    {
        public int Connects, Stops;
        public bool HoldConnect;
        public IReadOnlyList<ControllerInfo> GetControllers() => [];
        public Task<PairingKeys> PairAsync(string host, string id, string pin, CancellationToken token) => throw new NotSupportedException();
        public Task ConnectAsync(AccountProfile profile, BridgeOptions options, CancellationToken token) { Connects++; return HoldConnect ? Task.Delay(Timeout.Infinite, token) : Task.CompletedTask; }
        public Task UpdateOptionsAsync(BridgeOptions options, CancellationToken token) => Task.CompletedTask;
        public Task StopAsync(CancellationToken token) { Stops++; return Task.CompletedTask; }
        public BackendSnapshot GetSnapshot() => new(BridgeState.Connected, true, 0, 0, 0);
        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }

    private static void CheckCancellation()
    {
        var dialog = NewDialog();
        RunDialog(dialog, () => { Populate(dialog); dialog.Close(); });
        Assert(dialog.DialogResult != true && dialog.Profiles.Count == 0, "Cancel must not produce accounts");
    }

    private static void CheckLifetimeCancellation()
    {
        using var lifetime = new CancellationTokenSource();
        var dialog = NewDialog(lifetime.Token);
        RunDialog(dialog, () => { Populate(dialog); lifetime.Cancel(); });
        Assert(dialog.DialogResult != true && dialog.Profiles.Count == 0, "Lifetime cancellation must close without accounts");
    }

    private static IEnumerable<T> Descendants<T>(DependencyObject parent) where T : DependencyObject
    {
        for (int i = 0; i < VisualTreeHelper.GetChildrenCount(parent); i++)
        {
            var child = VisualTreeHelper.GetChild(parent, i);
            if (child is T match) yield return match;
            foreach (var nested in Descendants<T>(child)) yield return nested;
        }
    }

    private static void Render(FrameworkElement element, string file)
    {
        var bitmap = new RenderTargetBitmap((int)Math.Ceiling(element.ActualWidth), (int)Math.Ceiling(element.ActualHeight), 96, 96, PixelFormats.Pbgra32);
        var background = new DrawingVisual();
        using (var drawing = background.RenderOpen()) drawing.DrawRectangle(new SolidColorBrush(Color.FromRgb(243, 245, 250)), null, new Rect(0, 0, element.ActualWidth, element.ActualHeight));
        bitmap.Render(background);
        bitmap.Render(element);
        var encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(bitmap));
        using var output = File.Create(Path.Combine(Output, file));
        encoder.Save(output);
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
        checks++;
    }
}
