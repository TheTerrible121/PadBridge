using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Xml;
using System.Xml.Linq;

namespace PadBridge.Controller.Services;

public sealed record AppleMuxDevice(uint DeviceId, string SerialNumber, string ConnectionType);

/// <summary>
/// A small native replacement for iproxy. It talks to Apple Mobile Device
/// Service on loopback:27015 and forwards a local TCP port to the same port in
/// the PadBridge iPad app. No Python or libimobiledevice install is required.
/// </summary>
public sealed class UsbMuxForwarder : IAsyncDisposable
{
    private const int AppleMuxPort = 27015;
    private const uint PlistProtocolVersion = 1;
    private const uint PlistMessage = 8;
    private const int MaximumPacketSize = 16 * 1024 * 1024;

    private readonly int _localPort;
    private readonly int _devicePort;
    private readonly Action<string> _log;
    private readonly CancellationTokenSource _lifetime = new();
    private TcpListener? _listener;
    private Task? _acceptLoop;
    private int _tag;
    private DateTime _lastForwardError = DateTime.MinValue;

    public UsbMuxForwarder(int localPort, int devicePort, Action<string> log)
    {
        _localPort = localPort;
        _devicePort = devicePort;
        _log = log;
    }

    public void Start()
    {
        if (_listener is not null) return;
        _listener = new TcpListener(IPAddress.Loopback, _localPort);
        try
        {
            _listener.Start(4);
        }
        catch (SocketException exception) when (exception.SocketErrorCode == SocketError.AddressAlreadyInUse)
        {
            _listener = null;
            throw new InvalidOperationException(
                "USB port 52100 is already in use. Close the old iproxy/pymobiledevice3 " +
                "PowerShell window once, then press Connect again.", exception);
        }
        _acceptLoop = AcceptLoopAsync(_lifetime.Token);
        _log($"USB bridge listening on 127.0.0.1:{_localPort}.");
    }

    public async Task<AppleMuxDevice?> FindUsbDeviceAsync(CancellationToken cancellationToken)
    {
        try
        {
            return (await ListDevicesAsync(cancellationToken))
                .FirstOrDefault(device => device.ConnectionType.Equals(
                    "USB", StringComparison.OrdinalIgnoreCase));
        }
        catch (Exception exception) when (exception is not OperationCanceledException)
        {
            return null;
        }
    }

