#pragma once

#include "protocol.hpp"

#include <optional>

namespace padbridge {

// Converts remote pointer callbacks into the strict Windows lifecycle:
// exactly one DOWN, zero or more UPDATEs, and one UP/CANCEL. UIKit may replay
// coalesced callbacks and a reconnect may leave late packets in flight.
constexpr std::optional<PointerPhase> normalizePointerPhase(
    const bool alreadyActive, const PointerPhase requested) noexcept {
    if (requested == PointerPhase::down) {
        return alreadyActive ? PointerPhase::move : PointerPhase::down;
    }
    if (!alreadyActive) return std::nullopt;
    return requested;
}

}  // namespace padbridge
