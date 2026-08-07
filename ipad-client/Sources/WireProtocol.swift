import Foundation

enum WireMessageType: UInt8 {
    case hello = 1
    case videoConfig = 2
    case videoFrame = 3
    case pointer = 4
    case audioConfig = 5
    case audioFrame = 6
    case ping = 7
    case pong = 8
    case stats = 9
}

struct WireFlags: OptionSet {
    let rawValue: UInt16
    static let keyframe = WireFlags(rawValue: 1 << 0)
    static let discontinuity = WireFlags(rawValue: 1 << 1)
}

struct WireHeader {
    static let size = 24
    static let maximumPayload = 16 * 1024 * 1024
    static let magic: [UInt8] = [0x50, 0x44, 0x42, 0x31]

    let type: WireMessageType
    let flags: WireFlags
    let payloadLength: UInt32
    let sequence: UInt32
    let timestampNs: UInt64

    init?(data: Data) {
        guard data.count == Self.size else { return nil }
        let bytes = [UInt8](data)
        guard Array(bytes[0..<4]) == Self.magic,
              bytes[4] == 1,
              let type = WireMessageType(rawValue: bytes[5]) else { return nil }
        let length = bytes.readUInt32(at: 8)
        guard length <= Self.maximumPayload else { return nil }
        self.type = type
        self.flags = WireFlags(rawValue: bytes.readUInt16(at: 6))
        self.payloadLength = length
        self.sequence = bytes.readUInt32(at: 12)
        self.timestampNs = bytes.readUInt64(at: 16)
    }

    init(type: WireMessageType, flags: WireFlags = [], payloadLength: UInt32,
         sequence: UInt32, timestampNs: UInt64) {
        self.type = type
        self.flags = flags
        self.payloadLength = payloadLength
        self.sequence = sequence
        self.timestampNs = timestampNs
    }

    func encode() -> Data {
        var data = Data(Self.magic)
        data.append(1)
        data.append(type.rawValue)
        data.appendBigEndian(flags.rawValue)
        data.appendBigEndian(payloadLength)
        data.appendBigEndian(sequence)
        data.appendBigEndian(timestampNs)
        return data
    }
}

struct WirePacket {
    let header: WireHeader
    let payload: Data
}

struct WireVideoConfig {
    static let size = 16
    let width: UInt16
    let height: UInt16
    let refreshHz: UInt16
    let codec: UInt8
    let pixelFormat: UInt8
    let bitrate: UInt32

    init?(data: Data) {
        guard data.count == Self.size else { return nil }
        let bytes = [UInt8](data)
        width = bytes.readUInt16(at: 0)
        height = bytes.readUInt16(at: 2)
        refreshHz = bytes.readUInt16(at: 4)
        codec = bytes[6]
        pixelFormat = bytes[7]
        bitrate = bytes.readUInt32(at: 8)
    }
}

final class WireParser {
    private var buffer = Data()

    func reset() { buffer.removeAll(keepingCapacity: true) }

    func append(_ incoming: Data) throws -> [WirePacket] {
        buffer.append(incoming)
        var packets: [WirePacket] = []
        while buffer.count >= WireHeader.size {
            let headerData = Data(buffer.prefix(WireHeader.size))
            guard let header = WireHeader(data: headerData) else {
                buffer.removeAll()
                throw WireError.invalidHeader
            }
            let total = WireHeader.size + Int(header.payloadLength)
            guard buffer.count >= total else { break }
            let payload = buffer.subdata(in: WireHeader.size..<total)
            packets.append(WirePacket(header: header, payload: payload))
            buffer.removeSubrange(0..<total)
        }
        return packets
    }
}

enum WireError: Error {
    case invalidHeader
}

private extension Array where Element == UInt8 {
    func readUInt16(at offset: Int) -> UInt16 {
        (UInt16(self[offset]) << 8) | UInt16(self[offset + 1])
    }

    func readUInt32(at offset: Int) -> UInt32 {
        (UInt32(self[offset]) << 24) | (UInt32(self[offset + 1]) << 16) |
        (UInt32(self[offset + 2]) << 8) | UInt32(self[offset + 3])
    }

    func readUInt64(at offset: Int) -> UInt64 {
        var value: UInt64 = 0
        for byte in self[offset..<(offset + 8)] {
            value = (value << 8) | UInt64(byte)
        }
        return value
    }
}

extension Data {
    mutating func appendBigEndian(_ value: UInt16) {
        append(UInt8(truncatingIfNeeded: value >> 8))
        append(UInt8(truncatingIfNeeded: value))
    }

    mutating func appendBigEndian(_ value: UInt32) {
        append(UInt8(truncatingIfNeeded: value >> 24))
        append(UInt8(truncatingIfNeeded: value >> 16))
        append(UInt8(truncatingIfNeeded: value >> 8))
        append(UInt8(truncatingIfNeeded: value))
    }

    mutating func appendBigEndian(_ value: UInt64) {
        for shift in stride(from: 56, through: 0, by: -8) {
            append(UInt8(truncatingIfNeeded: value >> UInt64(shift)))
        }
    }

    mutating func appendBigEndian(_ value: Float) {
        appendBigEndian(value.bitPattern)
    }
}

