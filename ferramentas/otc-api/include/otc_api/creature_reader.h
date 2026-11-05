#pragma once

#include "otc_api/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace otc {

struct CreatureLayout;
bool readCreatureNameAt(std::uintptr_t addr, const CreatureLayout& layout, std::string& out);

struct CreatureCalibHints {
    std::string name;
    bool hasZ = false;
    int z = 0;
    bool hasHpperc = false;
    int hpperc = 0;
    bool hasId = false;
    std::uint32_t id = 0;
};

void startCreatureReader();

void setCreatureCalibHints(const CreatureCalibHints& hints);
bool runCreatureScan();
std::string creatureCalibMessage();

std::string creaturesJson();
std::vector<CreatureSnapshot> currentCreatures();
// Uncached read for temporal correlation; false is not an empty battle list.
bool readFreshCreatures(std::vector<CreatureSnapshot>& out,
    std::vector<std::uint32_t>* removedZeroHp = nullptr);
void replayCreaturesToPipe();

bool resolveCreature(std::uint32_t id, std::uintptr_t& ptr, std::uintptr_t& rep);
bool resolveCreatureAt(int x, int y, int z, std::uintptr_t& ptr, std::uintptr_t& rep);
bool resolveCreatureByObject(std::uintptr_t obj, std::uintptr_t& ptr, std::uintptr_t& rep);

} // namespace otc

