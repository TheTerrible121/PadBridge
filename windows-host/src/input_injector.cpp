#include "input_injector.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace padbridge {

struct InputInjector::Impl {
#ifdef _WIN32
    struct TouchState {
        POINT point{};
        std::uint32_t pressure{512};
    };

    RECT target{};
    bool touchReady{false};
    HSYNTHETICPOINTERDEVICE penDevice{nullptr};
    std::map<std::uint32_t, TouchState> touches;
    std::optional<std::uint32_t> primaryTouch;
    std::optional<std::uint32_t> activePen;
    std::optional<POINT> penLocation;
#endif
    bool isAvailable{false};
    std::string statusText;
};

#ifdef _WIN32
namespace {

constexpr std::size_t kMaximumTouches = 10;

BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto* monitors = reinterpret_cast<std::vector<RECT>*>(data);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        monitors->push_back(info.rcMonitor);
    }
    return TRUE;
}

std::vector<RECT> monitorRects() {
    std::vector<RECT> monitors;
    EnumDisplayMonitors(nullptr, nullptr, collectMonitor,
                        reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}

POINT mapPoint(const PointerEvent& event, const RECT& target) {
    const auto x = std::clamp(event.x, 0.0F, 1.0F);
    const auto y = std::clamp(event.y, 0.0F, 1.0F);
    const auto width = std::max<LONG>(1, target.right - target.left);
    const auto height = std::max<LONG>(1, target.bottom - target.top);
    return POINT{
        target.left + static_cast<LONG>(std::lround(x * static_cast<float>(width - 1))),
        target.top + static_cast<LONG>(std::lround(y * static_cast<float>(height - 1))),
    };
}

std::uint32_t normalizedPressure(const float pressure, const std::uint32_t fallback) {
    if (!std::isfinite(pressure)) return fallback;
    return static_cast<std::uint32_t>(
        std::lround(std::clamp(pressure, 0.0F, 1.0F) * 1024.0F));
}

POINTER_FLAGS activeFlags(const PointerPhase phase, const bool primary,
                          const bool pen) {
    POINTER_FLAGS flags = POINTER_FLAG_NONE;
    switch (phase) {
        case PointerPhase::down:
            flags = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_DOWN;
            break;
        case PointerPhase::move:
            flags = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE;
            break;
        case PointerPhase::up:
            flags = POINTER_FLAG_UP;
            break;
        case PointerPhase::cancel:
            flags = POINTER_FLAG_UP | POINTER_FLAG_CANCELED;
            break;
    }
    if (primary) flags |= POINTER_FLAG_PRIMARY;
    if (pen && (phase == PointerPhase::down || phase == PointerPhase::move)) {
        flags |= POINTER_FLAG_FIRSTBUTTON;
    }
    return flags;
}

bool injectTouchFrame(std::vector<POINTER_TOUCH_INFO>& contacts) {
    for (int retry = 0; retry < 4; ++retry) {
        if (InjectTouchInput(static_cast<UINT32>(contacts.size()), contacts.data())) return true;
        if (GetLastError() != ERROR_NOT_READY) return false;
        Sleep(1);
    }
    return false;
}

bool injectPenFrame(HSYNTHETICPOINTERDEVICE device, POINTER_TYPE_INFO& info) {
    for (int retry = 0; retry < 4; ++retry) {
        if (InjectSyntheticPointerInput(device, &info, 1)) return true;
        if (GetLastError() != ERROR_NOT_READY) return false;
        Sleep(1);
    }
    return false;
}

bool injectTouch(InputInjector::Impl& impl, const PointerEvent& event) {
    if (!impl.touchReady) return false;

    const bool existed = impl.touches.contains(event.id);
    if (event.phase == PointerPhase::down) {
        if (!existed && impl.touches.size() >= kMaximumTouches) return false;
        impl.touches[event.id] = {mapPoint(event, impl.target),
                                  normalizedPressure(event.pressure, 512)};
        if (!impl.primaryTouch.has_value()) impl.primaryTouch = event.id;
    } else if (!existed) {
        // A move/up without a preceding down can arrive after a reconnect. Ignore it.
        return false;
    } else if (event.phase == PointerPhase::move) {
        impl.touches[event.id] = {mapPoint(event, impl.target),
                                  normalizedPressure(event.pressure, 512)};
    }

    std::vector<POINTER_TOUCH_INFO> contacts;
    contacts.reserve(impl.touches.size());
    for (const auto& [id, state] : impl.touches) {
        POINTER_TOUCH_INFO contact{};
        contact.pointerInfo.pointerType = PT_TOUCH;
        contact.pointerInfo.pointerId = id;
        contact.pointerInfo.ptPixelLocation = state.point;
        const auto phase = id == event.id ? event.phase : PointerPhase::move;
        contact.pointerInfo.pointerFlags = activeFlags(
            phase, impl.primaryTouch.has_value() && *impl.primaryTouch == id, false);
        contact.touchFlags = TOUCH_FLAG_NONE;
        contact.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION |
                            TOUCH_MASK_PRESSURE;
        contact.rcContact.left = std::max(impl.target.left, state.point.x - 3);
        contact.rcContact.right = std::min(impl.target.right - 1, state.point.x + 3);
        contact.rcContact.top = std::max(impl.target.top, state.point.y - 3);
        contact.rcContact.bottom = std::min(impl.target.bottom - 1, state.point.y + 3);
        contact.orientation = 90;
        contact.pressure = state.pressure;
        contacts.push_back(contact);
    }

    const bool injected = !contacts.empty() && injectTouchFrame(contacts);
    if (!injected) {
        if (!existed && event.phase == PointerPhase::down) {
            impl.touches.erase(event.id);
            if (impl.touches.empty()) impl.primaryTouch.reset();
        }
        return false;
    }

    if (event.phase == PointerPhase::up || event.phase == PointerPhase::cancel) {
        impl.touches.erase(event.id);
        if (impl.touches.empty()) impl.primaryTouch.reset();
    }
    return true;
}

bool injectPen(InputInjector::Impl& impl, const PointerEvent& event) {
    if (impl.penDevice == nullptr) return false;

    if (event.phase == PointerPhase::down) {
        impl.activePen = event.id;
        impl.penLocation = mapPoint(event, impl.target);
    } else if (!impl.activePen.has_value() || *impl.activePen != event.id ||
               !impl.penLocation.has_value()) {
        return false;
    } else if (event.phase == PointerPhase::move) {
        impl.penLocation = mapPoint(event, impl.target);
    }

    POINTER_TYPE_INFO info{};
    info.type = PT_PEN;
    info.penInfo.pointerInfo.pointerType = PT_PEN;
    info.penInfo.pointerInfo.pointerId = 0;
    info.penInfo.pointerInfo.ptPixelLocation = *impl.penLocation;
    info.penInfo.pointerInfo.pointerFlags = activeFlags(event.phase, true, true);
    info.penInfo.penFlags = PEN_FLAG_NONE;
    info.penInfo.penMask = PEN_MASK_PRESSURE | PEN_MASK_TILT_X | PEN_MASK_TILT_Y;
    info.penInfo.pressure = normalizedPressure(event.pressure, 512);
    constexpr float radiansToDegrees = 57.29577951308232F;
    info.penInfo.tiltX = static_cast<INT32>(std::lround(
        std::clamp(event.tiltX * radiansToDegrees, -90.0F, 90.0F)));
    info.penInfo.tiltY = static_cast<INT32>(std::lround(
        std::clamp(event.tiltY * radiansToDegrees, -90.0F, 90.0F)));

    const bool injected = injectPenFrame(impl.penDevice, info);
    if (injected && (event.phase == PointerPhase::up ||
                     event.phase == PointerPhase::cancel)) {
        impl.activePen.reset();
        impl.penLocation.reset();
    }
    return injected;
}

}  // namespace
#endif

