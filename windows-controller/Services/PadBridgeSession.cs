using System.Diagnostics;
using System.Net;
using PadBridge.Controller.Models;

namespace PadBridge.Controller.Services;

public enum SessionPhase
{
    Idle,
    Starting,
    Waiting,
    Streaming,
    Stopping,
    Error
}

public sealed record SessionSnapshot(
    SessionPhase Phase,
    string Title,
    string Detail,
    string Transport = "—");

public sealed class PadBridgeSession : IAsyncDisposable
{
    private readonly DisplayService _displayService = new();
    private readonly MdnsDiscovery _discovery = new();
    private readonly SemaphoreSlim _gate = new(1, 1);
    private CancellationTokenSource? _lifetime;
    private UsbMuxForwarder? _usbForwarder;
    private WindowRouter? _windowRouter;
    private Process? _host;
    private AppSettings? _activeSettings;
    private ConnectionEndpoint? _discoveredEndpoint;
    private CancellationTokenSource? _disconnectGrace;
    private bool _hasStreamed;
    private SessionSnapshot _snapshot = new(SessionPhase.Idle, "Ready", "Connect your iPad");

    public event Action<SessionSnapshot>? StateChanged;
    public event Action<string>? LogReceived;
    public event Action? RemoteSessionEnded;

    public SessionSnapshot Snapshot => _snapshot;
    public bool IsActive => _snapshot.Phase is SessionPhase.Starting or SessionPhase.Waiting
                                           or SessionPhase.Streaming;

    /// <summary>
    /// Lightweight tray-mode discovery. No virtual monitor, host, FFmpeg, or
    /// encoder is started until the foreground iPad receiver is available.
    /// </summary>
    public async Task<bool> ReceiverAvailableAsync(
        AppSettings settings, CancellationToken cancellationToken)
    {
        if (IsActive) return true;

        if (settings.ConnectionMode is ConnectionMode.Auto or ConnectionMode.Usb)
        {
            await using var probe = new UsbMuxForwarder(0, 52100, _ => { });
            if (await probe.IsDevicePortOpenAsync(cancellationToken)) return true;
        }

        if (settings.ConnectionMode is ConnectionMode.Auto or ConnectionMode.Wifi)
        {
            var discovered = await _discovery.DiscoverAsync(
                TimeSpan.FromMilliseconds(900), _ => { }, cancellationToken);
            if (discovered is not null)
            {
                Interlocked.Exchange(ref _discoveredEndpoint,
                    new ConnectionEndpoint(discovered.Address.ToString(), discovered.Port,
                        "Wi-Fi", settings.WifiBitrateMbps));
                return true;
            }
        }

        return false;
    }

    public async Task StartAsync(AppSettings settings)
    {
        await _gate.WaitAsync();
        try
        {
            if (IsActive) return;
            _activeSettings = settings;
            _hasStreamed = false;
            _lifetime = new CancellationTokenSource();
            var cancellationToken = _lifetime.Token;
            SetState(SessionPhase.Starting, "Preparing PadBridge", "Enabling the iPad display…");

            try
            {
                var paths = ResolveDependencies();
                Log($"PadBridge host: {paths.Host}");
                var display = await _displayService.PrepareNativeDisplayAsync(Log, cancellationToken);
                var endpoint = await ResolveEndpointAsync(settings, cancellationToken);

                if (settings.RouteNewWindows)
                {
                    _windowRouter = new WindowRouter(display);
                    if (_windowRouter.Start())
                        Log("New apps will open on the display under the cursor.");
                }

                StartHost(paths, endpoint, display, settings);
                SetState(SessionPhase.Waiting, "Waiting for iPad",
                    endpoint.Transport == "USB"
                        ? "Open PadBridge on the connected iPad"
                        : $"Connecting to {endpoint.Host}:{endpoint.Port}",
                    endpoint.Transport);
            }
            catch (OperationCanceledException)
            {
                SetState(SessionPhase.Idle, "Ready", "Connect your iPad");
            }
            catch (Exception exception)
            {
                Log(exception.Message);
                SetState(SessionPhase.Error, "Couldn’t start", exception.Message);
                await CleanupCoreAsync(collapseDisplay: false, CancellationToken.None);
            }
        }
        finally
        {
            _gate.Release();
        }
    }

    public async Task StopAsync()
    {
        await _gate.WaitAsync();
        try
        {
            if (_snapshot.Phase == SessionPhase.Idle) return;
            SetState(SessionPhase.Stopping, "Disconnecting", "Cleaning up the display…",
                     _snapshot.Transport);
            _lifetime?.Cancel();
            var collapse = _activeSettings?.CollapseDisplayOnStop ?? true;
            await CleanupCoreAsync(collapse, CancellationToken.None);
            SetState(SessionPhase.Idle, "Ready", "Connect your iPad");
        }
        finally
        {
            _gate.Release();
        }
    }

