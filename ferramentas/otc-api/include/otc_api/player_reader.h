#pragma once

#include "otc_api/profile.h"
#include "otc_api/types.h"

#include <cstdint>
#include <string>

namespace otc {

struct CalibHints {
    bool hasHealth = false;
    int health = 0;
    int maxHealth = 0;
    bool hasMana = false;
    int mana = 0;
    int maxMana = 0;
    bool hasLevel = false;
    int level = 0;
    bool hasExp = false;
    std::uint64_t experience = 0;
    bool hasCapacity = false;
    int capacity = 0;
    bool hasPosition = false;
    int x = 0;
    int y = 0;
    int z = 0;
    bool hasZ = false;
    bool hasPositionBounds = false;
    int minX = 0;
    int maxX = 0;
    int minY = 0;
    int maxY = 0;
    bool forcePositionRediscovery = false;
    bool requirePositionMovement = false;
    bool expectBattle = false;
    std::uint32_t expectStateBits = 0;
    bool hasItemId = false;
    int itemId = 0;
    int itemSlot = 0; // 1..10, 0 = any
    bool needInventory = false;
};

void startPlayerReader();

std::string dllStatusJson();
std::string playerStatsJson();
std::string inventoryJson();
std::string calibrationJson();

PlayerSnapshot currentPlayer();
bool isPlayerReady();
std::uintptr_t currentStatsBase();
std::uintptr_t currentPlayerObject();
bool resolveInventoryItem(int slot, int expectId, std::uintptr_t& ptr, std::uintptr_t& rep);
bool resolveLocalPlayerShared(std::uintptr_t& ptr, std::uintptr_t& rep);
bool copyLiveProfile(Profile& out);
void setContainerLayout(const ContainerLayout& layout);
void setCreatureLayout(const CreatureLayout& layout);
void setActionLayout(const ActionLayout& layout);
void setMapLayout(const MapLayout& layout);

void setCalibHints(const CalibHints& hints);
bool runCalibrationScan();
bool runRefineScan();
bool saveCurrentProfile();

void replayStatusToPipe();

} // namespace otc

