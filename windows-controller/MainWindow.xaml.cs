using System.ComponentModel;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using PadBridge.Controller.Models;
using PadBridge.Controller.Services;
using Forms = System.Windows.Forms;
using DrawingIcon = System.Drawing.Icon;
using DrawingSystemIcons = System.Drawing.SystemIcons;

namespace PadBridge.Controller;

public partial class MainWindow : Window
{
    private readonly SettingsStore _settingsStore = new();
    private readonly PadBridgeSession _session = new();
    private readonly StringBuilder _log = new();
    private readonly bool _backgroundLaunch;
    private AppSettings _settings;
    private Forms.NotifyIcon? _trayIcon;
    private Forms.ToolStripMenuItem? _trayConnectionItem;
    private bool _allowClose;
    private bool _busy;
    private bool _exiting;

    public MainWindow(bool backgroundLaunch)
    {
        _backgroundLaunch = backgroundLaunch;
        _settings = _settingsStore.Load();
        InitializeComponent();
        ApplySettingsToControls();
        _session.StateChanged += snapshot => Dispatcher.InvokeAsync(() => ApplyState(snapshot));
        _session.LogReceived += line => Dispatcher.InvokeAsync(() => AppendLog(line));
        _session.RemoteSessionEnded += () => Dispatcher.InvokeAsync(ExitApplication);
        CreateTrayIcon();
        Loaded += MainWindow_Loaded;
    }

    private async void MainWindow_Loaded(object sender, RoutedEventArgs e)
    {
        ApplyState(_session.Snapshot);
        if (_settings.AutoConnect)
            await StartSessionAsync();
        if (_backgroundLaunch) Hide();
    }

    private async void ConnectButton_Click(object sender, RoutedEventArgs e)
    {
        if (_busy) return;
        if (_session.IsActive)
            await StopSessionAsync();
        else
            await StartSessionAsync();
    }

    private async Task StartSessionAsync()
    {
        if (_busy) return;
        _busy = true;
        ConnectButton.IsEnabled = false;
        try
        {
            _settings = ReadSettingsFromControls();
            _settingsStore.Save(_settings);
            StartupService.SetEnabled(_settings.StartWithWindows);
            await _session.StartAsync(_settings);
        }
        finally
        {
            _busy = false;
            ConnectButton.IsEnabled = true;
            ApplyState(_session.Snapshot);
        }
    }

    private async Task StopSessionAsync()
    {
        if (_busy) return;
        _busy = true;
        ConnectButton.IsEnabled = false;
        try { await _session.StopAsync(); }
        finally
        {
            _busy = false;
            ConnectButton.IsEnabled = true;
            ApplyState(_session.Snapshot);
        }
    }

    private void ApplyState(SessionSnapshot snapshot)
    {
        StatusTitle.Text = snapshot.Title;
        StatusDetail.Text = snapshot.Detail;
        TransportValue.Text = snapshot.Transport == "—"
            ? GetSelectedMode().ToString().ToUpperInvariant()
            : snapshot.Transport.ToUpperInvariant();
        StatusDot.Fill = new SolidColorBrush(snapshot.Phase switch
        {
            SessionPhase.Streaming => Color.FromRgb(104, 247, 154),
            SessionPhase.Starting or SessionPhase.Waiting => Color.FromRgb(244, 201, 93),
            SessionPhase.Error => Color.FromRgb(255, 107, 107),
            _ => Color.FromRgb(112, 112, 112)
        });
        ConnectButton.Content = snapshot.Phase switch
        {
            SessionPhase.Streaming or SessionPhase.Waiting or SessionPhase.Starting => "Disconnect iPad",
            SessionPhase.Stopping => "Disconnecting…",
            _ => "Connect iPad"
        };
        RefreshValue.Text = $"{GetComboTagInt(FpsBox, 120)} Hz adaptive";
        if (_trayConnectionItem is not null)
            _trayConnectionItem.Text = _session.IsActive ? "Disconnect" : "Connect";
        if (snapshot.Phase == SessionPhase.Error)
            DiagnosticsExpander.IsExpanded = true;
    }

    private void AppendLog(string line)
    {
        _log.AppendLine(line);
        if (_log.Length > 50_000) _log.Remove(0, _log.Length - 40_000);
        LogBox.Text = _log.ToString();
        LogBox.ScrollToEnd();
    }

    private void ApplySettingsToControls()
    {
        ConnectionModeBox.SelectedIndex = _settings.ConnectionMode switch
        {
            ConnectionMode.Usb => 1,
            ConnectionMode.Wifi => 2,
            _ => 0
        };
        WifiAddressBox.Text = _settings.WifiAddress;
        FpsBox.SelectedIndex = _settings.FramesPerSecond == 60 ? 1 : 0;
        UsbBitrateBox.Text = _settings.UsbBitrateMbps.ToString();
        WifiBitrateBox.Text = _settings.WifiBitrateMbps.ToString();
        AdapterBox.Text = _settings.AdapterIndex.ToString();
        OutputBox.Text = _settings.CaptureOutputIndex.ToString();
        ZeroCopyCheck.IsChecked = _settings.ZeroCopy;
        RouteWindowsCheck.IsChecked = _settings.RouteNewWindows;
        CollapseDisplayCheck.IsChecked = _settings.CollapseDisplayOnStop;
        AutoConnectCheck.IsChecked = _settings.AutoConnect;
        StartWithWindowsCheck.IsChecked = _settings.StartWithWindows;
        UpdateWifiField();
    }