    public async ValueTask DisposeAsync()
    {
        await StopAsync();
        _gate.Dispose();
    }

    private async Task<ConnectionEndpoint> ResolveEndpointAsync(
        AppSettings settings, CancellationToken cancellationToken)
    {
        var cachedEndpoint = Interlocked.Exchange(ref _discoveredEndpoint, null);
        if (cachedEndpoint is not null)
        {
            Log($"Found active iPad at {cachedEndpoint.Host}:{cachedEndpoint.Port}.");
            return cachedEndpoint;
        }

        if (settings.ConnectionMode is ConnectionMode.Auto or ConnectionMode.Usb)
        {
            var forwarder = new UsbMuxForwarder(52100, 52100, Log);
            var device = await forwarder.FindUsbDeviceAsync(cancellationToken);
            if (device is not null || settings.ConnectionMode == ConnectionMode.Usb)
            {
                forwarder.Start();
                _usbForwarder = forwarder;
                Log(device is null
                    ? "USB mode is waiting for a trusted iPad."
                    : "Trusted iPad detected over USB.");
                return new ConnectionEndpoint("127.0.0.1", 52100, "USB",
                    settings.UsbBitrateMbps);
            }
            await forwarder.DisposeAsync();
        }

        if (settings.ConnectionMode is ConnectionMode.Auto or ConnectionMode.Wifi)
        {
            var discovered = await _discovery.DiscoverAsync(
                TimeSpan.FromSeconds(settings.ConnectionMode == ConnectionMode.Wifi ? 6 : 3),
                Log, cancellationToken);
            if (discovered is not null)
            {
                Log($"Found iPad at {discovered.Address}:{discovered.Port}.");
                return new ConnectionEndpoint(discovered.Address.ToString(), discovered.Port,
                    "Wi-Fi", settings.WifiBitrateMbps);
            }

            if (IPAddress.TryParse(settings.WifiAddress.Trim(), out var savedAddress))
            {
                Log($"Using saved iPad address {savedAddress}.");
                return new ConnectionEndpoint(savedAddress.ToString(), 52100, "Wi-Fi",
                    settings.WifiBitrateMbps);
            }

            if (settings.ConnectionMode == ConnectionMode.Wifi)
                throw new InvalidOperationException(
                    "No PadBridge iPad was found on Wi-Fi. Open the iPad app or enter its IP address.");
        }

        // Auto mode stays useful even when the cable is plugged in after the
        // Windows app starts: keep a native USB listener ready and let the host retry.
        _usbForwarder = new UsbMuxForwarder(52100, 52100, Log);
        _usbForwarder.Start();
        Log("No iPad found yet; waiting for USB.");
        return new ConnectionEndpoint("127.0.0.1", 52100, "USB",
            settings.UsbBitrateMbps);
    }

    private void StartHost(DependencyPaths paths, ConnectionEndpoint endpoint,
                           DisplayMonitor display, AppSettings settings)
    {
        var start = new ProcessStartInfo(paths.Host)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            WorkingDirectory = Path.GetDirectoryName(paths.Host)!
        };
        Add(start, "--host", endpoint.Host);
        Add(start, "--port", endpoint.Port.ToString());
        Add(start, "--fps", Math.Clamp(settings.FramesPerSecond, 30, 120).ToString());
        Add(start, "--bitrate", (Math.Clamp(endpoint.BitrateMbps, 10, 100) * 1_000_000).ToString());
        Add(start, "--adapter", Math.Max(0, settings.AdapterIndex).ToString());
        Add(start, "--display", Math.Max(0, settings.CaptureOutputIndex).ToString());
        Add(start, "--input-display", display.Index.ToString());
        Add(start, "--ffmpeg", paths.Ffmpeg);
        if (settings.ZeroCopy) start.ArgumentList.Add("--zero-copy");

