using System.Buffers.Binary;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;

namespace PadBridge.Controller.Services;

public sealed record DiscoveredPadBridge(string Name, IPAddress Address, int Port);

/// <summary>Dependency-free Bonjour/mDNS discovery for _padbridge._tcp.local.</summary>
public sealed class MdnsDiscovery
{
    private const string ServiceName = "_padbridge._tcp.local";
    private static readonly IPEndPoint MulticastEndpoint =
        new(IPAddress.Parse("224.0.0.251"), 5353);

    public async Task<DiscoveredPadBridge?> DiscoverAsync(
        TimeSpan timeout, Action<string> log, CancellationToken cancellationToken)
    {
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        linked.CancelAfter(timeout);
        using var udp = CreateClient();

        var instances = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var services = new Dictionary<string, (string Target, int Port)>(
            StringComparer.OrdinalIgnoreCase);
        var addresses = new Dictionary<string, IPAddress>(StringComparer.OrdinalIgnoreCase);
        var sawPadBridge = false;

        log("Looking for PadBridge on local Wi-Fi…");
        await udp.SendAsync(BuildQuery(), MulticastEndpoint, linked.Token);
        var resendAt = DateTime.UtcNow.AddSeconds(1);

        while (!linked.IsCancellationRequested)
        {
            try
            {
                var receiveTask = udp.ReceiveAsync(linked.Token).AsTask();
                var completed = await Task.WhenAny(receiveTask,
                    Task.Delay(250, linked.Token));
                if (completed != receiveTask)
                {
                    if (DateTime.UtcNow >= resendAt)
                    {
                        await udp.SendAsync(BuildQuery(), MulticastEndpoint, linked.Token);
                        resendAt = DateTime.UtcNow.AddSeconds(1);
                    }
                    continue;
                }

                var packet = await receiveTask;
                foreach (var record in ParseRecords(packet.Buffer))
                {
                    sawPadBridge |= record.Name.Contains("_padbridge._tcp",
                        StringComparison.OrdinalIgnoreCase);
                    switch (record)
                    {
                        case PointerRecord pointer when pointer.Name.Equals(
                            ServiceName, StringComparison.OrdinalIgnoreCase):
                            instances.Add(pointer.Target);
                            break;
                        case ServiceRecord service:
                            services[service.Name] = (service.Target, service.Port);
                            break;
                        case AddressRecord address when IsUsable(address.Address):
                            addresses[address.Name] = address.Address;
                            break;
                    }
                }

                foreach (var instance in instances)
                {
                    if (services.TryGetValue(instance, out var service) &&
                        addresses.TryGetValue(service.Target, out var address))
                    {
                        return new DiscoveredPadBridge(instance, address, service.Port);
                    }
                }

                // Some Apple mDNS responses split PTR, SRV, and A records
                // across packets. If the packet clearly belongs to PadBridge,
                // the only private A record and advertised port are safe.
                if (sawPadBridge && addresses.Count == 1 && services.Count > 0)
                {
                    var service = services.Values.First();
                    return new DiscoveredPadBridge("PadBridge iPad",
                        addresses.Values.First(), service.Port);
                }
            }
            catch (OperationCanceledException) { break; }
            catch (SocketException) when (linked.IsCancellationRequested) { break; }
            catch (InvalidDataException) { }
        }
        return null;
    }

    private static UdpClient CreateClient()
    {
        var udp = new UdpClient(AddressFamily.InterNetwork);
        udp.Client.ExclusiveAddressUse = false;
        udp.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
        try
        {
            udp.Client.Bind(new IPEndPoint(IPAddress.Any, 5353));
            udp.JoinMulticastGroup(MulticastEndpoint.Address);
        }
        catch (SocketException)
        {
            udp.Client.Bind(new IPEndPoint(IPAddress.Any, 0));
        }
        return udp;
    }

    private static byte[] BuildQuery()
    {
        using var stream = new MemoryStream();
        Span<byte> header = stackalloc byte[12];
        BinaryPrimitives.WriteUInt16BigEndian(header.Slice(4, 2), 1); // one question
        stream.Write(header);
        WriteName(stream, ServiceName);
        Span<byte> question = stackalloc byte[4];
        BinaryPrimitives.WriteUInt16BigEndian(question.Slice(0, 2), 12); // PTR
        BinaryPrimitives.WriteUInt16BigEndian(question.Slice(2, 2), 0x8001); // IN + unicast preferred
        stream.Write(question);
        return stream.ToArray();
    }

