#pragma once
#include <windows.h>
#include <cstdint>
#include <limits>

namespace crome::olders_attack {
enum class Dispatch { Reject, AlreadySelected, Call };
inline Dispatch dispatch(std::uintptr_t target, std::uintptr_t selected,
                         std::uint32_t expectedId, std::uint32_t observedId,
                         std::uint32_t hp, bool inMap) {
    if (!target || !expectedId || observedId != expectedId || hp == 0 || hp > 100 || !inMap)
        return Dispatch::Reject;
    return target == selected ? Dispatch::AlreadySelected : Dispatch::Call;
}
// Exact Olders baseline: the natural caller increments Creature+8 once for
// its by-value argument; Game::attack decrements it on every normal exit.
// This is not a generic std::shared_ptr control block or a borrowed argument.
inline bool acquireConsumedReference(volatile LONG* counter) {
    LONG previous=InterlockedCompareExchange(counter,0,0);
    for(;;){
        if(previous<=0||previous==std::numeric_limits<LONG>::max())return false;
        const LONG observed=InterlockedCompareExchange(counter,previous+1,previous);
        if(observed==previous)return true;
        previous=observed;
    }
}
inline constexpr std::uintptr_t counterOffset=8;
}
