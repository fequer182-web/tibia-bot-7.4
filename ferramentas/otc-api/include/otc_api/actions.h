#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace otc {

struct ActionResult {
    bool ok = false;
    std::string error;
    std::string via; // "client" | "keys"
};

ActionResult actionSay(const std::string& text, const std::string& mode = {});
ActionResult actionStep(const std::string& direction);
ActionResult actionStop();
ActionResult actionLogout();
ActionResult actionWalkTo(int x, int y, int z);

// Calibration-only semantic anchor. Resolves the static Game object from the
// client's Lua binding metadata and returns an image-relative RVA.
bool resolveCalibrationGameAnchors(std::vector<std::int32_t>& gameRvas);

struct UseTarget {
    bool self = false;
    bool hasCreature = false;
    std::uint32_t creatureId = 0;
    int container = -1;
    int slot = -1;
    int id = 0;
    bool hasPos = false;
    int x = 0;
    int y = 0;
    int z = 0;
};

// containerIndex < 0 → inventory slot 1–10; otherwise P2 window index, slot 0-based.
// to == nullptr or empty → Game::use; otherwise Game::useWith.
ActionResult actionUseItem(int containerIndex, int slot, int id, const UseTarget* to = nullptr);
// chase: -1 leave client chase as-is, 0 DontChase, 1 ChaseOpponent (not Game::follow).
ActionResult actionAttack(std::uint32_t creatureId, int chase = -1);
ActionResult actionCancelAttack();
ActionResult actionFollow(std::uint32_t creatureId);
ActionResult actionCancelFollow();
ActionResult actionMoveItem(int containerIndex, int slot, int id, const UseTarget& to, int count);

struct LookTarget {
    int container = -1;
    int slot = -1;
    int id = 0;
    bool hasCreature = false;
    std::uint32_t creatureId = 0;
    bool hasPos = false;
    int x = 0;
    int y = 0;
    int z = 0;
};

struct LookInfo {
    bool valid = false;
    std::string kind;
    std::uint32_t id = 0;
    int count = 0;
    bool hasPos = false;
    int x = 0;
    int y = 0;
    int z = 0;
    int container = -1;
    int slot = -1;
    std::string name;
};

ActionResult actionLook(const LookTarget& target);
LookInfo currentLook();
std::string lastLookJson();
void pumpLookCapture();

ActionResult actionClick(int x, int y, int z);
ActionResult actionUseCorpse(int x, int y, int z, int expectedItemId);
ActionResult actionSetLight(bool enabled, float intensity, float& appliedIntensity);

} // namespace otc

