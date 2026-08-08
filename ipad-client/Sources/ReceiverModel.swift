import CoreVideo
import Foundation
import QuartzCore
import UIKit

final class ReceiverModel: ObservableObject {
    @Published private(set) var status = "Starting PadBridge…"
    @Published private(set) var streamDescription = "2420 × 1668 • target 120 Hz"
    @Published private(set) var decodedFPS = 0
    @Published private(set) var isConnected = false
    @Published private(set) var isStreaming = false

    let mailbox = FrameMailbox()
    private let receiver = NetworkReceiver()
    private let decodedQueue = DispatchQueue(label: "dev.padbridge.decoded-fps",
                                              qos: .userInteractive)
    private lazy var decoder = H264Decoder { [weak self] pixelBuffer in
        self?.decodedQueue.async { [weak self] in self?.didDecode(pixelBuffer) }
    }
    private var frameCount = 0
    private var reportStart = CACurrentMediaTime()
    private var configuredFPS = 120
    private var started = false

    func start() {
        guard !started else { return }
        started = true
        receiver.onStatus = { [weak self] text in
            DispatchQueue.main.async { self?.status = text }
        }
        receiver.onPacket = { [weak self] packet in self?.handle(packet) }
        receiver.onConnectionChanged = { [weak self] connected in
            DispatchQueue.main.async {
                self?.isConnected = connected
                if !connected {
                    self?.isStreaming = false
                    self?.decodedFPS = 0
                    UIApplication.shared.isIdleTimerDisabled = false
                }
            }
        }
        decoder.onError = { [weak self] text in
            DispatchQueue.main.async { self?.status = text }
        }
        receiver.start()
    }

    func stop() {
        guard started else { return }
        started = false
        receiver.stop()
        decoder.reset()
        UIApplication.shared.isIdleTimerDisabled = false
        DispatchQueue.main.async {
            self.isConnected = false
            self.isStreaming = false
            self.decodedFPS = 0
        }
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
            mailbox.setRequestedFrameRate(Int(config.refreshHz))
            decodedQueue.async { [weak self] in
                self?.configuredFPS = max(1, Int(config.refreshHz))
                self?.frameCount = 0
                self?.reportStart = CACurrentMediaTime()
            }
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
        if elapsed >= 1.0 {
            // VideoToolbox may invoke its asynchronous callback concurrently.
            // This method is serialized on decodedQueue, and the longer sample
            // window avoids a misleading 70-250 fps counter at a real 120 fps.
            let measured = Int((Double(frameCount) / elapsed).rounded())
            let fps = min(configuredFPS, measured)
            frameCount = 0
            reportStart = now
            DispatchQueue.main.async {
                self.decodedFPS = fps
                self.status = "Streaming"
                self.isStreaming = true
                UIApplication.shared.isIdleTimerDisabled = true
            }
        }
    }
}
