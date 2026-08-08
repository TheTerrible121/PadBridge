using System.Diagnostics;
using System.Runtime.InteropServices;

namespace PadBridge.Controller.Services;

public readonly record struct DisplayRectangle(int Left, int Top, int Right, int Bottom)
{
    public int Width => Right - Left;
    public int Height => Bottom - Top;
    public bool Contains(int x, int y) => x >= Left && x < Right && y >= Top && y < Bottom;
}

public sealed record DisplayMonitor(
    int Index,
    nint Handle,
    string DeviceName,
    DisplayRectangle Bounds,
    DisplayRectangle WorkArea,
    bool IsPrimary,
    int RefreshRate);

public sealed class DisplayService
{
    public const int TargetWidth = 2420;
    public const int TargetHeight = 1668;

    public async Task<DisplayMonitor> PrepareNativeDisplayAsync(
        Action<string> log, CancellationToken cancellationToken)
    {
        log("Enabling extended-display mode…");
        await RunDisplaySwitchAsync("/extend", cancellationToken);

        var deadline = DateTime.UtcNow.AddSeconds(8);
        var attemptedModeChange = false;
        do
        {
            cancellationToken.ThrowIfCancellationRequested();
            var monitors = EnumerateMonitors();
            var primary = monitors.FirstOrDefault(m => m.IsPrimary);
            var target = monitors
                .Where(m => !m.IsPrimary &&
                            m.Bounds.Width == TargetWidth && m.Bounds.Height == TargetHeight)
                .OrderByDescending(m => m.Bounds.Left)
                .FirstOrDefault();
            if (target is not null)
            {
                var targetLeft = primary?.Bounds.Right ?? target.Bounds.Left;
                var targetTop = primary?.Bounds.Top ?? target.Bounds.Top;
                if (target.RefreshRate is < 119 or > 121 || target.Bounds.Left != targetLeft ||
                    target.Bounds.Top != targetTop)
                {
                    log("Setting the iPad display to native 120 Hz on the right side…");
                    if (!TrySetNativeMode(target.DeviceName, targetLeft, targetTop))
                        throw new InvalidOperationException(
                            "Windows could not set the virtual display to 2420 × 1668 at 120 Hz.");
                    await Task.Delay(750, cancellationToken);
                    continue;
                }
                log($"iPad display ready: {TargetWidth} × {TargetHeight} at monitor #{target.Index}.");
                return target;
            }

            // VDD can briefly return as 800×600 after /extend. When it is the
            // laptop's only secondary monitor, safely restore the native iPad mode.
            var secondary = monitors.Where(m => !m.IsPrimary).ToList();
            if (!attemptedModeChange && secondary.Count == 1)
            {
                attemptedModeChange = true;
                log("Restoring the virtual display to 2420 × 1668 at 120 Hz…");
                TrySetNativeMode(secondary[0].DeviceName,
                    primary?.Bounds.Right ?? secondary[0].Bounds.Left,
                    primary?.Bounds.Top ?? secondary[0].Bounds.Top);
            }
            await Task.Delay(350, cancellationToken);
        } while (DateTime.UtcNow < deadline);

        var summary = string.Join(", ", EnumerateMonitors().Select(m =>
            $"#{m.Index} {m.Bounds.Width}×{m.Bounds.Height}{(m.IsPrimary ? " primary" : "")}"));
        throw new InvalidOperationException(
            $"The 2420 × 1668 virtual display is not active. Detected: {summary}. " +
            "Open VDD Control once, enable the PadBridge display at 2420 × 1668 / 120 Hz, then retry.");
    }

    public async Task CollapseToInternalAsync(Action<string> log, CancellationToken cancellationToken)
    {
        var monitors = EnumerateMonitors();
        // Avoid disabling a user's physical desktop setup. Automatic collapse
        // is safe only for the laptop panel plus the known PadBridge display.
        if (monitors.Count == 2 && monitors.Any(m => m.IsPrimary) &&
            monitors.Any(m => !m.IsPrimary && m.Bounds.Width == TargetWidth &&
                              m.Bounds.Height == TargetHeight))
        {
            log("Removing the iPad display from the Windows desktop…");
            await RunDisplaySwitchAsync("/internal", cancellationToken);
        }
    }