        _host = new Process { StartInfo = start, EnableRaisingEvents = true };
        _host.OutputDataReceived += (_, eventArgs) => HandleHostLine(eventArgs.Data, false);
        _host.ErrorDataReceived += (_, eventArgs) => HandleHostLine(eventArgs.Data, true);
        _host.Exited += (_, _) => HandleHostExit();
        if (!_host.Start()) throw new InvalidOperationException("Could not launch the PadBridge host.");
        _host.BeginOutputReadLine();
        _host.BeginErrorReadLine();
        Log($"Starting native {settings.FramesPerSecond} Hz stream over {endpoint.Transport}.");
    }

    private void HandleHostLine(string? line, bool error)
    {
        if (string.IsNullOrWhiteSpace(line)) return;
        Log(line);
        if (line.Contains("iPad handshake complete", StringComparison.OrdinalIgnoreCase))
        {
            _hasStreamed = true;
            CancelDisconnectGrace();
            SetState(SessionPhase.Streaming, "Connected",
                $"2420 × 1668 • {_activeSettings?.FramesPerSecond ?? 120} Hz • near-zero latency",
                _snapshot.Transport);
        }
        else if (line.Contains("disconnected; reconnecting", StringComparison.OrdinalIgnoreCase) ||
                 line.Contains("Transport disconnected", StringComparison.OrdinalIgnoreCase))
        {
            SetState(SessionPhase.Waiting, "Reconnecting", "Waiting for the iPad app…",
                _snapshot.Transport);
            if (_hasStreamed) BeginDisconnectGrace();
        }
    }

    private void HandleHostExit()
    {
        var cancellationRequested = _lifetime?.IsCancellationRequested ?? true;
        if (cancellationRequested) return;
        var code = _host?.ExitCode ?? -1;
        _ = CleanupUnexpectedHostExitAsync(code);
    }

    private async Task CleanupUnexpectedHostExitAsync(int exitCode)
    {
        await _gate.WaitAsync();
        try
        {
            if (_lifetime?.IsCancellationRequested ?? true) return;
            _lifetime.Cancel();
            var transport = _snapshot.Transport;
            await CleanupCoreAsync(collapseDisplay: true, CancellationToken.None);
            SetState(SessionPhase.Error, "Streaming stopped",
                $"The display host exited with code {exitCode}. Open diagnostics for details.",
                transport);
        }
        finally
        {
            _gate.Release();
        }
    }

    private async Task CleanupCoreAsync(bool collapseDisplay, CancellationToken cancellationToken)
    {
        CancelDisconnectGrace();
        _windowRouter?.Dispose();
        _windowRouter = null;

        if (_host is not null)
        {
            try
            {
                if (!_host.HasExited)
                {
                    _host.Kill(entireProcessTree: true);
                    await _host.WaitForExitAsync(cancellationToken);
                }
            }
            catch (InvalidOperationException) { }
            finally
            {
                _host.Dispose();
                _host = null;
            }
        }

        if (_usbForwarder is not null)
        {
            await _usbForwarder.DisposeAsync();
            _usbForwarder = null;
        }

        if (collapseDisplay)
        {
            try { await _displayService.CollapseToInternalAsync(Log, cancellationToken); }
            catch (Exception exception) { Log($"Display cleanup: {exception.Message}"); }
        }

        _lifetime?.Dispose();
        _lifetime = null;
    }

    private void BeginDisconnectGrace()
    {
        CancelDisconnectGrace();
        var lifetime = _lifetime;
        if (lifetime is null || lifetime.IsCancellationRequested) return;
        _disconnectGrace = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token);
        var token = _disconnectGrace.Token;
        _ = EndAfterDisconnectAsync(token);
    }

    private async Task EndAfterDisconnectAsync(CancellationToken cancellationToken)
    {
        try
        {
            await Task.Delay(TimeSpan.FromSeconds(3), cancellationToken);
            Log("iPad disconnected. Ending capture and releasing all resources.");
            await StopAsync();
            RemoteSessionEnded?.Invoke();
        }
        catch (OperationCanceledException) { }
    }

    private void CancelDisconnectGrace()
    {
        var grace = Interlocked.Exchange(ref _disconnectGrace, null);
        if (grace is null) return;
        grace.Cancel();
        grace.Dispose();
    }

    private static DependencyPaths ResolveDependencies()
    {
        var baseDirectory = AppContext.BaseDirectory;
        var host = Path.Combine(baseDirectory, "padbridge_host.exe");
        var ffmpeg = Path.Combine(baseDirectory, "ffmpeg.exe");
        if (!File.Exists(host))
            throw new FileNotFoundException("padbridge_host.exe is missing. Reinstall PadBridge.", host);
        if (!File.Exists(ffmpeg))
            throw new FileNotFoundException("The bundled ffmpeg.exe is missing. Reinstall PadBridge.", ffmpeg);
        return new DependencyPaths(host, ffmpeg);
    }

    private static void Add(ProcessStartInfo start, string name, string value)
    {
        start.ArgumentList.Add(name);
        start.ArgumentList.Add(value);
    }

    private void SetState(SessionPhase phase, string title, string detail, string transport = "—")
    {
        _snapshot = new SessionSnapshot(phase, title, detail, transport);
        StateChanged?.Invoke(_snapshot);
    }

    private void Log(string message) => LogReceived?.Invoke(
        $"{DateTime.Now:HH:mm:ss}  {message.Trim()}");

    private sealed record ConnectionEndpoint(string Host, int Port, string Transport, int BitrateMbps);
    private sealed record DependencyPaths(string Host, string Ffmpeg);
}
