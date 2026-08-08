#include "input_injector.hpp"
#include "pointer_sequence.hpp"

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
        std::uint32_t pointerId{0};
    };

    RECT target{};
    bool touchReady{false};
    bool legacyTouchReady{false};
    HSYNTHETICPOINTERDEVICE touchDevice{nullptr};
    HSYNTHETICPOINTERDEVICE penDevice{nullptr};
    std::map<std::uint32_t, TouchState> touches;
    std::optional<std::uint32_t> primaryTouch;
    std::optional<std::uint32_t> activePen;
    std::optional<POINT> penLocation;
#endif
    bool isAvailable{false};
    std::string statusText;
    std::string lastErrorText;
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

std::uint32_t normalizedPressure(const float pressure, const std::uint32_t fallback,
                                 const bool requireContact) {
    if (!std::isfinite(pressure)) return fallback;
    auto value = static_cast<std::uint32_t>(
        std::lround(std::clamp(pressure, 0.0F, 1.0F) * 1024.0F));
    if (requireContact && value == 0) value = 1;
    return value;
}

POINTER_FLAGS touchPointerFlags(const PointerPhase phase, const bool primary) {
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
    return flags;
}

POINTER_FLAGS penPointerFlags(const PointerPhase phase) {
    switch (phase) {
        case PointerPhase::down:
            return POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_DOWN;
        case PointerPhase::move:
            return POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE;
        case PointerPhase::up:
            return POINTER_FLAG_INRANGE | POINTER_FLAG_UP;
        case PointerPhase::cancel:
            return POINTER_FLAG_INRANGE | POINTER_FLAG_UP | POINTER_FLAG_CANCELED;
    }
    return POINTER_FLAG_NONE;
}

std::string inputErrorText(const DWORD error) {
    switch (error) {
        case ERROR_ACCESS_DENIED:
            return "access denied (Win32 error 5)";
        case ERROR_INVALID_PARAMETER:
            return "invalid pointer sequence (Win32 error 87)";
        case ERROR_NOT_READY:
            return "input timing not ready (Win32 error 21)";
        default:
            return "Win32 error " + std::to_string(error);
    }
}

std::optional<std::uint32_t> allocateTouchPointerId(const InputInjector::Impl& impl) {
    // Microsoft's current synthetic-touch reference implementation numbers
    // contacts from zero. Keep IDs inside the device's registered 0..9 range.
    for (std::uint32_t candidate = 0; candidate < kMaximumTouches; ++candidate) {
        const bool used = std::any_of(impl.touches.begin(), impl.touches.end(),
                                      [candidate](const auto& item) {
                                          return item.second.pointerId == candidate;
                                      });
        if (!used) return candidate;
    }
    return std::nullopt;
}

bool injectTouchFrame(InputInjector::Impl& impl,
                      std::vector<POINTER_TOUCH_INFO>& contacts, DWORD& error) {
    for (int retry = 0; retry < 4; ++retry) {
        SetLastError(ERROR_SUCCESS);
        bool succeeded = false;
        if (impl.touchDevice != nullptr) {
            std::vector<POINTER_TYPE_INFO> pointers(contacts.size());
            for (std::size_t index = 0; index < contacts.size(); ++index) {
                pointers[index].type = PT_TOUCH;
                pointers[index].touchInfo = contacts[index];
            }
            succeeded = InjectSyntheticPointerInput(
                            impl.touchDevice, pointers.data(),
                            static_cast<UINT32>(pointers.size())) != FALSE;
        } else if (impl.legacyTouchReady) {
            succeeded = InjectTouchInput(static_cast<UINT32>(contacts.size()),
                                         contacts.data()) != FALSE;
        }
        if (succeeded) {
            error = ERROR_SUCCESS;
            return true;
        }
        error = GetLastError();
        if (error != ERROR_NOT_READY) return false;
        Sleep(1);
    }
    return false;
}

bool injectPenFrame(HSYNTHETICPOINTERDEVICE device, POINTER_TYPE_INFO& info,
                    DWORD& error) {
    for (int retry = 0; retry < 4; ++retry) {
        SetLastError(ERROR_SUCCESS);
        if (InjectSyntheticPointerInput(device, &info, 1)) {
            error = ERROR_SUCCESS;
            return true;
        }
        error = GetLastError();
        if (error != ERROR_NOT_READY) return false;
        Sleep(1);
    }
    return false;
}

