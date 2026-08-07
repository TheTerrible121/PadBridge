#include "socket_client.hpp"

#include <array>
#include <cstring>
#include <iostream>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace padbridge {
namespace {

void closeSocket(const PadBridgeSocket socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    ::close(socket);
#endif
}

}  // namespace

TcpClient::TcpClient() {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        std::cerr << "WSAStartup failed\n";
    }
#endif
}

TcpClient::~TcpClient() {
    close();
#ifdef _WIN32
    WSACleanup();
#endif
}

bool TcpClient::connectTo(const std::string& host, const std::uint16_t port) {
    close();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* results = nullptr;
    const auto service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &results) != 0) {
        return false;
    }

    PadBridgeSocket connected = kInvalidPadBridgeSocket;
    for (auto* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
        const auto candidateSocket =
            ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (candidateSocket == kInvalidPadBridgeSocket) {
            continue;
        }
        if (::connect(candidateSocket, candidate->ai_addr,
                      static_cast<int>(candidate->ai_addrlen)) == 0) {
            int enabled = 1;
            setsockopt(candidateSocket, IPPROTO_TCP, TCP_NODELAY,
                       reinterpret_cast<const char*>(&enabled), sizeof(enabled));
            connected = candidateSocket;
            break;
        }
        closeSocket(candidateSocket);
    }
    freeaddrinfo(results);
    if (connected != kInvalidPadBridgeSocket) {
        const std::scoped_lock lock(socketMutex_);
        socket_ = connected;
        open_.store(true);
    }
    return open_.load();
}

bool TcpClient::sendMessage(const MessageType type, const std::uint16_t flags,
                            const std::uint32_t sequence, const std::uint64_t timestampNs,
                            const std::span<const std::uint8_t> payload) {
    if (payload.size() > kMaxPayloadSize) {
        return false;
    }
    const Header header{kProtocolVersion, type, flags,
                        static_cast<std::uint32_t>(payload.size()), sequence, timestampNs};
    const auto encoded = encodeHeader(header);
    return sendAll(encoded) && sendAll(payload);
}

bool TcpClient::receiveMessage(Message& message) {
    std::array<std::uint8_t, kHeaderSize> bytes{};
    if (!receiveAll(bytes)) {
        return false;
    }
    const auto decoded = decodeHeader(bytes);
    if (!decoded) {
        close();
        return false;
    }
    message.header = *decoded;
    message.payload.resize(message.header.payloadLength);
    return receiveAll(message.payload);
}

void TcpClient::close() {
    PadBridgeSocket socket = kInvalidPadBridgeSocket;
    {
        const std::scoped_lock lock(socketMutex_);
        socket = socket_;
        socket_ = kInvalidPadBridgeSocket;
        open_.store(false);
    }
    if (socket != kInvalidPadBridgeSocket) {
#ifdef _WIN32
        shutdown(socket, SD_BOTH);
#else
        shutdown(socket, SHUT_RDWR);
#endif
        closeSocket(socket);
    }
}

bool TcpClient::sendAll(std::span<const std::uint8_t> bytes) {
    PadBridgeSocket socket = kInvalidPadBridgeSocket;
    {
        const std::scoped_lock lock(socketMutex_);
        socket = socket_;
    }
    while (!bytes.empty() && open_.load()) {
        const auto amount = ::send(socket, reinterpret_cast<const char*>(bytes.data()),
                                   static_cast<int>(bytes.size()), 0);
        if (amount <= 0) {
            close();
            return false;
        }
        bytes = bytes.subspan(static_cast<std::size_t>(amount));
    }
    return bytes.empty();
}

bool TcpClient::receiveAll(std::span<std::uint8_t> bytes) {
    PadBridgeSocket socket = kInvalidPadBridgeSocket;
    {
        const std::scoped_lock lock(socketMutex_);
        socket = socket_;
    }
    while (!bytes.empty() && open_.load()) {
        const auto amount = ::recv(socket, reinterpret_cast<char*>(bytes.data()),
                                   static_cast<int>(bytes.size()), 0);
        if (amount <= 0) {
            close();
            return false;
        }
        bytes = bytes.subspan(static_cast<std::size_t>(amount));
    }
    return bytes.empty();
}

}  // namespace padbridge
