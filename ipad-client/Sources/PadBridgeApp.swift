import SwiftUI

@main
struct PadBridgeApp: App {
    @Environment(\.scenePhase) private var scenePhase
    @StateObject private var model = ReceiverModel()

    var body: some Scene {
        WindowGroup {
            ZStack {
                Color.black.ignoresSafeArea()
                MetalDisplayView(mailbox: model.mailbox, onPointer: model.sendPointer)
                    .ignoresSafeArea()

                if !model.isStreaming {
                    WaitingView(model: model)
                        .transition(.opacity)
                        .allowsHitTesting(false)
                }

                if model.isStreaming {
                    StreamingPill(model: model)
                        .frame(maxHeight: .infinity, alignment: .top)
                        .padding(.top, 18)
                        .transition(.move(edge: .top).combined(with: .opacity))
                        .allowsHitTesting(false)
                }
            }
            .animation(.easeOut(duration: 0.28), value: model.isStreaming)
            .persistentSystemOverlays(.hidden)
            .onAppear { model.start() }
            .onChange(of: scenePhase) { _, phase in
                if phase == .active { model.start() }
                else if phase == .background { model.stop() }
            }
        }
    }
}

private struct WaitingView: View {
    @ObservedObject var model: ReceiverModel

    var body: some View {
        VStack(spacing: 0) {
            Image("BrandMark")
                .resizable()
                .scaledToFit()
                .frame(width: 270, height: 160)
                .accessibilityHidden(true)

            Text("PadBridge")
                .font(.system(size: 38, weight: .semibold, design: .rounded))
                .foregroundStyle(.white)
                .padding(.top, 18)

            Text(model.isConnected ? "Windows connected" : "Ready for your Windows PC")
                .font(.system(size: 18, weight: .medium))
                .foregroundStyle(.white.opacity(0.72))
                .padding(.top, 11)

            HStack(spacing: 9) {
                Circle()
                    .fill(model.isConnected ? Color.green : Color.white.opacity(0.48))
                    .frame(width: 8, height: 8)
                Text(model.status)
                    .font(.system(size: 13, weight: .medium, design: .monospaced))
                    .foregroundStyle(.white.opacity(0.58))
            }
            .padding(.horizontal, 15)
            .padding(.vertical, 10)
            .background(.white.opacity(0.06), in: Capsule())
            .overlay(Capsule().stroke(.white.opacity(0.1), lineWidth: 1))
            .padding(.top, 25)

            Text("USB-C preferred  •  Wi-Fi supported  •  120 Hz")
                .font(.system(size: 12, weight: .medium))
                .foregroundStyle(.white.opacity(0.35))
                .padding(.top, 17)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(
            RadialGradient(colors: [.white.opacity(0.035), .clear],
                           center: .center, startRadius: 20, endRadius: 520)
        )
    }
}

private struct StreamingPill: View {
    @ObservedObject var model: ReceiverModel

    var body: some View {
        HStack(spacing: 10) {
            Circle().fill(Color.green).frame(width: 7, height: 7)
            Text("2420 × 1668")
            Rectangle().fill(.white.opacity(0.22)).frame(width: 1, height: 13)
            Text("\(model.decodedFPS) fps")
        }
        .font(.system(size: 12, weight: .semibold, design: .monospaced))
        .foregroundStyle(.white.opacity(0.82))
        .padding(.horizontal, 15)
        .padding(.vertical, 9)
        .background(.black.opacity(0.72), in: Capsule())
        .overlay(Capsule().stroke(.white.opacity(0.12), lineWidth: 1))
    }
}