    /// <summary>
    /// Checks whether the foreground PadBridge iPad app is accepting its USB
    /// port. The successful probe is immediately closed; the real host takes
    /// over the connection when the session starts.
    /// </summary>
    public async Task<bool> IsDevicePortOpenAsync(CancellationToken cancellationToken)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(TimeSpan.FromMilliseconds(1200));
        try
        {
            var device = await FindUsbDeviceAsync(timeout.Token);
            if (device is null) return false;

            using var upstream = new TcpClient(AddressFamily.InterNetwork) { NoDelay = true };
            await upstream.ConnectAsync(IPAddress.Loopback, AppleMuxPort, timeout.Token);
            var networkOrderPort = BinaryPrimitives.ReverseEndianness((ushort)_devicePort);
            var response = await SendPlistRequestAsync(upstream.GetStream(), BuildConnectPlist(
                device.DeviceId, networkOrderPort), timeout.Token);
            return ReadResultNumber(response) == 0;
        }
        catch (Exception exception) when (exception is not OperationCanceledException ||
                                          !cancellationToken.IsCancellationRequested)
        {
            return false;
        }
    }

    public async ValueTask DisposeAsync()
    {
        _lifetime.Cancel();
        _listener?.Stop();
        if (_acceptLoop is not null)
        {
            try { await _acceptLoop; }
            catch (OperationCanceledException) { }
            catch (ObjectDisposedException) { }
            catch (SocketException) { }
        }
        _listener = null;
        _lifetime.Dispose();
    }

    private async Task AcceptLoopAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            TcpClient local;
            try
            {
                local = await _listener!.AcceptTcpClientAsync(cancellationToken);
            }
            catch (OperationCanceledException) { break; }
            catch (ObjectDisposedException) { break; }
            _ = ForwardConnectionAsync(local, cancellationToken);
        }
    }

    private async Task ForwardConnectionAsync(TcpClient local, CancellationToken cancellationToken)
    {
        using (local)
        {
            try
            {
                local.NoDelay = true;
                var device = await FindUsbDeviceAsync(cancellationToken)
                             ?? throw new IOException(
                                 "No trusted USB iPad was found. Connect and unlock the iPad, then tap Trust.");

                using var upstream = new TcpClient(AddressFamily.InterNetwork) { NoDelay = true };
                await upstream.ConnectAsync(IPAddress.Loopback, AppleMuxPort, cancellationToken);
                var stream = upstream.GetStream();

                var networkOrderPort = BinaryPrimitives.ReverseEndianness((ushort)_devicePort);
                var response = await SendPlistRequestAsync(stream, BuildConnectPlist(
                    device.DeviceId, networkOrderPort), cancellationToken);
                var result = ReadResultNumber(response);
                if (result != 0)
                {
                    throw new IOException($"Apple USB connection failed with usbmux result {result}.");
                }

                _log($"USB connected to iPad {ShortSerial(device.SerialNumber)}.");
                using var linked = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                var localStream = local.GetStream();
                var toDevice = localStream.CopyToAsync(stream, 128 * 1024, linked.Token);
                var fromDevice = stream.CopyToAsync(localStream, 128 * 1024, linked.Token);
                await Task.WhenAny(toDevice, fromDevice);
                linked.Cancel();
                try { await Task.WhenAll(toDevice, fromDevice); }
                catch (OperationCanceledException) { }
                catch (IOException) { }
            }
            catch (OperationCanceledException) { }
            catch (Exception exception)
            {
                ReportForwardError(exception.Message);
            }
        }
    }

    private async Task<IReadOnlyList<AppleMuxDevice>> ListDevicesAsync(
        CancellationToken cancellationToken)
    {
        using var client = new TcpClient(AddressFamily.InterNetwork) { NoDelay = true };
        await client.ConnectAsync(IPAddress.Loopback, AppleMuxPort, cancellationToken);
        var packet = await SendPlistRequestAsync(client.GetStream(), BuildListDevicesPlist(),
                                                 cancellationToken);
        return ParseDeviceList(packet.Payload);
    }

    private async Task<MuxPacket> SendPlistRequestAsync(
        NetworkStream stream, string xml, CancellationToken cancellationToken)
    {
        var payload = Encoding.UTF8.GetBytes(xml);
        var tag = unchecked((uint)Interlocked.Increment(ref _tag));
        var header = new byte[16];
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(0, 4),
                                                 checked((uint)(header.Length + payload.Length)));
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(4, 4), PlistProtocolVersion);
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(8, 4), PlistMessage);
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(12, 4), tag);
        await stream.WriteAsync(header, cancellationToken);
        await stream.WriteAsync(payload, cancellationToken);
        await stream.FlushAsync(cancellationToken);
        return await ReadPacketAsync(stream, cancellationToken);
    }

    private static async Task<MuxPacket> ReadPacketAsync(
        NetworkStream stream, CancellationToken cancellationToken)
    {
        var header = new byte[16];
        await stream.ReadExactlyAsync(header, cancellationToken);
        var length = BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(0, 4));
        if (length < 16 || length > MaximumPacketSize)
            throw new IOException($"Invalid Apple USB packet length: {length}.");
        var payload = new byte[checked((int)length - 16)];
        await stream.ReadExactlyAsync(payload, cancellationToken);
        return new MuxPacket(
            BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(4, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(8, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(12, 4)),
            payload);
    }

    private static IReadOnlyList<AppleMuxDevice> ParseDeviceList(byte[] payload)
    {
        var document = ParsePlist(payload);
        var root = document.Root?.Elements().FirstOrDefault(e => e.Name.LocalName == "dict");
        var array = root is null ? null : FindDictionaryValue(root, "DeviceList");
        if (array is null || array.Name.LocalName != "array")
            return Array.Empty<AppleMuxDevice>();

        var devices = new List<AppleMuxDevice>();
        foreach (var dictionary in array.Elements().Where(e => e.Name.LocalName == "dict"))
        {
            var idNode = FindDictionaryValue(dictionary, "DeviceID");
            var properties = FindDictionaryValue(dictionary, "Properties");
            if (idNode is null || properties is null ||
                !uint.TryParse(idNode.Value, out var deviceId)) continue;
            var serial = FindDictionaryValue(properties, "SerialNumber")?.Value ?? "iPad";
            var connection = FindDictionaryValue(properties, "ConnectionType")?.Value ?? "Unknown";
            devices.Add(new AppleMuxDevice(deviceId, serial, connection));
        }
        return devices;
    }

    private static uint ReadResultNumber(MuxPacket packet)
    {
        // Older usbmux implementations may return a binary result packet.
        if (packet.Message == 1 && packet.Payload.Length >= 4)
            return BinaryPrimitives.ReadUInt32LittleEndian(packet.Payload.AsSpan(0, 4));

        var document = ParsePlist(packet.Payload);
        var root = document.Root?.Elements().FirstOrDefault(e => e.Name.LocalName == "dict");
        var value = root is null ? null : FindDictionaryValue(root, "Number");
        return value is not null && uint.TryParse(value.Value, out var result)
            ? result
            : uint.MaxValue;
    }

    private static XDocument ParsePlist(byte[] payload)
    {
        using var text = new StringReader(Encoding.UTF8.GetString(payload));
        using var reader = XmlReader.Create(text, new XmlReaderSettings
        {
            DtdProcessing = DtdProcessing.Ignore,
            XmlResolver = null
        });
        return XDocument.Load(reader, LoadOptions.None);
    }

    private static XElement? FindDictionaryValue(XElement dictionary, string key)
    {
        var children = dictionary.Elements().ToList();
        for (var index = 0; index + 1 < children.Count; index++)
        {
            if (children[index].Name.LocalName == "key" && children[index].Value == key)
                return children[index + 1];
        }
        return null;
    }

    private static string BuildListDevicesPlist() => BuildPlist("ListDevices", "");

    private static string BuildConnectPlist(uint deviceId, ushort networkOrderPort) =>
        BuildPlist("Connect",
            $"<key>DeviceID</key><integer>{deviceId}</integer>" +
            $"<key>PortNumber</key><integer>{networkOrderPort}</integer>");

    private static string BuildPlist(string messageType, string additionalValues) =>
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" +
        "<plist version=\"1.0\"><dict>" +
        "<key>BundleID</key><string>dev.padbridge.controller</string>" +
        "<key>ClientVersionString</key><string>PadBridge 1.0</string>" +
        $"<key>MessageType</key><string>{messageType}</string>" +
        "<key>ProgName</key><string>PadBridge</string>" +
        "<key>kLibUSBMuxVersion</key><integer>3</integer>" +
        additionalValues + "</dict></plist>";

    private void ReportForwardError(string message)
    {
        var now = DateTime.UtcNow;
        if (now - _lastForwardError < TimeSpan.FromSeconds(3)) return;
        _lastForwardError = now;
        _log(message);
    }

    private static string ShortSerial(string serial) =>
        serial.Length <= 8 ? serial : $"…{serial[^6..]}";

    private sealed record MuxPacket(uint Version, uint Message, uint Tag, byte[] Payload);
}