    private static void WriteName(Stream stream, string name)
    {
        foreach (var label in name.Split('.'))
        {
            var bytes = Encoding.ASCII.GetBytes(label);
            stream.WriteByte(checked((byte)bytes.Length));
            stream.Write(bytes);
        }
        stream.WriteByte(0);
    }

    private static IReadOnlyList<DnsRecord> ParseRecords(byte[] packet)
    {
        if (packet.Length < 12) throw new InvalidDataException("Short DNS packet.");
        var questionCount = ReadUInt16(packet, 4);
        var answerCount = ReadUInt16(packet, 6);
        var authorityCount = ReadUInt16(packet, 8);
        var additionalCount = ReadUInt16(packet, 10);
        var offset = 12;

        for (var index = 0; index < questionCount; index++)
        {
            _ = ReadName(packet, ref offset);
            Require(packet, offset, 4);
            offset += 4;
        }

        var records = new List<DnsRecord>();
        var total = answerCount + authorityCount + additionalCount;
        for (var index = 0; index < total; index++)
        {
            var name = ReadName(packet, ref offset);
            Require(packet, offset, 10);
            var type = ReadUInt16(packet, offset);
            var length = ReadUInt16(packet, offset + 8);
            offset += 10;
            Require(packet, offset, length);
            var dataOffset = offset;
            var dataEnd = offset + length;

            if (type == 1 && length == 4)
            {
                records.Add(new AddressRecord(name,
                    new IPAddress(packet.AsSpan(dataOffset, 4))));
            }
            else if (type == 12)
            {
                var pointerOffset = dataOffset;
                records.Add(new PointerRecord(name, ReadName(packet, ref pointerOffset)));
            }
            else if (type == 33 && length >= 7)
            {
                var port = ReadUInt16(packet, dataOffset + 4);
                var targetOffset = dataOffset + 6;
                records.Add(new ServiceRecord(name, ReadName(packet, ref targetOffset), port));
            }

            offset = dataEnd;
        }
        return records;
    }

    private static string ReadName(byte[] packet, ref int offset)
    {
        var labels = new List<string>();
        var cursor = offset;
        var jumped = false;
        var jumps = 0;
        while (true)
        {
            Require(packet, cursor, 1);
            var length = packet[cursor++];
            if (length == 0)
            {
                if (!jumped) offset = cursor;
                break;
            }
            if ((length & 0xC0) == 0xC0)
            {
                Require(packet, cursor, 1);
                var pointer = ((length & 0x3F) << 8) | packet[cursor++];
                if (!jumped) offset = cursor;
                cursor = pointer;
                jumped = true;
                if (++jumps > 16) throw new InvalidDataException("DNS compression loop.");
                continue;
            }
            if (length > 63) throw new InvalidDataException("Invalid DNS label.");
            Require(packet, cursor, length);
            labels.Add(Encoding.UTF8.GetString(packet, cursor, length));
            cursor += length;
            if (!jumped) offset = cursor;
        }
        return string.Join('.', labels);
    }

    private static ushort ReadUInt16(byte[] bytes, int offset)
    {
        Require(bytes, offset, 2);
        return BinaryPrimitives.ReadUInt16BigEndian(bytes.AsSpan(offset, 2));
    }

    private static void Require(byte[] bytes, int offset, int amount)
    {
        if (offset < 0 || amount < 0 || offset + amount > bytes.Length)
            throw new InvalidDataException("Malformed DNS packet.");
    }

    private static bool IsUsable(IPAddress address)
    {
        if (address.AddressFamily != AddressFamily.InterNetwork ||
            IPAddress.IsLoopback(address)) return false;
        var bytes = address.GetAddressBytes();
        return bytes[0] == 10 || bytes[0] == 192 && bytes[1] == 168 ||
               bytes[0] == 172 && bytes[1] is >= 16 and <= 31 ||
               bytes[0] == 169 && bytes[1] == 254;
    }

    private abstract record DnsRecord(string Name);
    private sealed record PointerRecord(string RecordName, string Target) : DnsRecord(RecordName);
    private sealed record ServiceRecord(string RecordName, string Target, int Port) : DnsRecord(RecordName);
    private sealed record AddressRecord(string RecordName, IPAddress Address) : DnsRecord(RecordName);
}
