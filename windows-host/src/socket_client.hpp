#pragma once

#include "protocol.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
using PadBridgeSocket = SOCKET;
constexpr PadBridgeSocket kInvalidPadBridgeSocket = INVALID_SOCKET;
#else
using PadBridgeSocket = int;
constexpr PadBridgeSocket kInvalidPadBridgeSocket = -1;
#endif

namespace padbridge {

struct Message {
    Header header;
    std::vector<std::uint8_t> payload;
};

class TcpClient {
public:
    TcpClient();
    ~TcpClient();
    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    bool connectTo(const std::string& host, std::uint16_t port);
    bool sendMessage(MessageType type, std::uint16_t flags,
                     std::uint32_t sequence, std::uint64_t timestampNs,
                     std::span<const std::uint8_t> payload);
    bool receiveMessage(Message& message);
    void close();
    [[nodiscard]] bool isOpen() const { return open_.load(); }

private:
    PadBridgeSocket socket_{kInvalidPadBridgeSocket};
    mutable std::mutex socketMutex_;
    std::atomic_bool open_{false};
    bool sendAll(std::span<const std::uint8_t> bytes);
    bool receiveAll(std::span<std::uint8_t> bytes);
};

}  // namespace padbridge
