import CoreVideo
import Foundation
import QuartzCore
import UIKit

final class ReceiverModel: ObservableObject {
    @Published private(set) var status = "Starting…"
    @Published private(set) var streamDescription = "2420 × 1668 • target 120 Hz"
    @Published private(set) var decodedFPS = 0

    let mailbox = FrameMailbox()
    private let receiver = NetworkReceiver()
    private lazy var decoder = H264Decoder { [weak self] in self?.didDecode($0) }
    private var frameCount = 0
    private var reportStart = CACurrentMediaTime()

    func start() {
        UIApplication.shared.isIdleTimerDisabled = true
        receiver.onStatus = { [weak self] text in
            DispatchQueue.main.async { self?.status = text }
        }
        receiver.onPacket = { [weak self] packet in self?.handle(packet) }
        decoder.onError = { [weak self] text in
            DispatchQueue.main.async { self?.status = text }
        }
        receiver.start()
    }

    func stop() {
        receiver.stop()
        decoder.reset()
    }

    func sendPointer(_ event: PointerEvent) {
        receiver.send(type: .pointer, payload: event.encode(), timestampNs: event.timestampNs)
    }

    private func handle(_ packet: WirePacket) {
        switch packet.header.type {
        case .videoConfig:
            guard let config = WireVideoConfig(data: packet.payload),
                  config.codec == 1, config.pixelFormat == 1 else {
                DispatchQueue.main.async { self.status = "Unsupported video format" }
                return
            }
            decoder.reset()
            DispatchQueue.main.async {
                self.streamDescription = "\(config.width) × \(config.height) • \(config.refreshHz) Hz • \(config.bitrate / 1_000_000) Mb/s"
            }
        case .videoFrame:
            decoder.decode(accessUnit: packet.payload,
                           timestampNs: packet.header.timestampNs,
                           keyframe: packet.header.flags.contains(.keyframe))
        case .ping:
            receiver.send(type: .pong, payload: Data(), timestampNs: packet.header.timestampNs)
        default:
            break
        }
    }

    private func didDecode(_ pixelBuffer: CVPixelBuffer) {
        mailbox.publish(pixelBuffer)
        frameCount += 1
        let now = CACurrentMediaTime()
        let elapsed = now - reportStart
        if elapsed >= 0.5 {
            let fps = Int((Double(frameCount) / elapsed).rounded())
            frameCount = 0
            reportStart = now
            DispatchQueue.main.async {
                self.decodedFPS = fps
                self.status = "Streaming"
            }
        }
    }
}