InputInjector::InputInjector(const int monitorIndex) : impl_(std::make_unique<Impl>()) {
#ifdef _WIN32
    // Coordinates supplied to the injection APIs are physical desktop pixels.
    // This call can legitimately fail if another component set awareness first.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const auto monitors = monitorRects();
    if (monitorIndex < 0 || static_cast<std::size_t>(monitorIndex) >= monitors.size()) {
        std::ostringstream message;
        message << "input monitor #" << monitorIndex << " is invalid; Windows found "
                << monitors.size() << " monitor(s)";
        impl_->statusText = message.str();
        return;
    }

    impl_->target = monitors[static_cast<std::size_t>(monitorIndex)];
    impl_->touchReady = InitializeTouchInjection(
                            static_cast<UINT32>(kMaximumTouches), TOUCH_FEEDBACK_NONE) != FALSE;
    impl_->penDevice = CreateSyntheticPointerDevice(PT_PEN, 1, POINTER_FEEDBACK_NONE);
    impl_->isAvailable = impl_->touchReady || impl_->penDevice != nullptr;

    const auto width = impl_->target.right - impl_->target.left;
    const auto height = impl_->target.bottom - impl_->target.top;
    std::ostringstream message;
    message << "input monitor #" << monitorIndex << " at (" << impl_->target.left << ','
            << impl_->target.top << "), " << width << 'x' << height << ": touch "
            << (impl_->touchReady ? "ready" : "unavailable") << ", Pencil "
            << (impl_->penDevice != nullptr ? "ready" : "unavailable");
    impl_->statusText = message.str();
#else
    (void)monitorIndex;
    impl_->statusText = "input injection is available only on Windows";
#endif
}

InputInjector::~InputInjector() {
#ifdef _WIN32
    if (impl_->penDevice != nullptr) DestroySyntheticPointerDevice(impl_->penDevice);
#endif
}

bool InputInjector::available() const noexcept {
    return impl_->isAvailable;
}

const std::string& InputInjector::status() const noexcept {
    return impl_->statusText;
}

bool InputInjector::inject(const PointerEvent& event) {
#ifdef _WIN32
    return event.tool == PointerTool::pencil ? injectPen(*impl_, event)
                                             : injectTouch(*impl_, event);
#else
    (void)event;
    return false;
#endif
}

}  // namespace padbridge
