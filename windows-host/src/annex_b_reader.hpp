#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace padbridge {

class AnnexBAccessUnitParser {
public:
    using Callback = std::function<void(std::vector<std::uint8_t>&&, bool keyframe)>;

    void push(std::span<const std::uint8_t> bytes, const Callback& callback);
    void flush(const Callback& callback);

private:
    std::vector<std::uint8_t> buffer_;
    void emitComplete(const Callback& callback, bool flushAll);
    static bool containsIdr(std::span<const std::uint8_t> bytes);
};

}  // namespace padbridge

