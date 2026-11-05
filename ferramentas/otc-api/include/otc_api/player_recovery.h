#pragma once

#include "otc_api/profile.h"

namespace otc {

// Reuse the calibrated stats offset, never coordinates/HP from a previous session.
inline std::vector<ChainStep> gamePlayerRecoveryChain(const Profile& layout, std::int32_t gameRva,
                                                    std::int32_t memberOffset = 0) {
    if (gameRva <= 0 || layout.chain.size() < 2 || layout.chain.back().deref ||
        layout.chain.back().offset <= 0 || layout.chain.back().offset > 8192 ||
        !layout.hasPosition || !layout.inventory.present || !layout.inventory.slotsArePointers ||
        memberOffset < 0 || memberOffset > 64 || memberOffset % sizeof(void*) != 0)
        return {};
    return {{gameRva, false}, {memberOffset, true}, layout.chain.back()};
}

struct PlayerRecoveryConfirmation {
    std::uintptr_t base = 0;
    std::int32_t rva = 0;
    bool observe(std::uintptr_t candidate, std::int32_t candidateRva) {
        const bool confirmed = candidate && candidate == base && candidateRva == rva;
        base = candidate;
        rva = candidate ? candidateRva : 0;
        return confirmed;
    }
};

} // namespace otc