    private AppSettings ReadSettingsFromControls() => new()
    {
        ConnectionMode = GetSelectedMode(),
        WifiAddress = WifiAddressBox.Text.Trim(),
        FramesPerSecond = GetComboTagInt(FpsBox, 120),
        UsbBitrateMbps = ParseInt(UsbBitrateBox.Text, 60, 10, 100),
        WifiBitrateMbps = ParseInt(WifiBitrateBox.Text, 35, 10, 100),
        AdapterIndex = ParseInt(AdapterBox.Text, 1, 0, 8),
        CaptureOutputIndex = ParseInt(OutputBox.Text, 0, 0, 8),
        ZeroCopy = ZeroCopyCheck.IsChecked == true,
        RouteNewWindows = RouteWindowsCheck.IsChecked == true,
        CollapseDisplayOnStop = CollapseDisplayCheck.IsChecked == true,
        AutoConnect = AutoConnectCheck.IsChecked == true,
        StartWithWindows = StartWithWindowsCheck.IsChecked == true
    };

    private ConnectionMode GetSelectedMode()
    {
        if (ConnectionModeBox.SelectedItem is ComboBoxItem item &&
            Enum.TryParse<ConnectionMode>(item.Tag?.ToString(), out var mode)) return mode;
        return ConnectionMode.Auto;
    }

    private static int GetComboTagInt(System.Windows.Controls.ComboBox box, int fallback) =>
        box.SelectedItem is ComboBoxItem item && int.TryParse(item.Tag?.ToString(), out var value)
            ? value : fallback;

    private static int ParseInt(string value, int fallback, int minimum, int maximum) =>
        int.TryParse(value, out var parsed) ? Math.Clamp(parsed, minimum, maximum) : fallback;

    private void ConnectionModeBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (WifiAddressBox is not null) UpdateWifiField();
    }

    private void UpdateWifiField()
    {
        WifiAddressBox.IsEnabled = GetSelectedMode() != ConnectionMode.Usb;
        WifiAddressBox.Opacity = WifiAddressBox.IsEnabled ? 1 : 0.45;
    }

    private void CreateTrayIcon()
    {
        var menu = new Forms.ContextMenuStrip();
        var open = new Forms.ToolStripMenuItem("Open PadBridge");
        open.Click += (_, _) => Dispatcher.Invoke(ShowFromTray);
        _trayConnectionItem = new Forms.ToolStripMenuItem("Connect");
        _trayConnectionItem.Click += async (_, _) =>
        {
            if (_session.IsActive) await Dispatcher.InvokeAsync(StopSessionAsync).Task.Unwrap();
            else await Dispatcher.InvokeAsync(StartSessionAsync).Task.Unwrap();
        };
        var exit = new Forms.ToolStripMenuItem("Exit");
        exit.Click += async (_, _) => await Dispatcher.InvokeAsync(ExitApplication).Task.Unwrap();
        menu.Items.Add(open);
        menu.Items.Add(_trayConnectionItem);
        menu.Items.Add(new Forms.ToolStripSeparator());
        menu.Items.Add(exit);

        var icon = Environment.ProcessPath is { } path
            ? DrawingIcon.ExtractAssociatedIcon(path)
            : DrawingSystemIcons.Application;
        _trayIcon = new Forms.NotifyIcon
        {
            Text = "PadBridge",
            Icon = icon ?? DrawingSystemIcons.Application,
            ContextMenuStrip = menu,
            Visible = true
        };
        _trayIcon.DoubleClick += (_, _) => Dispatcher.Invoke(ShowFromTray);
    }

    private void ShowFromTray()
    {
        Show();
        WindowState = WindowState.Normal;
        Activate();
        Topmost = true;
        Topmost = false;
        Focus();
    }

    private async Task ExitApplication()
    {
        if (_exiting) return;
        _exiting = true;
        _allowClose = true;
        await _session.DisposeAsync();
        _trayIcon?.Dispose();
        _trayIcon = null;
        Close();
        System.Windows.Application.Current.Shutdown();
    }

    private void Window_Closing(object? sender, CancelEventArgs e)
    {
        if (_allowClose) return;
        e.Cancel = true;
        _ = ExitApplication();
    }

    private void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ChangedButton == MouseButton.Left) DragMove();
    }

    private void MinimizeButton_Click(object sender, RoutedEventArgs e) =>
        WindowState = WindowState.Minimized;

    private async void CloseButton_Click(object sender, RoutedEventArgs e) =>
        await ExitApplication();
}
