#pragma once

#include "annex_b_reader.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace padbridge {

struct EncoderSettings {
    std::string ffmpegPath{"ffmpeg"};
    std::uint16_t width{2420};
    std::uint16_t height{1668};
    std::uint16_t fps{120};
    std::uint32_t bitrate{60'000'000};
    int displayIndex{-1};
};

class FfmpegSource {
public:
    explicit FfmpegSource(EncoderSettings settings) : settings_(std::move(settings)) {}
    bool run(const AnnexBAccessUnitParser::Callback& callback);

private:
    EncoderSettings settings_;
    std::string command() const;
};

}  // namespace padbridge
