#pragma once

#include "protocol.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace padbridge {

class InputInjector {
public:
    // Public only so platform-specific implementation helpers can operate on
    // the otherwise opaque pimpl type; callers still cannot construct it.
    struct Impl;

    explicit InputInjector(int monitorIndex);
    ~InputInjector();

    InputInjector(const InputInjector&) = delete;
    InputInjector& operator=(const InputInjector&) = delete;

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] const std::string& status() const noexcept;
    [[nodiscard]] const std::string& lastError() const noexcept;
    bool inject(const PointerEvent& event);

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace padbridge
