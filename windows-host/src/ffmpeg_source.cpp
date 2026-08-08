#include "ffmpeg_source.hpp"

#include <array>
#include <cstdio>
#include <sstream>

namespace padbridge {

std::string FfmpegSource::command() const {
    const auto bitrateK = settings_.bitrate / 1000U;
    std::ostringstream out;
    out << '"' << settings_.ffmpegPath << '"'
        << " -hide_banner -loglevel warning";
    if (settings_.displayIndex >= 0) {
        out << " -f lavfi -i ddagrab=output_idx=" << settings_.displayIndex
            << ":framerate=" << settings_.fps
            << ":draw_mouse=1,hwdownload,format=bgra";
    } else {
        out << " -f lavfi -i testsrc2=size=" << settings_.width << 'x' << settings_.height
            << ":rate=" << settings_.fps << " -pix_fmt nv12";
    }
    out << " -an -c:v h264_nvenc -preset p1 -tune ull"
        << " -rc cbr -b:v " << bitrateK << "k -maxrate " << bitrateK << "k"
        << " -bufsize " << (bitrateK / 30U) << "k"
        << " -g " << settings_.fps << " -bf 0 -zerolatency 1 -forced-idr 1"
        << " -bsf:v h264_metadata=aud=insert -flush_packets 1 -f h264 -";
    return out.str();
}

bool FfmpegSource::run(const AnnexBAccessUnitParser::Callback& callback) {
#ifdef _WIN32
    FILE* pipe = _popen(command().c_str(), "rb");
#else
    FILE* pipe = popen(command().c_str(), "r");
#endif
    if (pipe == nullptr) {
        return false;
    }

    AnnexBAccessUnitParser parser;
    std::array<std::uint8_t, 16U * 1024U> chunk{};
    while (const auto amount = std::fread(chunk.data(), 1, chunk.size(), pipe)) {
        parser.push(std::span<const std::uint8_t>(chunk.data(), amount), callback);
    }
    parser.flush(callback);
#ifdef _WIN32
    return _pclose(pipe) == 0;
#else
    return pclose(pipe) == 0;
#endif
}

}  // namespace padbridge
