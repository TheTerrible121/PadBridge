import SwiftUI

@main
struct PadBridgeApp: App {
    @StateObject private var model = ReceiverModel()

    var body: some Scene {
        WindowGroup {
            ZStack(alignment: .topLeading) {
                Color.black.ignoresSafeArea()
                MetalDisplayView(mailbox: model.mailbox, onPointer: model.sendPointer)
                    .ignoresSafeArea()

                VStack(alignment: .leading, spacing: 4) {
                    Text(model.status).font(.headline)
                    Text(model.streamDescription).font(.caption.monospacedDigit())
                    if model.decodedFPS > 0 {
                        Text("Decoded \(model.decodedFPS) fps").font(.caption.monospacedDigit())
                    }
                }
                .foregroundStyle(.white)
                .padding(12)
                .background(.black.opacity(0.58), in: RoundedRectangle(cornerRadius: 10))
                .padding(16)
                .allowsHitTesting(false)
            }
            .persistentSystemOverlays(.hidden)
            .onAppear { model.start() }
        }
    }
}

