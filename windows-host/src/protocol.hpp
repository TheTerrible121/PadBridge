#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace padbridge {

constexpr std::size_t kHeaderSize = 24;
constexpr std::size_t kVideoConfigSize = 16;
constexpr std::uint32_t kMaxPayloadSize = 16U * 1024U * 1024U;
constexpr std::uint8_t kProtocolVersion = 1;

enum class MessageType : std::uint8_t {
    hello = 1,
    videoConfig = 2,
    videoFrame = 3,
    pointer = 4,
    audioConfig = 5,
    audioFrame = 6,
    ping = 7,
    pong = 8,
    stats = 9,
};

enum MessageFlags : std::uint16_t {
    keyframe = 1U << 0U,
    discontinuity = 1U << 1U,
};

struct Header {
    std::uint8_t version{kProtocolVersion};
    MessageType type{MessageType::hello};
    std::uint16_t flags{0};
    std::uint32_t payloadLength{0};
    std::uint32_t sequence{0};
    std::uint64_t timestampNs{0};
};

struct VideoConfig {
    std::uint16_t width{2420};
    std::uint16_t height{1668};
    std::uint16_t refreshHz{120};
    std::uint8_t codec{1};
    std::uint8_t pixelFormat{1};
    std::uint32_t bitrate{60'000'000};
};

std::array<std::uint8_t, kHeaderSize> encodeHeader(const Header& header);
std::optional<Header> decodeHeader(std::span<const std::uint8_t> bytes);
std::array<std::uint8_t, kVideoConfigSize> encodeVideoConfig(const VideoConfig& config);
std::optional<VideoConfig> decodeVideoConfig(std::span<const std::uint8_t> bytes);
std::uint64_t monotonicNowNs();

}  // namespace padbridge

