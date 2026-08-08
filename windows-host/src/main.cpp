#include "ffmpeg_source.hpp"
#include "input_injector.hpp"
#include "protocol.hpp"
#include "socket_client.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic_bool stopRequested{false};

void handleSignal(int) {
    stopRequested.store(true);
}

struct Options {
    std::string host{"127.0.0.1"};
    std::uint16_t port{52100};
    padbridge::EncoderSettings encoder{};
    int inputDisplayIndex{-1};
};

void usage() {
    std::cout << "PadBridge host\n"
              << "Usage: padbridge_host [--host IP] [--port 52100] [--fps 120] "
                 "[--bitrate 60000000] [--adapter INDEX] [--display INDEX] "
                 "[--input-display INDEX] "
                 "[--zero-copy] [--ffmpeg PATH]\n"
              << "Without --display, an NVENC test pattern is sent. Use --zero-copy only "
                 "when the display is attached to the NVIDIA GPU.\n";
}

bool parseOptions(const int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            return false;
        }
        if (arg == "--zero-copy") {
            options.encoder.zeroCopy = true;
            continue;
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
            else if (arg == "--adapter") options.encoder.adapterIndex = std::stoi(value);
            else if (arg == "--display") options.encoder.displayIndex = std::stoi(value);
            else if (arg == "--input-display") options.inputDisplayIndex = std::stoi(value);
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
    // The GUI controller captures these streams. Disable full buffering so its
    // connection status changes immediately rather than minutes later.
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    if (argc > 1 && std::string(argv[1]) == "--help") {
        usage();
        return 0;
    }
    Options options;
    if (!parseOptions(argc, argv, options)) {
        usage();
        return 2;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    while (!stopRequested.load()) {
        padbridge::TcpClient client;
        std::cout << "Waiting for iPad through " << options.host << ':' << options.port
                  << " ...\n";
        while (!stopRequested.load() && !client.connectTo(options.host, options.port)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (stopRequested.load()) break;

        std::cout << "Transport connected; verifying the iPad app.\n";

        const int inputDisplayIndex = options.inputDisplayIndex >= 0
            ? options.inputDisplayIndex
            : std::max(options.encoder.displayIndex, 0);
        // Recreate synthetic devices for each transport session. A cable pull
        // can interrupt a gesture before UP; destroying the old devices keeps
        // stale contacts from poisoning the next connection.
        padbridge::InputInjector input(inputDisplayIndex);
        std::cout << "Input: " << input.status() << ".\n";

        std::atomic_bool peerReady{false};
        std::jthread feedbackThread([&](std::stop_token) {
            padbridge::Message message;
            bool announced = false;
            bool reportedInvalid = false;
            bool reportedFailure = false;
            while (!stopRequested.load() && client.receiveMessage(message)) {
                if (message.header.type == padbridge::MessageType::hello) {
                    if (!peerReady.exchange(true)) {
                        std::cout << "iPad handshake complete.\n";
                    }
                } else if (message.header.type == padbridge::MessageType::ping) {
                    client.sendMessage(padbridge::MessageType::pong, 0, message.header.sequence,
                                       message.header.timestampNs, {});
                } else if (message.header.type == padbridge::MessageType::pointer) {
                    if (!announced) {
                        std::cout << "Touch/Pencil feedback channel active.\n";
                        announced = true;
                    }
                    const auto event = padbridge::decodePointerEvent(message.payload);
                    if (!event.has_value()) {
                        if (!reportedInvalid) {
                            std::cerr << "Ignored an invalid pointer packet.\n";
                            reportedInvalid = true;
                        }
                    } else if (!input.inject(*event) && !reportedFailure) {
                        const char* tool = event->tool == padbridge::PointerTool::pencil
                            ? "Pencil" : "finger";
                        const char* phase = "CANCEL";
                        if (event->phase == padbridge::PointerPhase::down) phase = "DOWN";
                        else if (event->phase == padbridge::PointerPhase::move) phase = "UPDATE";
                        else if (event->phase == padbridge::PointerPhase::up) phase = "UP";
                        std::cerr << "Windows rejected a touch/Pencil event: "
                                  << (input.lastError().empty() ? "unknown input error"
                                                                : input.lastError())
                                  << " [" << tool << ' ' << phase << ", id "
                                  << event->id << "].\n";
                        reportedFailure = true;
                    }
                }
            }
            if (!stopRequested.load()) {
                std::cout << "Transport disconnected.\n";
            }
        });

        std::jthread heartbeatThread([&](std::stop_token token) {
            std::uint32_t heartbeatSequence = 0xF0000000U;
            while (!token.stop_requested() && !stopRequested.load() && client.isOpen()) {
                for (int step = 0; step < 10 && !token.stop_requested() &&
                                   !stopRequested.load() && client.isOpen(); ++step) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (!token.stop_requested() && !stopRequested.load() && client.isOpen()) {
                    client.sendMessage(padbridge::MessageType::ping, 0, heartbeatSequence++,
                                       padbridge::monotonicNowNs(), {});
                }
            }
        });

        std::uint32_t sequence = 0;
        const std::string hello = "PadBridge Windows host/1.0";
        client.sendMessage(padbridge::MessageType::hello, 0, sequence++,
                           padbridge::monotonicNowNs(),
                           std::span<const std::uint8_t>(
                               reinterpret_cast<const std::uint8_t*>(hello.data()), hello.size()));

        const auto handshakeDeadline = std::chrono::steady_clock::now() +
                                       std::chrono::seconds(10);
        while (!stopRequested.load() && client.isOpen() && !peerReady.load() &&
               std::chrono::steady_clock::now() < handshakeDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!peerReady.load()) {
            client.close();
            feedbackThread.request_stop();
            heartbeatThread.request_stop();
            if (!stopRequested.load()) {
                std::cout << "iPad app not ready; retrying.\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }
            continue;
        }

        std::cout << "Starting " << options.encoder.width << 'x'
                  << options.encoder.height << '@' << options.encoder.fps << " NVENC stream"
                  << (options.encoder.displayIndex >= 0 ? " from Windows display" : " test pattern")
                  << (options.encoder.displayIndex >= 0 && options.encoder.adapterIndex >= 0
                          ? " on DXGI adapter #" + std::to_string(options.encoder.adapterIndex)
                          : "")
                  << (options.encoder.displayIndex >= 0 && options.encoder.zeroCopy
                          ? " (zero-copy).\n" : ".\n");

        const padbridge::VideoConfig config{options.encoder.width, options.encoder.height,
                                            options.encoder.fps, 1, 1, options.encoder.bitrate};
        const auto configBytes = padbridge::encodeVideoConfig(config);
        if (!client.sendMessage(padbridge::MessageType::videoConfig, 0, sequence++,
                                padbridge::monotonicNowNs(), configBytes)) {
            client.close();
            continue;
        }

        const auto interval = std::chrono::nanoseconds(1'000'000'000LL / options.encoder.fps);
        auto nextFrame = std::chrono::steady_clock::now();
        padbridge::FfmpegSource source(options.encoder);
        const bool completed = source.run(
            [&](std::vector<std::uint8_t>&& accessUnit, const bool isKeyframe) {
                if (!client.isOpen()) return;
                client.sendMessage(padbridge::MessageType::videoFrame,
                                   isKeyframe ? padbridge::MessageFlags::keyframe : 0,
                                   sequence++, padbridge::monotonicNowNs(), accessUnit);
                if (options.encoder.displayIndex < 0) {
                    nextFrame += interval;
                    std::this_thread::sleep_until(nextFrame);
                }
            },
            [&] { return !stopRequested.load() && client.isOpen(); });

        const bool disconnected = !client.isOpen();
        client.close();
        feedbackThread.request_stop();
        heartbeatThread.request_stop();
        if (!completed && !disconnected && !stopRequested.load()) {
            std::cerr << "FFmpeg stopped. Check that the bundled FFmpeg supports ddagrab/"
                         "h264_nvenc and that the NVIDIA driver is current.\n";
            return 1;
        }
        if (!stopRequested.load()) {
            std::cout << "iPad disconnected; reconnecting automatically.\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(350));
        }
    }

    std::cout << "PadBridge stopped.\n";
    return 0;
}
