import CoreVideo
import Foundation

final class FrameMailbox {
    private let lock = NSLock()
    private var latest: CVPixelBuffer?

    func publish(_ pixelBuffer: CVPixelBuffer) {
        lock.lock()
        latest = pixelBuffer
        lock.unlock()
    }

    func takeLatest() -> CVPixelBuffer? {
        lock.lock()
        defer { lock.unlock() }
        let frame = latest
        latest = nil
        return frame
    }
}

