import Foundation
import UIKit

struct PointerEvent {
    enum Phase: UInt8 { case down = 0, move = 1, up = 2, cancel = 3 }
    enum Tool: UInt8 { case finger = 0, pencil = 1 }

    let id: UInt32
    let phase: Phase
    let tool: Tool
    let x: Float
    let y: Float
    let pressure: Float
    let tiltX: Float
    let tiltY: Float
    let timestampNs: UInt64

    func encode() -> Data {
        var data = Data()
        data.reserveCapacity(36)
        data.appendBigEndian(id)
        data.append(phase.rawValue)
        data.append(tool.rawValue)
        data.appendBigEndian(UInt16(0))
        data.appendBigEndian(x)
        data.appendBigEndian(y)
        data.appendBigEndian(pressure)
        data.appendBigEndian(tiltX)
        data.appendBigEndian(tiltY)
        data.appendBigEndian(timestampNs)
        return data
    }
}