POINTER_TYPE_INFO makePenInfo(const POINT point, const PointerEvent& event,
                              const POINTER_FLAGS flags, const bool contact) {
    POINTER_TYPE_INFO info{};
    info.type = PT_PEN;
    info.penInfo.pointerInfo.pointerType = PT_PEN;
    info.penInfo.pointerInfo.pointerId = 1;
    info.penInfo.pointerInfo.ptPixelLocation = point;
    info.penInfo.pointerInfo.pointerFlags = flags;
    info.penInfo.penFlags = PEN_FLAG_NONE;
    info.penInfo.penMask = PEN_MASK_PRESSURE | PEN_MASK_TILT_X | PEN_MASK_TILT_Y;
    info.penInfo.pressure = contact
        ? normalizedPressure(event.pressure, 512, true)
        : 0;
    constexpr float radiansToDegrees = 57.29577951308232F;
    info.penInfo.tiltX = static_cast<INT32>(std::lround(
        std::clamp(event.tiltX * radiansToDegrees, -90.0F, 90.0F)));
    info.penInfo.tiltY = static_cast<INT32>(std::lround(
        std::clamp(event.tiltY * radiansToDegrees, -90.0F, 90.0F)));
    return info;
}

bool injectTouch(InputInjector::Impl& impl, const PointerEvent& event) {
    if (!impl.touchReady) return false;

    const bool existed = impl.touches.contains(event.id);
    const auto normalizedPhase = normalizePointerPhase(existed, event.phase);
    if (!normalizedPhase.has_value()) {
        // A move/up without a preceding down can arrive after cancellation or
        // reconnect. It is stale input, not an injection failure.
        return true;
    }
    const auto previous = existed
        ? std::optional<InputInjector::Impl::TouchState>(impl.touches.at(event.id))
        : std::nullopt;
    const auto effectivePhase = *normalizedPhase;
    if (event.phase == PointerPhase::down) {
        if (!existed && impl.touches.size() >= kMaximumTouches) return false;
        if (!existed) {
            const auto pointerId = allocateTouchPointerId(impl);
            if (!pointerId.has_value()) return false;
            impl.touches[event.id] = {mapPoint(event, impl.target), *pointerId};
            if (!impl.primaryTouch.has_value()) impl.primaryTouch = event.id;
        } else {
            // A coalesced/retried DOWN for an active pointer is an UPDATE, not
            // a second pointer transition.
            impl.touches[event.id].point = mapPoint(event, impl.target);
        }
    } else if (event.phase == PointerPhase::move) {
        impl.touches[event.id].point = mapPoint(event, impl.target);
    }

    std::vector<POINTER_TOUCH_INFO> contacts;
    contacts.reserve(impl.touches.size());
    for (const auto& [id, state] : impl.touches) {
        POINTER_TOUCH_INFO contact{};
        contact.pointerInfo.pointerType = PT_TOUCH;
        contact.pointerInfo.pointerId = state.pointerId;
        contact.pointerInfo.ptPixelLocation = state.point;
        const auto phase = id == event.id ? effectivePhase : PointerPhase::move;
        contact.pointerInfo.pointerFlags = touchPointerFlags(
            phase, impl.primaryTouch.has_value() && *impl.primaryTouch == id);
        contact.touchFlags = TOUCH_FLAG_NONE;
        // Contact area is the only optional field used by Microsoft's current
        // working synthetic-touch path. Some virtual-display configurations
        // reject optional touch pressure/orientation with error 87.
        contact.touchMask = TOUCH_MASK_CONTACTAREA;
        contact.rcContact.left = std::max(impl.target.left, state.point.x - 3);
        contact.rcContact.right = std::min(impl.target.right - 1, state.point.x + 3);
        contact.rcContact.top = std::max(impl.target.top, state.point.y - 3);
        contact.rcContact.bottom = std::min(impl.target.bottom - 1, state.point.y + 3);
        contacts.push_back(contact);
    }

    DWORD error = ERROR_SUCCESS;
    const bool injected = !contacts.empty() && injectTouchFrame(impl, contacts, error);
    if (!injected) {
        if (error == ERROR_INVALID_PARAMETER) {
            // Error 87 cancels every active OS injection contact. Mirror that
            // reset locally so the next UIKit DOWN starts a fresh sequence.
            impl.touches.clear();
            impl.primaryTouch.reset();
        } else if (!existed && event.phase == PointerPhase::down) {
            impl.touches.erase(event.id);
            if (impl.touches.empty()) impl.primaryTouch.reset();
        } else if (previous.has_value()) {
            impl.touches[event.id] = *previous;
        }
        impl.lastErrorText = inputErrorText(error);
        return false;
    }
    impl.lastErrorText.clear();

    if (event.phase == PointerPhase::up || event.phase == PointerPhase::cancel) {
        impl.touches.erase(event.id);
        // Windows does not promote another contact to primary until every
        // contact in the gesture is lifted. Keep the old ID as a sentinel.
        if (impl.touches.empty()) impl.primaryTouch.reset();
    }
    return true;
}

