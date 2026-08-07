#include "annex_b_reader.hpp"
#include "protocol.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    using namespace padbridge;

    const Header original{kProtocolVersion, MessageType::videoFrame, MessageFlags::keyframe,
                          0x00123456U, 0x89abcdefU, 0x0123456789abcdefULL};
    const auto encoded = encodeHeader(original);
    const auto decoded = decodeHeader(encoded);
    assert(decoded.has_value());
    assert(decoded->type == MessageType::videoFrame);
    assert(decoded->flags == MessageFlags::keyframe);
    assert(decoded->payloadLength == original.payloadLength);
    assert(decoded->sequence == original.sequence);
    assert(decoded->timestampNs == original.timestampNs);

    auto invalid = encoded;
    invalid[0] = 'X';
    assert(!decodeHeader(invalid).has_value());

    const VideoConfig video{2420, 1668, 120, 1, 1, 60'000'000};
    const auto videoBytes = encodeVideoConfig(video);
    const auto decodedVideo = decodeVideoConfig(videoBytes);
    assert(decodedVideo.has_value());
    assert(decodedVideo->width == 2420);
    assert(decodedVideo->height == 1668);
    assert(decodedVideo->refreshHz == 120);
    assert(decodedVideo->bitrate == 60'000'000);

    const std::array<std::uint8_t, kPointerEventSize> pointerBytes{
        0x00, 0x00, 0x00, 0x07,  // id 7
        0x00,                    // down
        0x01,                    // Pencil
        0x00, 0x00,              // buttons
        0x3e, 0x80, 0x00, 0x00,  // x = 0.25
        0x3f, 0x40, 0x00, 0x00,  // y = 0.75
        0x3f, 0x00, 0x00, 0x00,  // pressure = 0.5
        0x00, 0x00, 0x00, 0x00,  // tilt x = 0
        0x00, 0x00, 0x00, 0x00,  // tilt y = 0
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    };
    const auto pointer = decodePointerEvent(pointerBytes);
    assert(pointer.has_value());
    assert(pointer->id == 7);
    assert(pointer->phase == PointerPhase::down);
    assert(pointer->tool == PointerTool::pencil);
    assert(pointer->x == 0.25F);
    assert(pointer->y == 0.75F);
    assert(pointer->pressure == 0.5F);
    assert(pointer->timestampNs == 1);
    auto invalidPointer = pointerBytes;
    invalidPointer[4] = 4;
    assert(!decodePointerEvent(invalidPointer).has_value());
    assert(!decodePointerEvent(std::span<const std::uint8_t>(pointerBytes.data(),
                                                             pointerBytes.size() - 1))
                .has_value());

    const std::vector<std::uint8_t> stream{
        0, 0, 0, 1, 9, 0xf0, 0, 0, 1, 7, 0x11, 0, 0, 1, 8, 0x22,
        0, 0, 1, 5, 0xaa, 0xbb,
        0, 0, 0, 1, 9, 0xf0, 0, 0, 1, 1, 0xcc,
        0, 0, 1, 9, 0xf0, 0, 0, 1, 1, 0xdd,
    };
    AnnexBAccessUnitParser parser;
    std::vector<bool> keys;
    std::vector<std::size_t> sizes;
    const auto callback = [&](std::vector<std::uint8_t>&& unit, const bool key) {
        keys.push_back(key);
        sizes.push_back(unit.size());
    };
    parser.push(std::span<const std::uint8_t>(stream.data(), 17), callback);
    parser.push(std::span<const std::uint8_t>(stream.data() + 17, stream.size() - 17), callback);
    parser.flush(callback);
    assert(keys.size() == 3);
    assert(keys[0]);
    assert(!keys[1]);
    assert(!keys[2]);
    assert(sizes[0] > sizes[1]);

    std::cout << "PadBridge protocol tests passed\n";
    return 0;
}
