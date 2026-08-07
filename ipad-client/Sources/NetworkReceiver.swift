import Foundation
import Network

final class NetworkReceiver {
    var onPacket: ((WirePacket) -> Void)?
    var onStatus: ((String) -> Void)?

    private let queue = DispatchQueue(label: "dev.padbridge.network", qos: .userInteractive)
    private let parser = WireParser()
    private var listener: NWListener?
    private var connection: NWConnection?
    private var outgoingSequence: UInt32 = 0

    func start(port: UInt16 = 52100) {
        queue.async { [weak self] in self?.startOnQueue(port: port) }
    }

    func stop() {
        queue.async { [weak self] in
            self?.connection?.cancel()
            self?.listener?.cancel()
            self?.connection = nil
            self?.listener = nil
        }
    }

    func send(type: WireMessageType, payload: Data, timestampNs: UInt64) {
        queue.async { [weak self] in
            guard let self, let connection = self.connection else { return }
            let header = WireHeader(type: type, payloadLength: UInt32(payload.count),
                                    sequence: self.outgoingSequence, timestampNs: timestampNs)
            self.outgoingSequence &+= 1
            var message = header.encode()
            message.append(payload)
            connection.send(content: message, completion: .contentProcessed { [weak self] error in
                if let error { self?.onStatus?("Send error: \(error.localizedDescription)") }
            })
        }
    }

    private func startOnQueue(port: UInt16) {
        do {
            let tcp = NWProtocolTCP.Options()
            tcp.noDelay = true
            tcp.enableKeepalive = true
            let parameters = NWParameters(tls: nil, tcp: tcp)
            parameters.allowLocalEndpointReuse = true
            guard let endpointPort = NWEndpoint.Port(rawValue: port) else {
                onStatus?("Invalid listening port")
                return
            }
            let listener = try NWListener(using: parameters, on: endpointPort)
            listener.service = NWListener.Service(type: "_padbridge._tcp")
            listener.stateUpdateHandler = { [weak self] state in
                switch state {
                case .ready: self?.onStatus?("Ready — connect USB or Wi-Fi")
                case .failed(let error): self?.onStatus?("Listener failed: \(error.localizedDescription)")
                case .cancelled: self?.onStatus?("Stopped")
                default: break
                }
            }
            listener.newConnectionHandler = { [weak self] in self?.accept($0) }
            self.listener = listener
            listener.start(queue: queue)
        } catch {
            onStatus?("Could not listen: \(error.localizedDescription)")
        }
    }

    private func accept(_ newConnection: NWConnection) {
        connection?.cancel()
        parser.reset()
        connection = newConnection
        newConnection.stateUpdateHandler = { [weak self, weak newConnection] state in
            switch state {
            case .ready:
                self?.onStatus?("Windows connected")
                if let newConnection { self?.receive(on: newConnection) }
            case .failed(let error):
                self?.onStatus?("Connection failed: \(error.localizedDescription)")
            case .cancelled:
                self?.onStatus?("Disconnected — waiting")
            default: break
            }
        }
        newConnection.start(queue: queue)
    }

    private func receive(on connection: NWConnection) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 256 * 1024) {
            [weak self, weak connection] content, _, isComplete, error in
            guard let self, let connection else { return }
            if let content, !content.isEmpty {
                do {
                    for packet in try self.parser.append(content) { self.onPacket?(packet) }
                } catch {
                    self.onStatus?("Protocol error — reconnecting")
                    connection.cancel()
                    return
                }
            }
            if let error {
                self.onStatus?("Receive error: \(error.localizedDescription)")
                connection.cancel()
            } else if isComplete {
                connection.cancel()
            } else {
                self.receive(on: connection)
            }
        }
    }
}

