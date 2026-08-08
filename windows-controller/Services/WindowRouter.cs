using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace PadBridge.Controller.Services;

/// <summary>
/// Optionally places newly shown top-level windows on the iPad when the cursor
/// is already over the iPad display. Existing windows are never moved.
/// </summary>
public sealed class WindowRouter : IDisposable
{
    private const uint EventObjectShow = 0x8002;
    private const uint WineventOutOfContext = 0x0000;
    private const uint WineventSkipOwnProcess = 0x0002;
    private const int ObjectIdWindow = 0;
    private const uint GaRoot = 2;
    private const int GwlExStyle = -20;
    private const long WsExToolWindow = 0x00000080L;
    private const long WsExNoActivate = 0x08000000L;
    private const uint SwpNoZOrder = 0x0004;
    private const uint SwpNoActivate = 0x0010;
    private const uint SwpShowWindow = 0x0040;

    private readonly DisplayMonitor _target;
    private readonly WinEventProc _callback;
    private nint _hook;

    public WindowRouter(DisplayMonitor target)
    {
        _target = target;
        _callback = OnWindowEvent;
    }

    public bool Start()
    {
        if (_hook != nint.Zero) return true;
        _hook = SetWinEventHook(EventObjectShow, EventObjectShow, nint.Zero, _callback,
            0, 0, WineventOutOfContext | WineventSkipOwnProcess);
        return _hook != nint.Zero;
    }

    public void Dispose()
    {
        if (_hook == nint.Zero) return;
        UnhookWinEvent(_hook);
        _hook = nint.Zero;
        GC.SuppressFinalize(this);
    }

    private void OnWindowEvent(nint hook, uint eventType, nint window, int objectId,
                               int childId, uint eventThread, uint eventTime)
    {
        if (window == nint.Zero || objectId != ObjectIdWindow) return;
        _ = Task.Delay(90).ContinueWith(_ => TryRoute(window),
            CancellationToken.None, TaskContinuationOptions.None, TaskScheduler.Default);
    }

    private void TryRoute(nint window)
    {
        if (!IsWindow(window) || !IsWindowVisible(window) ||
            GetAncestor(window, GaRoot) != window) return;

        GetWindowThreadProcessId(window, out var processId);
        if (processId == (uint)Environment.ProcessId) return;
        var extendedStyle = GetWindowLongPtr(window, GwlExStyle).ToInt64();
        if ((extendedStyle & (WsExToolWindow | WsExNoActivate)) != 0) return;

        var className = new StringBuilder(128);
        GetClassName(window, className, className.Capacity);
        if (className.ToString() is "Shell_TrayWnd" or "Progman" or "WorkerW") return;

        if (!GetCursorPos(out var cursor) ||
            !_target.Bounds.Contains(cursor.X, cursor.Y) ||
            !GetWindowRect(window, out var bounds)) return;

        var width = Math.Max(320, bounds.Right - bounds.Left);
        var height = Math.Max(240, bounds.Bottom - bounds.Top);
        var intersectionWidth = Math.Max(0,
            Math.Min(bounds.Right, _target.Bounds.Right) - Math.Max(bounds.Left, _target.Bounds.Left));
        var intersectionHeight = Math.Max(0,
            Math.Min(bounds.Bottom, _target.Bounds.Bottom) - Math.Max(bounds.Top, _target.Bounds.Top));
        if ((long)intersectionWidth * intersectionHeight >= (long)width * height / 2) return;

        width = Math.Min(width, _target.WorkArea.Width);
        height = Math.Min(height, _target.WorkArea.Height);
        var x = Math.Clamp(cursor.X - width / 2, _target.WorkArea.Left,
                           _target.WorkArea.Right - width);
        var y = Math.Clamp(cursor.Y - 32, _target.WorkArea.Top,
                           _target.WorkArea.Bottom - height);
        SetWindowPos(window, nint.Zero, x, y, width, height,
            SwpNoZOrder | SwpNoActivate | SwpShowWindow);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Point { public int X; public int Y; }

    [StructLayout(LayoutKind.Sequential)]
    private struct Rect { public int Left; public int Top; public int Right; public int Bottom; }

    private delegate void WinEventProc(nint hook, uint eventType, nint window,
        int objectId, int childId, uint eventThread, uint eventTime);

    [DllImport("user32.dll")]
    private static extern nint SetWinEventHook(uint eventMin, uint eventMax, nint module,
        WinEventProc callback, uint processId, uint threadId, uint flags);

    [DllImport("user32.dll")]
    private static extern bool UnhookWinEvent(nint hook);

    [DllImport("user32.dll")]
    private static extern bool IsWindow(nint window);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(nint window);

    [DllImport("user32.dll")]
    private static extern nint GetAncestor(nint window, uint flags);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(nint window, out uint processId);

    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
    private static extern nint GetWindowLongPtr(nint window, int index);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(nint window, StringBuilder className, int maximum);

    [DllImport("user32.dll")]
    private static extern bool GetCursorPos(out Point point);

    [DllImport("user32.dll")]
    private static extern bool GetWindowRect(nint window, out Rect rect);

    [DllImport("user32.dll")]
    private static extern bool SetWindowPos(nint window, nint insertAfter,
        int x, int y, int width, int height, uint flags);
}
