using System.Threading;
using System.Windows;

namespace PadBridge.Controller;

public partial class App : System.Windows.Application
{
    private Mutex? _singleInstance;

    protected override void OnStartup(StartupEventArgs e)
    {
        _singleInstance = new Mutex(true, "Local\\PadBridge.Controller", out var created);
        if (!created)
        {
            _singleInstance.Dispose();
            _singleInstance = null;
            System.Windows.MessageBox.Show("PadBridge is already running in the system tray.",
                "PadBridge", MessageBoxButton.OK, MessageBoxImage.Information);
            Shutdown();
            return;
        }

        base.OnStartup(e);
        var background = e.Args.Any(arg => arg.Equals("--background", StringComparison.OrdinalIgnoreCase));
        var window = new MainWindow(background);
        MainWindow = window;
        window.Show();
        if (background) window.Hide();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        try { _singleInstance?.ReleaseMutex(); }
        catch (ApplicationException) { }
        _singleInstance?.Dispose();
        base.OnExit(e);
    }
}