    public static IReadOnlyList<DisplayMonitor> EnumerateMonitors()
    {
        var result = new List<DisplayMonitor>();
        MonitorEnumProc callback = (monitor, _, _, _) =>
        {
            var info = new MonitorInfoEx { Size = Marshal.SizeOf<MonitorInfoEx>() };
            if (GetMonitorInfo(monitor, ref info))
            {
                result.Add(new DisplayMonitor(
                    result.Count,
                    monitor,
                    info.DeviceName.TrimEnd('\0'),
                    ToDisplayRectangle(info.Monitor),
                    ToDisplayRectangle(info.WorkArea),
                    (info.Flags & MonitorInfoPrimary) != 0,
                    ReadRefreshRate(info.DeviceName)));
            }
            return true;
        };
        EnumDisplayMonitors(nint.Zero, nint.Zero, callback, nint.Zero);
        GC.KeepAlive(callback);
        return result;
    }

    private static DisplayRectangle ToDisplayRectangle(NativeRect value) =>
        new(value.Left, value.Top, value.Right, value.Bottom);

    private static int ReadRefreshRate(string deviceName)
    {
        var mode = CreateDevMode();
        return EnumDisplaySettings(deviceName, EnumCurrentSettings, ref mode)
            ? mode.DisplayFrequency
            : 0;
    }

    private static bool TrySetNativeMode(string deviceName, int left, int top)
    {
        var mode = CreateDevMode();
        if (!EnumDisplaySettings(deviceName, EnumCurrentSettings, ref mode)) return false;
        mode.PelsWidth = TargetWidth;
        mode.PelsHeight = TargetHeight;
        mode.DisplayFrequency = 120;
        mode.PositionX = left;
        mode.PositionY = top;
        mode.Fields |= DmPosition | DmPelsWidth | DmPelsHeight | DmDisplayFrequency;
        return ChangeDisplaySettingsEx(deviceName, ref mode, nint.Zero,
            CdsUpdateRegistry, nint.Zero) == DispChangeSuccessful;
    }

    private static DevMode CreateDevMode() => new()
    {
        DeviceName = new string('\0', 32),
        FormName = new string('\0', 32),
        Size = checked((short)Marshal.SizeOf<DevMode>())
    };

    private static async Task RunDisplaySwitchAsync(string argument, CancellationToken cancellationToken)
    {
        var executable = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),
                                      "DisplaySwitch.exe");
        using var process = Process.Start(new ProcessStartInfo(executable, argument)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            WindowStyle = ProcessWindowStyle.Hidden
        });
        if (process is not null)
        {
            await process.WaitForExitAsync(cancellationToken);
        }
        await Task.Delay(500, cancellationToken);
    }

    private const uint MonitorInfoPrimary = 1;
    private const int EnumCurrentSettings = -1;
    private const int DispChangeSuccessful = 0;
    private const uint CdsUpdateRegistry = 0x00000001;
    private const int DmPosition = 0x00000020;
    private const int DmPelsWidth = 0x00080000;
    private const int DmPelsHeight = 0x00100000;
    private const int DmDisplayFrequency = 0x00400000;

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeRect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct MonitorInfoEx
    {
        public int Size;
        public NativeRect Monitor;
        public NativeRect WorkArea;
        public uint Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string DeviceName;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct DevMode
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
        public short SpecVersion;
        public short DriverVersion;
        public short Size;
        public short DriverExtra;
        public int Fields;
        public int PositionX;
        public int PositionY;
        public int DisplayOrientation;
        public int DisplayFixedOutput;
        public short Color;
        public short Duplex;
        public short YResolution;
        public short TTOption;
        public short Collate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string FormName;
        public short LogPixels;
        public int BitsPerPel;
        public int PelsWidth;
        public int PelsHeight;
        public int DisplayFlags;
        public int DisplayFrequency;
        public int ICMMethod;
        public int ICMIntent;
        public int MediaType;
        public int DitherType;
        public int Reserved1;
        public int Reserved2;
        public int PanningWidth;
        public int PanningHeight;
    }

    private delegate bool MonitorEnumProc(nint monitor, nint hdc, nint rect, nint data);

    [DllImport("user32.dll")]
    private static extern bool EnumDisplayMonitors(
        nint hdc, nint clipRect, MonitorEnumProc callback, nint data);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern bool GetMonitorInfo(nint monitor, ref MonitorInfoEx info);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern bool EnumDisplaySettings(
        string deviceName, int modeNumber, ref DevMode mode);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int ChangeDisplaySettingsEx(
        string deviceName, ref DevMode mode, nint window, uint flags, nint parameters);
}
