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
        // Desktop Duplication can report cursor-only updates faster than the
        // monitor refresh rate. Keep sparse/idle capture, but discard updates
        // closer together than one requested frame interval before encoding.
        // The select filter only examines timestamps, so D3D11 frames remain
        // on the GPU in the zero-copy path.
        std::ostringstream captureGraph;
        captureGraph << "ddagrab=output_idx=" << settings_.displayIndex
                     << ":framerate=" << settings_.fps
                     << ":draw_mouse=1:dup_frames=0"
                     << ",select='isnan(prev_selected_t)+gt(floor(t*"
                     << settings_.fps << "+0.001),floor(prev_selected_t*"
                     << settings_.fps << "+0.001))'";
        if (!settings_.zeroCopy) {
            captureGraph << ",hwdownload,format=bgra";
        }

        if (settings_.adapterIndex >= 0) {
            // A source filter inside -filter_complex receives -filter_hw_device.
            // This is required for displays owned by a non-default DXGI adapter.
            out << " -init_hw_device d3d11va=padbridge:" << settings_.adapterIndex
                << " -filter_hw_device padbridge -filter_complex \""
                << captureGraph.str() << '"';
        } else {
            // hwdownload in captureGraph is required when the captured monitor
            // and NVENC live on different GPUs (for example, the OMEN internal
            // AMD display and its RTX encoder).
            out << " -f lavfi -i \"" << captureGraph.str() << '"';
        }
    } else {
        out << " -f lavfi -i testsrc2=size=" << settings_.width << 'x' << settings_.height
            << ":rate=" << settings_.fps << " -pix_fmt nv12";
    }
    // VFR preserves ddagrab's idle-frame suppression while dropping frames that
    // land on the same timestamp. Using the filtergraph time base prevents the
    // raw H.264 muxer from receiving duplicate DTS values at 120 Hz.
    out << " -an -fps_mode vfr -enc_time_base filter"
        << " -c:v h264_nvenc -preset p1 -tune ull"
        << " -rc cbr -b:v " << bitrateK << "k -maxrate " << bitrateK << "k"
        << " -bufsize " << (bitrateK / 30U) << "k"
        << " -g " << settings_.fps << " -bf 0 -zerolatency 1 -forced-idr 1"
        << " -bsf:v h264_metadata=aud=insert -flush_packets 1 -f h264 -";
    return out.str();
}

bool FfmpegSource::run(const AnnexBAccessUnitParser::Callback& callback,
                       const std::function<bool()>& shouldContinue) {
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
    while (shouldContinue()) {
        const auto amount = std::fread(chunk.data(), 1, chunk.size(), pipe);
        if (amount == 0) break;
        parser.push(std::span<const std::uint8_t>(chunk.data(), amount), callback);
    }
    const bool interrupted = !shouldContinue();
    if (!interrupted) parser.flush(callback);
#ifdef _WIN32
    const int exitCode = _pclose(pipe);
#else
    const int exitCode = pclose(pipe);
#endif
    // Closing the read pipe intentionally terminates FFmpeg when the iPad
    // disconnects. That is a clean session boundary, not an encoder failure.
    return interrupted || exitCode == 0;
}

}  // namespace padbridge
