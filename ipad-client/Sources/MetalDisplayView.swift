import CoreVideo
import MetalKit
import QuartzCore
import SwiftUI
import UIKit

struct MetalDisplayView: UIViewRepresentable {
    let mailbox: FrameMailbox
    let onPointer: (PointerEvent) -> Void

    func makeUIView(context: Context) -> PadBridgeMetalView {
        let view = PadBridgeMetalView(mailbox: mailbox)
        view.onPointer = onPointer
        return view
    }

    func updateUIView(_ view: PadBridgeMetalView, context: Context) {
        view.onPointer = onPointer
    }
}

final class PadBridgeMetalView: MTKView, MTKViewDelegate {
    var onPointer: ((PointerEvent) -> Void)?

    private let mailbox: FrameMailbox
    private let commandQueue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState
    private var textureCache: CVMetalTextureCache!
    private var touchIDs: [ObjectIdentifier: UInt32] = [:]
    private var nextTouchID: UInt32 = 1
    private var videoSize = CGSize.zero

    init(mailbox: FrameMailbox) {
        guard let device = MTLCreateSystemDefaultDevice(),
              let commandQueue = device.makeCommandQueue(),
              let library = device.makeDefaultLibrary(),
              let vertex = library.makeFunction(name: "padbridgeVertex"),
              let fragment = library.makeFunction(name: "padbridgeNV12Fragment") else {
            fatalError("Metal is unavailable")
        }
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = vertex
        descriptor.fragmentFunction = fragment
        descriptor.colorAttachments[0].pixelFormat = .bgra8Unorm
        guard let pipeline = try? device.makeRenderPipelineState(descriptor: descriptor) else {
            fatalError("Could not create Metal video pipeline")
        }
        self.mailbox = mailbox
        self.commandQueue = commandQueue
        self.pipeline = pipeline
        super.init(frame: .zero, device: device)

        colorPixelFormat = .bgra8Unorm
        clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        framebufferOnly = true
        isPaused = false
        enableSetNeedsDisplay = false
        preferredFramesPerSecond = min(120, UIScreen.main.maximumFramesPerSecond)
        contentScaleFactor = UIScreen.main.nativeScale
        autoResizeDrawable = true
        isMultipleTouchEnabled = true
        isOpaque = true
        backgroundColor = .black
        delegate = self
        if let metalLayer = layer as? CAMetalLayer {
            // On iPad, MTKView's display link is the public VSync clock.
            // Two drawables keeps presentation synchronized without queuing
            // the default three frames of work.
            metalLayer.presentsWithTransaction = false
            metalLayer.maximumDrawableCount = 2
        }
        CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, device, nil, &textureCache)
    }

    required init(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        autoreleasepool { drawLatestFrame(in: view) }
    }

    private func drawLatestFrame(in view: MTKView) {
        guard let pixelBuffer = mailbox.takeLatest(),
              CVPixelBufferGetPlaneCount(pixelBuffer) == 2 else { return }

        videoSize = CGSize(width: CVPixelBufferGetWidth(pixelBuffer),
                           height: CVPixelBufferGetHeight(pixelBuffer))

        guard let descriptor = currentRenderPassDescriptor,
              let drawable = currentDrawable else { return }
        descriptor.colorAttachments[0].loadAction = .clear
        descriptor.colorAttachments[0].storeAction = .store
        descriptor.colorAttachments[0].clearColor = clearColor

        var lumaReference: CVMetalTexture?
        var chromaReference: CVMetalTexture?
        let lumaWidth = CVPixelBufferGetWidthOfPlane(pixelBuffer, 0)
        let lumaHeight = CVPixelBufferGetHeightOfPlane(pixelBuffer, 0)
        let chromaWidth = CVPixelBufferGetWidthOfPlane(pixelBuffer, 1)
        let chromaHeight = CVPixelBufferGetHeightOfPlane(pixelBuffer, 1)
        CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, textureCache, pixelBuffer, nil, .r8Unorm,
            lumaWidth, lumaHeight, 0, &lumaReference)
        CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, textureCache, pixelBuffer, nil, .rg8Unorm,
            chromaWidth, chromaHeight, 1, &chromaReference)
        guard let lumaReference, let chromaReference,
              let luma = CVMetalTextureGetTexture(lumaReference),
              let chroma = CVMetalTextureGetTexture(chromaReference),
              let commandBuffer = commandQueue.makeCommandBuffer(),
              let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: descriptor) else {
            return
        }

        encoder.setRenderPipelineState(pipeline)
        encoder.setViewport(aspectFitViewport(video: videoSize, drawable: view.drawableSize))
        encoder.setFragmentTexture(luma, index: 0)
        encoder.setFragmentTexture(chroma, index: 1)
        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        encoder.endEncoding()
        commandBuffer.present(drawable)
        commandBuffer.commit()
    }

    private func aspectFitViewport(video: CGSize, drawable: CGSize) -> MTLViewport {
        guard video.width > 0, video.height > 0,
              drawable.width > 0, drawable.height > 0 else {
            return MTLViewport(originX: 0, originY: 0, width: drawable.width,
                               height: drawable.height, znear: 0, zfar: 1)
        }
        let scale = min(drawable.width / video.width, drawable.height / video.height)
        let width = video.width * scale
        let height = video.height * scale
        return MTLViewport(originX: (drawable.width - width) * 0.5,
                           originY: (drawable.height - height) * 0.5,
                           width: width, height: height, znear: 0, zfar: 1)
    }

    private func videoRectInView() -> CGRect {
        guard videoSize.width > 0, videoSize.height > 0,
              bounds.width > 0, bounds.height > 0 else { return bounds }
        let scale = min(bounds.width / videoSize.width, bounds.height / videoSize.height)
        let size = CGSize(width: videoSize.width * scale, height: videoSize.height * scale)
        return CGRect(x: (bounds.width - size.width) * 0.5,
                      y: (bounds.height - size.height) * 0.5,
                      width: size.width, height: size.height)
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        emit(touches, phase: .down, event: event)
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        emit(touches, phase: .move, event: event)
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        emit(touches, phase: .up, event: event)
        touches.forEach { touchIDs.removeValue(forKey: ObjectIdentifier($0)) }
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        emit(touches, phase: .cancel, event: event)
        touches.forEach { touchIDs.removeValue(forKey: ObjectIdentifier($0)) }
    }

    private func emit(_ touches: Set<UITouch>, phase: PointerEvent.Phase, event: UIEvent?) {
        for touch in touches {
            let videoRect = videoRectInView()
            if phase == .down && !videoRect.contains(touch.location(in: self)) { continue }

            let key = ObjectIdentifier(touch)
            let id: UInt32
            if let existing = touchIDs[key] {
                id = existing
            } else {
                id = nextTouchID
                nextTouchID &+= 1
                touchIDs[key] = id
            }
            let samples = event?.coalescedTouches(for: touch) ?? [touch]
            for sample in samples { emit(sample, id: id, phase: phase, videoRect: videoRect) }
        }
    }

    private func emit(_ touch: UITouch, id: UInt32, phase: PointerEvent.Phase,
                      videoRect: CGRect) {
        let location = touch.location(in: self)
        let normalizedX = Float(max(0, min(1,
            (location.x - videoRect.minX) / max(videoRect.width, 1))))
        let normalizedY = Float(max(0, min(1,
            (location.y - videoRect.minY) / max(videoRect.height, 1))))
        let isPencil = touch.type == .pencil
        let pressure = isPencil && touch.maximumPossibleForce > 0
            ? Float(max(0, min(1, touch.force / touch.maximumPossibleForce))) : 0.5
        let tilt = max(0, (.pi / 2) - touch.altitudeAngle)
        let azimuth = touch.azimuthAngle(in: self)
        onPointer?(PointerEvent(
            id: id, phase: phase, tool: isPencil ? .pencil : .finger,
            x: normalizedX, y: normalizedY, pressure: pressure,
            tiltX: Float(tilt * cos(azimuth)), tiltY: Float(tilt * sin(azimuth)),
            timestampNs: UInt64(max(0, touch.timestamp) * 1_000_000_000)))
    }
}
