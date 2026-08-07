#include "annex_b_reader.hpp"

#include <algorithm>
#include <cstddef>

namespace padbridge {
namespace {

struct StartCode {
    std::size_t offset;
    std::size_t length;
};

std::vector<StartCode> findStartCodes(const std::span<const std::uint8_t> bytes) {
    std::vector<StartCode> starts;
    for (std::size_t i = 0; i + 3 <= bytes.size();) {
        if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) {
            starts.push_back({i, 3});
            i += 3;
        } else if (i + 4 <= bytes.size() && bytes[i] == 0 && bytes[i + 1] == 0 &&
                   bytes[i + 2] == 0 && bytes[i + 3] == 1) {
            starts.push_back({i, 4});
            i += 4;
        } else {
            ++i;
        }
    }
    return starts;
}

std::vector<std::size_t> findAudOffsets(const std::span<const std::uint8_t> bytes) {
    std::vector<std::size_t> offsets;
    for (const auto start : findStartCodes(bytes)) {
        const auto header = start.offset + start.length;
        if (header < bytes.size() && (bytes[header] & 0x1fU) == 9U) {
            offsets.push_back(start.offset);
        }
    }
    return offsets;
}

}  // namespace

void AnnexBAccessUnitParser::push(const std::span<const std::uint8_t> bytes,
                                  const Callback& callback) {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    emitComplete(callback, false);
}

void AnnexBAccessUnitParser::flush(const Callback& callback) {
    emitComplete(callback, true);
}

void AnnexBAccessUnitParser::emitComplete(const Callback& callback, const bool flushAll) {
    const auto auds = findAudOffsets(buffer_);
    if (auds.size() >= 2) {
        std::size_t begin = 0;
        for (std::size_t index = 1; index < auds.size(); ++index) {
            const auto end = auds[index];
            std::vector<std::uint8_t> unit(buffer_.begin() + static_cast<std::ptrdiff_t>(begin),
                                           buffer_.begin() + static_cast<std::ptrdiff_t>(end));
            const bool keyframe = containsIdr(unit);
            callback(std::move(unit), keyframe);
            begin = end;
        }
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(begin));
    }

    if (flushAll && !buffer_.empty()) {
        auto unit = std::move(buffer_);
        buffer_.clear();
        const bool keyframe = containsIdr(unit);
        callback(std::move(unit), keyframe);
    }
}

bool AnnexBAccessUnitParser::containsIdr(const std::span<const std::uint8_t> bytes) {
    for (const auto start : findStartCodes(bytes)) {
        const auto header = start.offset + start.length;
        if (header < bytes.size() && (bytes[header] & 0x1fU) == 5U) {
            return true;
        }
    }
    return false;
}

}  // namespace padbridge
