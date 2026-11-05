#pragma once
#include "core/profile/BuildProfile.hpp"
#include <cstdint>
namespace crome { struct RuntimeContext { uintptr_t moduleBase{}; uintptr_t localPlayer{}; uintptr_t game{}; uintptr_t map{}; const BuildProfile* profile{}; bool attached() const { return moduleBase && profile; } }; }