bool injectPen(InputInjector::Impl& impl, const PointerEvent& event) {
    if (impl.penDevice == nullptr) return false;

    const bool existed = impl.activePen.has_value() &&
                         *impl.activePen == event.id &&
                         impl.penLocation.has_value();
    const auto normalizedPhase = normalizePointerPhase(existed, event.phase);
    if (!normalizedPhase.has_value()) return true;
    const auto effectivePhase = *normalizedPhase;
    if (event.phase == PointerPhase::down) {
        if (!existed) {
            impl.activePen = event.id;
        }
        impl.penLocation = mapPoint(event, impl.target);
    } else if (event.phase == PointerPhase::move) {
        impl.penLocation = mapPoint(event, impl.target);
    }

    DWORD error = ERROR_SUCCESS;
    if (!existed && effectivePhase == PointerPhase::down) {
        // A physical Pencil enters hover range before touching. Windows uses
        // the same state machine for a synthetic pen and rejects/ignores some
        // direct out-of-range -> DOWN transitions.
        auto hover = makePenInfo(*impl.penLocation, event,
                                 POINTER_FLAG_INRANGE | POINTER_FLAG_UPDATE, false);
        if (!injectPenFrame(impl.penDevice, hover, error)) {
            impl.activePen.reset();
            impl.penLocation.reset();
            impl.lastErrorText = inputErrorText(error);
            return false;
        }
    }

    const bool inContact = effectivePhase == PointerPhase::down ||
                           effectivePhase == PointerPhase::move;
    auto info = makePenInfo(*impl.penLocation, event,
                            penPointerFlags(effectivePhase), inContact);
    const bool injected = injectPenFrame(impl.penDevice, info, error);
    if (!injected) {
        if (error == ERROR_INVALID_PARAMETER) {
            impl.activePen.reset();
            impl.penLocation.reset();
        }
        impl.lastErrorText = inputErrorText(error);
        return false;
    }
    impl.lastErrorText.clear();
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
    // Windows 11's modern synthetic-pointer API is the primary touch path.
    // Retain InjectTouchInput as a compatibility fallback.
    impl_->touchDevice = CreateSyntheticPointerDevice(
        PT_TOUCH, static_cast<ULONG>(kMaximumTouches), POINTER_FEEDBACK_NONE);
    if (impl_->touchDevice == nullptr) {
        impl_->legacyTouchReady = InitializeTouchInjection(
            static_cast<UINT32>(kMaximumTouches), TOUCH_FEEDBACK_NONE) != FALSE;
    }
    impl_->touchReady = impl_->touchDevice != nullptr || impl_->legacyTouchReady;
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
    if (impl_->touchDevice != nullptr) DestroySyntheticPointerDevice(impl_->touchDevice);
    if (impl_->penDevice != nullptr) DestroySyntheticPointerDevice(impl_->penDevice);
#endif
}

bool InputInjector::available() const noexcept {
    return impl_->isAvailable;
}

const std::string& InputInjector::status() const noexcept {
    return impl_->statusText;
}

const std::string& InputInjector::lastError() const noexcept {
    return impl_->lastErrorText;
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
