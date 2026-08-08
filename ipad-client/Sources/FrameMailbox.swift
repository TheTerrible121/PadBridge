import CoreVideo
import Foundation

final class FrameMailbox {
    private let lock = NSLock()
    private var latest: CVPixelBuffer?
    private var frameAvailableHandler: (() -> Void)?
    private var requestedFrameRate = 120

    func publish(_ pixelBuffer: CVPixelBuffer) {
        let handler: (() -> Void)?
        lock.lock()
        latest = pixelBuffer
        handler = frameAvailableHandler
        lock.unlock()
        handler?()
    }

    func takeLatest() -> CVPixelBuffer? {
        lock.lock()
        defer { lock.unlock() }
        let frame = latest
        latest = nil
        return frame
    }

    func setFrameAvailableHandler(_ handler: (() -> Void)?) {
        lock.lock()
        frameAvailableHandler = handler
        lock.unlock()
    }

    func setRequestedFrameRate(_ frameRate: Int) {
        lock.lock()
        requestedFrameRate = max(1, min(120, frameRate))
        lock.unlock()
    }

    func requestedFramesPerSecond() -> Int {
        lock.lock()
        defer { lock.unlock() }
        return requestedFrameRate
    }
}
