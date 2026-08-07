#include "ffmpeg_source.hpp"
#include "protocol.hpp"
#include "socket_client.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

namespace {

struct Options {
    std::string host{"127.0.0.1"};
    std::uint16_t port{52100};
    padbridge::EncoderSettings encoder{};
};

void usage() {
    std::cout << "PadBridge host checkpoint 1\n"
              << "Usage: padbridge_host [--host IP] [--port 52100] [--fps 120] "
                 "[--bitrate 60000000] [--display INDEX] [--ffmpeg PATH]\n"
              << "Without --display, an NVENC test pattern is sent.\n";
}

bool parseOptions(const int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            return false;
        }
        if (i + 1 >= argc) {
            return false;
        }
        const std::string value = argv[++i];
        try {
            if (arg == "--host") options.host = value;
            else if (arg == "--port") options.port = static_cast<std::uint16_t>(std::stoul(value));
            else if (arg == "--fps") options.encoder.fps = static_cast<std::uint16_t>(std::stoul(value));
            else if (arg == "--bitrate") options.encoder.bitrate = std::stoul(value);
            else if (arg == "--display") options.encoder.displayIndex = std::stoi(value);
            else if (arg == "--ffmpeg") options.encoder.ffmpegPath = value;
            else return false;
        } catch (...) {
            return false;
        }
    }
    return options.encoder.fps > 0 && options.port > 0;
}

}  // namespace

int main(const int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--help") {
        usage();
        return 0;
    }
    Options options;
    if (!parseOptions(argc, argv, options)) {
        usage();
        return 2;
    }

    padbridge::TcpClient client;
    std::cout << "Waiting for iPad through " << options.host << ':' << options.port << " ...\n";
    while (!client.connectTo(options.host, options.port)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    std::cout << "Connected. Starting " << options.encoder.width << 'x'
              << options.encoder.height << '@' << options.encoder.fps << " NVENC stream"
              << (options.encoder.displayIndex >= 0 ? " from Windows display.\n" : " test pattern.\n");

    std::jthread feedbackThread([&](std::stop_token) {
        padbridge::Message message;
        bool announced = false;
        while (client.receiveMessage(message)) {
            if (message.header.type == padbridge::MessageType::pointer && !announced) {
                std::cout << "Touch/Pencil feedback channel active.\n";
                announced = true;
            }
        }
    });

    std::uint32_t sequence = 0;
    const padbridge::VideoConfig config{options.encoder.width, options.encoder.height,
                                        options.encoder.fps, 1, 1, options.encoder.bitrate};
    const auto configBytes = padbridge::encodeVideoConfig(config);
    if (!client.sendMessage(padbridge::MessageType::videoConfig, 0, sequence++,
                            padbridge::monotonicNowNs(), configBytes)) {
        std::cerr << "Could not send video configuration.\n";
        client.close();
        return 1;
    }

    const auto interval = std::chrono::nanoseconds(1'000'000'000LL / options.encoder.fps);
    auto nextFrame = std::chrono::steady_clock::now();
    padbridge::FfmpegSource source(options.encoder);
    const bool completed = source.run([&](std::vector<std::uint8_t>&& accessUnit,
                                           const bool isKeyframe) {
        nextFrame += interval;
        if (client.isOpen()) {
            client.sendMessage(padbridge::MessageType::videoFrame,
                               isKeyframe ? padbridge::MessageFlags::keyframe : 0,
                               sequence++, padbridge::monotonicNowNs(), accessUnit);
        }
        std::this_thread::sleep_until(nextFrame);
    });

    if (!completed) {
        std::cerr << "FFmpeg stopped. Check that FFmpeg includes h264_nvenc and the NVIDIA "
                     "driver is current.\n";
        client.close();
        return 1;
    }
    client.close();
    return 0;
}
