#include "protocol.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>

namespace padbridge {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'P', 'D', 'B', '1'};

void put16(std::uint8_t* out, const std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 8U);
    out[1] = static_cast<std::uint8_t>(value);
}

void put32(std::uint8_t* out, const std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 24U);
    out[1] = static_cast<std::uint8_t>(value >> 16U);
    out[2] = static_cast<std::uint8_t>(value >> 8U);
    out[3] = static_cast<std::uint8_t>(value);
}

void put64(std::uint8_t* out, const std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        out[index] = static_cast<std::uint8_t>(value >> ((7U - index) * 8U));
    }
}

std::uint16_t get16(const std::uint8_t* in) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[0]) << 8U) |
                                      static_cast<std::uint16_t>(in[1]));
}

std::uint32_t get32(const std::uint8_t* in) {
    return (static_cast<std::uint32_t>(in[0]) << 24U) |
           (static_cast<std::uint32_t>(in[1]) << 16U) |
           (static_cast<std::uint32_t>(in[2]) << 8U) |
           static_cast<std::uint32_t>(in[3]);
}

std::uint64_t get64(const std::uint8_t* in) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8U) | static_cast<std::uint64_t>(in[index]);
    }
    return value;
}

float getFloat(const std::uint8_t* in) {
    return std::bit_cast<float>(get32(in));
}

}  // namespace

std::array<std::uint8_t, kHeaderSize> encodeHeader(const Header& header) {
    std::array<std::uint8_t, kHeaderSize> bytes{};
    std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
    bytes[4] = header.version;
    bytes[5] = static_cast<std::uint8_t>(header.type);
    put16(bytes.data() + 6, header.flags);
    put32(bytes.data() + 8, header.payloadLength);
    put32(bytes.data() + 12, header.sequence);
    put64(bytes.data() + 16, header.timestampNs);
    return bytes;
}

std::optional<Header> decodeHeader(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kHeaderSize ||
        !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()) ||
        bytes[4] != kProtocolVersion) {
        return std::nullopt;
    }

    Header header{};
    header.version = bytes[4];
    header.type = static_cast<MessageType>(bytes[5]);
    header.flags = get16(bytes.data() + 6);
    header.payloadLength = get32(bytes.data() + 8);
    header.sequence = get32(bytes.data() + 12);
    header.timestampNs = get64(bytes.data() + 16);
    if (header.payloadLength > kMaxPayloadSize) {
        return std::nullopt;
    }
    return header;
}

std::array<std::uint8_t, kVideoConfigSize> encodeVideoConfig(const VideoConfig& config) {
    std::array<std::uint8_t, kVideoConfigSize> bytes{};
    put16(bytes.data(), config.width);
    put16(bytes.data() + 2, config.height);
    put16(bytes.data() + 4, config.refreshHz);
    bytes[6] = config.codec;
    bytes[7] = config.pixelFormat;
    put32(bytes.data() + 8, config.bitrate);
    put32(bytes.data() + 12, 0);
    return bytes;
}

std::optional<VideoConfig> decodeVideoConfig(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kVideoConfigSize) {
        return std::nullopt;
    }
    VideoConfig config{};
    config.width = get16(bytes.data());
    config.height = get16(bytes.data() + 2);
    config.refreshHz = get16(bytes.data() + 4);
    config.codec = bytes[6];
    config.pixelFormat = bytes[7];
    config.bitrate = get32(bytes.data() + 8);
    return config;
}

std::optional<PointerEvent> decodePointerEvent(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kPointerEventSize || bytes[4] > 3U || bytes[5] > 1U) {
        return std::nullopt;
    }
    PointerEvent event{};
    event.id = get32(bytes.data());
    event.phase = static_cast<PointerPhase>(bytes[4]);
    event.tool = static_cast<PointerTool>(bytes[5]);
    event.buttons = get16(bytes.data() + 6);
    event.x = getFloat(bytes.data() + 8);
    event.y = getFloat(bytes.data() + 12);
    event.pressure = getFloat(bytes.data() + 16);
    event.tiltX = getFloat(bytes.data() + 20);
    event.tiltY = getFloat(bytes.data() + 24);
    event.timestampNs = get64(bytes.data() + 28);
    if (!std::isfinite(event.x) || !std::isfinite(event.y) ||
        !std::isfinite(event.pressure) || !std::isfinite(event.tiltX) ||
        !std::isfinite(event.tiltY)) {
        return std::nullopt;
    }
    return event;
}

std::uint64_t monotonicNowNs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace padbridge
