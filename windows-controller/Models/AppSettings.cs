namespace PadBridge.Controller.Models;

public enum ConnectionMode
{
    Auto,
    Usb,
    Wifi
}

public sealed class AppSettings
{
    public ConnectionMode ConnectionMode { get; set; } = ConnectionMode.Auto;
    public string WifiAddress { get; set; } = "";
    public int FramesPerSecond { get; set; } = 120;
    public int UsbBitrateMbps { get; set; } = 60;
    public int WifiBitrateMbps { get; set; } = 35;
    public int AdapterIndex { get; set; } = 1;
    public int CaptureOutputIndex { get; set; } = 0;
    public bool ZeroCopy { get; set; } = true;
    public bool RouteNewWindows { get; set; } = true;
    public bool CollapseDisplayOnStop { get; set; } = true;
    public bool AutoConnect { get; set; } = true;
    public bool StartWithWindows { get; set; }
}
