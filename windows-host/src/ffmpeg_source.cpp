#include "ffmpeg_source.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace padbridge {

#ifdef _WIN32
namespace {

struct MonitorSearch {
    LONG width{};
    LONG height{};
    RECT target{};
    bool found{false};
};

BOOL CALLBACK findMatchingMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto& search = *reinterpret_cast<MonitorSearch*>(data);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    const LONG width = info.rcMonitor.right - info.rcMonitor.left;
    const LONG height = info.rcMonitor.bottom - info.rcMonitor.top;
    if (width != search.width || height != search.height) return TRUE;

    search.target = info.rcMonitor;
    search.found = true;
    // Prefer the non-primary display when two monitors share a resolution.
    return (info.dwFlags & MONITORINFOF_PRIMARY) != 0 ? TRUE : FALSE;
}

bool findTargetMonitor(const std::uint16_t width, const std::uint16_t height,
                       RECT& target) {
    MonitorSearch search{static_cast<LONG>(width), static_cast<LONG>(height)};
    EnumDisplayMonitors(nullptr, nullptr, findMatchingMonitor,
                        reinterpret_cast<LPARAM>(&search));
    if (search.found) target = search.target;
    return search.found;
}

bool foregroundWindowIsOn(const RECT& target) {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) return false;
    const HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL);
    if (monitor == nullptr) return false;
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    return GetMonitorInfoW(monitor, &info) && EqualRect(&target, &info.rcMonitor);
}

bool targetDisplayIsActive(const RECT& target) {
    POINT cursor{};
    if (GetCursorPos(&cursor) && PtInRect(&target, cursor)) return true;

    // Synthetic touch/Pencil does not always move the Windows cursor. Recent
    // input counts as active when the foreground app is on the iPad display.
    LASTINPUTINFO input{};
    input.cbSize = sizeof(input);
    return GetLastInputInfo(&input) &&
           GetTickCount() - input.dwTime <= 750U &&
           foregroundWindowIsOn(target);
}

}  // namespace
#endif

std::string FfmpegSource::command() const {
    const auto bitrateK = settings_.bitrate / 1000U;
    std::ostringstream out;
    out << '"' << settings_.ffmpegPath << '"'
        << " -hide_banner -loglevel warning";
    if (settings_.displayIndex >= 0) {
        // A true 120 Hz clock is required for consistently smooth cursor and
        // window motion. The Windows pipe reader applies adaptive backpressure
        // below whenever the cursor is away from the iPad display.
        std::ostringstream captureGraph;
        captureGraph << "ddagrab=output_idx=" << settings_.displayIndex
                     << ":framerate=" << settings_.fps
                     << ":draw_mouse=1:dup_frames=1";
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
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 64U * 1024U) ||
        !SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
        if (readPipe != nullptr) CloseHandle(readPipe);
        if (writePipe != nullptr) CloseHandle(writePipe);
        std::cerr << "Could not create the FFmpeg output pipe (Win32 error "
                  << GetLastError() << ").\n";
        return false;
    }

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = writePipe;
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION process{};
    auto commandLine = command();
    std::vector<char> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back('\0');

    // Supplying lpApplicationName bypasses cmd.exe entirely. This handles
    // installed paths safely and avoids shell metacharacter parsing inside
    // FFmpeg filter expressions.
    const bool explicitPath = settings_.ffmpegPath.find('\\') != std::string::npos ||
                              settings_.ffmpegPath.find('/') != std::string::npos;
    const char* applicationName = explicitPath ? settings_.ffmpegPath.c_str() : nullptr;
    const BOOL started = CreateProcessA(
        applicationName, mutableCommand.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(writePipe);
    writePipe = nullptr;
    if (!started) {
        const DWORD error = GetLastError();
        CloseHandle(readPipe);
        std::cerr << "Could not start FFmpeg (Win32 error " << error << ").\n";
        return false;
    }

    CloseHandle(process.hThread);
    AnnexBAccessUnitParser parser;
    std::array<std::uint8_t, 16U * 1024U> chunk{};
    bool interrupted = false;
    bool readFailed = false;
    RECT targetMonitor{};
    const bool adaptive = findTargetMonitor(settings_.width, settings_.height,
                                             targetMonitor);
    auto fullRateUntil = std::chrono::steady_clock::now() +
                         std::chrono::seconds(2);
    std::cout << (adaptive
        ? "Adaptive capture active: 120 Hz on the iPad, near-zero work off-screen.\n"
        : "Adaptive target not found; maintaining the requested frame rate.\n");

    for (;;) {
        if (!shouldContinue()) {
            interrupted = true;
            TerminateProcess(process.hProcess, 0);
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (adaptive && targetDisplayIsActive(targetMonitor)) {
            // Keep the full-rate clock alive briefly after the cursor crosses
            // back to Windows. This prevents a rapid edge crossing from
            // oscillating between 1 and 120 Hz or feeling like a stutter.
            // The iPad display link holds the last active cadence for another
            // 0.5 s, producing a two-second end-to-end grace period.
            fullRateUntil = now + std::chrono::milliseconds(1500);
        }
        const bool fullRate = !adaptive || now < fullRateUntil;
        if (!fullRate) {
            // Stop draining stdout. The small pipe fills in milliseconds and
            // naturally blocks the FFmpeg capture/encode pipeline, eliminating
            // continuous NVENC, USB, and iPad decoder work while off-screen.
            if (WaitForSingleObject(process.hProcess, 5) == WAIT_OBJECT_0) break;
            continue;
        }

        DWORD available = 0;
        if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr)) {
            if (GetLastError() != ERROR_BROKEN_PIPE) readFailed = true;
            break;
        }

        if (available == 0) {
            if (WaitForSingleObject(process.hProcess, 5) == WAIT_OBJECT_0) break;
            continue;
        }

        DWORD amount = 0;
        const DWORD requested = std::min<DWORD>(
            available, static_cast<DWORD>(chunk.size()));
        if (!ReadFile(readPipe, chunk.data(), requested, &amount, nullptr)) {
            if (GetLastError() != ERROR_BROKEN_PIPE) readFailed = true;
            break;
        }
        if (amount != 0) {
            parser.push(std::span<const std::uint8_t>(chunk.data(), amount), callback);
        }
    }

    if (!interrupted) parser.flush(callback);
    if (WaitForSingleObject(process.hProcess, 2000) == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 1000);
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(readPipe);
    CloseHandle(process.hProcess);
    return interrupted || (!readFailed && exitCode == 0);
#else
    FILE* pipe = popen(command().c_str(), "r");
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
    const int exitCode = pclose(pipe);
    // Closing the read pipe intentionally terminates FFmpeg when the iPad
    // disconnects. That is a clean session boundary, not an encoder failure.
    return interrupted || exitCode == 0;
#endif
}

}  // namespace padbridge
