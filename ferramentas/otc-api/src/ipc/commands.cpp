#include "otc_api/commands.h"

#include "otc_api/actions.h"
#include "otc_api/corpse.h"
#include "otc_api/container_reader.h"
#include "otc_api/creature_reader.h"
#include "otc_api/logger.h"
#include "otc_api/map_reader.h"
#include "otc_api/pipe_server.h"
#include "otc_api/player_reader.h"

#include <nlohmann/json.hpp>

namespace otc {
namespace {

using nlohmann::json;

void reply(const json& req, const char* event, const json& data) {
    json out = data;
    if (req.contains("requestId"))
        out["requestId"] = req["requestId"];
    PipeServer::instance().emit(event, out.dump());
}

void replyAction(const json& req, const std::string& action, const ActionResult& r,
                 const json& extra = json::object()) {
    json data = {{"ok", r.ok}, {"action", action}};
    if (!r.ok && !r.error.empty())
        data["error"] = r.error;
    if (!r.via.empty())
        data["via"] = r.via;
    if (extra.is_object()) {
        for (auto it = extra.begin(); it != extra.end(); ++it)
            data[it.key()] = it.value();
    }
    reply(req, "action", data);
}

} // namespace

void dispatchCommand(const std::string& line) {
    json j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        LOG("bad json command");
        return;
    }

    const std::string action = j.value("action", "");
    if (action == "ping") {
        reply(j, "pong", {{"ok", true}});
        return;
    }

    if (action == "getState") {
        const std::string what = j.value("what", "player");
        json data;
        if (what == "monsterEnd")
            data = monsterEndState();
        else if (what == "corpse")
            data = corpseState();
        else if (what == "player")
            data = json::parse(playerStatsJson(), nullptr, false);
        else if (what == "inventory")
            data = json::parse(inventoryJson(), nullptr, false);
        else if (what == "containers" || what == "container")
            data = json::parse(containersJson(), nullptr, false);
        else if (what == "creatures" || what == "creature" || what == "battle")
            data = json::parse(creaturesJson(), nullptr, false);
        else if (what == "map" || what == "tiles" || what == "tile")
            data = json::parse(mapJson(), nullptr, false);
        else if (what == "look")
            data = json::parse(lastLookJson(), nullptr, false);
        else if (what == "calibration")
            data = json::parse(calibrationJson(), nullptr, false);
        else
            data = {{"error", "unknown what"}};
        if (data.is_discarded())
            data = json::object();
        reply(j, "state", {{"what", what}, {"data", data}});
        return;
    }

    if (action == "calibration") {
        const std::string cmd = j.value("cmd", "");
        if (cmd == "hints") {
            CalibHints h;
            if (j.contains("health")) {
                h.hasHealth = true;
                h.health = j.value("health", 0);
                h.maxHealth = j.value("maxHealth", h.health);
            }
            if (j.contains("mana")) {
                h.hasMana = true;
                h.mana = j.value("mana", 0);
                h.maxMana = j.value("maxMana", h.mana);
            }
            if (j.contains("level")) {
                h.hasLevel = true;
                h.level = j.value("level", 0);
            }
            if (j.contains("experience")) {
                h.hasExp = true;
                h.experience = j.value("experience", 0ull);
            }
            if (j.contains("capacity")) {
                h.hasCapacity = true;
                h.capacity = j.value("capacity", 0);
            }
            if (j.contains("x") && j.contains("y") && j.contains("z")) {
                h.hasPosition = true;
                h.x = j.value("x", 0);
                h.y = j.value("y", 0);
                h.z = j.value("z", 0);
                h.hasZ = true;
            } else if (j.contains("z")) {
                h.hasZ = true;
                h.z = j.value("z", 0);
            }
            const bool anyPositionBound = j.contains("minX") || j.contains("maxX") ||
                                          j.contains("minY") || j.contains("maxY");
            if (anyPositionBound) {
                h.hasPositionBounds = true;
                if (j.contains("minX") && j["minX"].is_number_integer())
                    h.minX = j["minX"].get<int>();
                if (j.contains("maxX") && j["maxX"].is_number_integer())
                    h.maxX = j["maxX"].get<int>();
                if (j.contains("minY") && j["minY"].is_number_integer())
                    h.minY = j["minY"].get<int>();
                if (j.contains("maxY") && j["maxY"].is_number_integer())
                    h.maxY = j["maxY"].get<int>();
            }
            h.forcePositionRediscovery = j.value("moved", false);
            h.requirePositionMovement = j.value("moving", false);
            if (h.requirePositionMovement)
                h.forcePositionRediscovery = true;
            if (j.value("battle", false) || j.value("swords", false))
                h.expectBattle = true;
            if (j.contains("stateBits"))
                h.expectStateBits = j.value("stateBits", 0u);
            else if (h.expectBattle)
                h.expectStateBits = 128;
            if (j.contains("itemId") ||
                (j.contains("id") && !j.contains("creatureName") && !j.contains("creatureId"))) {
                h.hasItemId = true;
                h.itemId = j.contains("itemId") ? j.value("itemId", 0) : j.value("id", 0);
            }
            if (j.contains("slot"))
                h.itemSlot = j.value("slot", 0);
            h.needInventory = j.value("inventory", false) || h.hasItemId;
            setCalibHints(h);
            if (j.contains("containerName") || j.contains("containerCapacity")) {
                ContainerCalibHints ch;
                ch.name = j.value("containerName", "");
                ch.capacity = j.value("containerCapacity", 0);
                ch.hasItemId = h.hasItemId;
                ch.itemId = h.itemId;
                setContainerCalibHints(ch);
            }
            if (j.contains("creatureName") || j.contains("hpperc") || j.contains("creatureId")) {
                CreatureCalibHints cr;
                cr.name = j.value("creatureName", "");
                if (j.contains("z")) {
                    cr.hasZ = true;
                    cr.z = j.value("z", 0);
                }
                if (j.contains("hpperc") || j.contains("healthPercent")) {
                    cr.hasHpperc = true;
                    cr.hpperc = j.contains("hpperc") ? j.value("hpperc", 0)
                                                    : j.value("healthPercent", 0);
                }
                if (j.contains("creatureId")) {
                    cr.hasId = true;
                    cr.id = j.value("creatureId", 0u);
                }
                setCreatureCalibHints(cr);
            }
            if (j.contains("mapItemId") || j.contains("itemId")) {
                MapCalibHints mh;
                if (j.contains("mapItemId")) {
                    mh.itemId = j.value("mapItemId", 0);
                    mh.hasItemId = mh.itemId > 0;
                } else if (j.contains("itemId")) {
                    mh.itemId = j.value("itemId", 0);
                    mh.hasItemId = mh.itemId > 0;
                }
                setMapCalibHints(mh);
            }
            reply(j, "calibration", json::parse(calibrationJson(), nullptr, false));
            return;
        }
        if (cmd == "scan") {
            const bool ok = runCalibrationScan();
            json data = json::parse(calibrationJson(), nullptr, false);
            data["ok"] = ok;
            reply(j, "calibration", data);
            return;
        }
        if (cmd == "refine") {
            const bool ok = runRefineScan();
            json data = json::parse(calibrationJson(), nullptr, false);
            data["ok"] = ok;
            reply(j, "calibration", data);
            return;
        }
        if (cmd == "scanContainers" || cmd == "containers") {
            const bool ok = runContainerScan();
            json data = json::parse(calibrationJson(), nullptr, false);
            data["ok"] = ok;
            data["message"] = containerCalibMessage();
            reply(j, "calibration", data);
            return;
        }
        if (cmd == "scanCreatures" || cmd == "creatures" || cmd == "battle") {
            const bool ok = runCreatureScan();
            json data = json::parse(calibrationJson(), nullptr, false);
            data["ok"] = ok;
            data["message"] = creatureCalibMessage();
            reply(j, "calibration", data);
            return;
        }
        if (cmd == "scanMap" || cmd == "map" || cmd == "tiles") {
            const bool ok = runMapScan();
            json data = json::parse(calibrationJson(), nullptr, false);
            data["ok"] = ok;
            data["message"] = mapCalibMessage();
            reply(j, "calibration", data);
            return;
        }
        if (cmd == "save") {
            const bool ok = saveCurrentProfile();
            json data = json::parse(calibrationJson(), nullptr, false);
            data["ok"] = ok;
            reply(j, "calibration", data);
            return;
        }
        reply(j, "calibration", {{"error", "unknown cmd"}});
        return;
    }

    if (action == "watchCorpse" || action == "openCorpse") {
        json data = action == "watchCorpse" ? watchCorpse(j) : openCorpse(j);
        data["action"] = action;
        reply(j, "action", data);
        return;
    }
    if (action == "say") {
        std::string mode = "say";
        if (j.contains("mode")) {
            if (j["mode"].is_string())
                mode = j["mode"].get<std::string>();
            else if (j["mode"].is_number_integer())
                mode = std::to_string(j["mode"].get<int>());
        }
        replyAction(j, action, actionSay(j.value("text", ""), mode));
        return;
    }
    if (action == "step") {
        std::string dir;
        if (j.contains("direction") && j["direction"].is_number_integer())
            dir = std::to_string(j["direction"].get<int>());
        else
            dir = j.value("direction", "");
        replyAction(j, action, actionStep(dir));
        return;
    }
    if (action == "stop") {
        replyAction(j, action, actionStop());
        return;
    }
    if (action == "logout") {
        replyAction(j, action, actionLogout());
        return;
    }
    if (action == "setLight") {
        if (!j.contains("enabled") || !j["enabled"].is_boolean()) {
            replyAction(j, action, ActionResult{false, "missing enabled"});
            return;
        }
        float intensity = 1.0f;
        if (j.contains("intensity")) {
            if (!j["intensity"].is_number()) {
                replyAction(j, action, ActionResult{false, "bad intensity"});
                return;
            }
            const double value = j["intensity"].get<double>();
            if (!(value >= 0.0 && value <= 1.0)) {
                replyAction(j, action, ActionResult{false, "bad intensity"});
                return;
            }
            intensity = static_cast<float>(value);
        }
        const bool enabled = j["enabled"].get<bool>();
        float applied = 0.0f;
        const auto result = actionSetLight(enabled, intensity, applied);
        json extra = {{"enabled", enabled}};
        if (result.ok)
            extra["intensity"] = applied;
        replyAction(j, action, result, extra);
        return;
    }
    if (action == "walkTo") {
        int x = 0;
        int y = 0;
        int z = 0;
        auto readCoord = [](const json& j, const char* key, int& out) {
            if (!j.contains(key) || !j[key].is_number())
                return false;
            if (j[key].is_number_integer())
                out = j[key].get<int>();
            else
                out = static_cast<int>(j[key].get<double>());
            return true;
        };
        if (!readCoord(j, "x", x) || !readCoord(j, "y", y) || !readCoord(j, "z", z)) {
            replyAction(j, action, ActionResult{false, "missing coordinates"});
            return;
        }
        replyAction(j, action, actionWalkTo(x, y, z));
        return;
    }
    if (action == "useItem") {
        int container = -1;
        int slot = -1;
        int id = 0;
        auto readInt = [](const json& o, const char* key, int& out) {
            if (!o.contains(key) || !o[key].is_number())
                return false;
            if (o[key].is_number_integer())
                out = o[key].get<int>();
            else
                out = static_cast<int>(o[key].get<double>());
            return true;
        };
        if (j.contains("from") && j["from"].is_object()) {
            const auto& from = j["from"];
            readInt(from, "container", container);
            readInt(from, "slot", slot);
            readInt(from, "id", id);
        }
        readInt(j, "container", container);
        readInt(j, "slot", slot);
        readInt(j, "id", id);
        if (slot < 0) {
            replyAction(j, action, ActionResult{false, "missing slot"});
            return;
        }
        UseTarget to;
        bool hasTo = false;
        const json* t = nullptr;
        if (j.contains("to") && j["to"].is_object())
            t = &j["to"];
        else if (j.contains("to") && j["to"].is_boolean() && j["to"].get<bool>()) {
            to.self = true;
            hasTo = true;
        }
        if (t) {
            hasTo = true;
            if (t->value("self", false))
                to.self = true;
            if (t->contains("creatureId")) {
                to.hasCreature = true;
                if ((*t)["creatureId"].is_number_unsigned())
                    to.creatureId = (*t)["creatureId"].get<std::uint32_t>();
                else
                    to.creatureId = static_cast<std::uint32_t>(t->value("creatureId", 0));
            }
            readInt(*t, "container", to.container);
            readInt(*t, "slot", to.slot);
            readInt(*t, "id", to.id);
            int x = 0, y = 0, z = 0;
            if (readInt(*t, "x", x) && readInt(*t, "y", y) && readInt(*t, "z", z)) {
                to.hasPos = true;
                to.x = x;
                to.y = y;
                to.z = z;
            }
        }
        replyAction(j, action, actionUseItem(container, slot, id, hasTo ? &to : nullptr));
        return;
    }
    if (action == "moveItem") {
        int container = -1;
        int slot = -1;
        int id = 0;
        int count = 0;
        auto readInt = [](const json& o, const char* key, int& out) {
            if (!o.contains(key) || !o[key].is_number())
                return false;
            if (o[key].is_number_integer())
                out = o[key].get<int>();
            else
                out = static_cast<int>(o[key].get<double>());
            return true;
        };
        if (j.contains("from") && j["from"].is_object()) {
            const auto& from = j["from"];
            readInt(from, "container", container);
            readInt(from, "slot", slot);
            readInt(from, "id", id);
        }
        readInt(j, "container", container);
        readInt(j, "slot", slot);
        readInt(j, "id", id);
        readInt(j, "count", count);
        if (slot < 0) {
            replyAction(j, action, ActionResult{false, "missing slot"});
            return;
        }
        UseTarget to;
        bool hasTo = false;
        if (j.contains("to") && j["to"].is_object()) {
            hasTo = true;
            const auto& t = j["to"];
            if (t.contains("creatureId")) {
                to.hasCreature = true;
                if (t["creatureId"].is_number_unsigned())
                    to.creatureId = t["creatureId"].get<std::uint32_t>();
                else
                    to.creatureId = static_cast<std::uint32_t>(t.value("creatureId", 0));
            }
            readInt(t, "container", to.container);
            readInt(t, "slot", to.slot);
            readInt(t, "id", to.id);
            int x = 0, y = 0, z = 0;
            if (readInt(t, "x", x) && readInt(t, "y", y) && readInt(t, "z", z)) {
                to.hasPos = true;
                to.x = x;
                to.y = y;
                to.z = z;
            }
        }
        if (!hasTo || (!to.hasPos && to.slot < 0)) {
            replyAction(j, action, ActionResult{false, "missing dest"});
            return;
        }
        replyAction(j, action, actionMoveItem(container, slot, id, to, count));
        return;
    }
    if (action == "attack") {
        std::uint32_t creatureId = 0;
        if (j.contains("creatureId")) {
            if (j["creatureId"].is_number_unsigned())
                creatureId = j["creatureId"].get<std::uint32_t>();
            else if (j["creatureId"].is_number())
                creatureId = static_cast<std::uint32_t>(j["creatureId"].get<double>());
        }
        int chase = -1;
        if (j.contains("follow") && !j["follow"].is_null()) {
            if (j["follow"].is_boolean())
                chase = j["follow"].get<bool>() ? 1 : 0;
            else if (j["follow"].is_number())
                chase = j["follow"].get<double>() != 0 ? 1 : 0;
        }
        json extra;
        if (chase >= 0)
            extra["follow"] = chase == 1;
        replyAction(j, action, actionAttack(creatureId, chase), extra);
        return;
    }
    if (action == "cancelAttack") {
        replyAction(j, action, actionCancelAttack());
        return;
    }
    if (action == "follow") {
        std::uint32_t creatureId = 0;
        if (j.contains("creatureId")) {
            if (j["creatureId"].is_number_unsigned())
                creatureId = j["creatureId"].get<std::uint32_t>();
            else if (j["creatureId"].is_number())
                creatureId = static_cast<std::uint32_t>(j["creatureId"].get<double>());
        }
        replyAction(j, action, actionFollow(creatureId));
        return;
    }
    if (action == "cancelFollow") {
        replyAction(j, action, actionCancelFollow());
        return;
    }
    if (action == "look") {
        LookTarget t;
        auto readInt = [](const json& o, const char* key, int& out) {
            if (!o.contains(key) || !o[key].is_number())
                return false;
            if (o[key].is_number_integer())
                out = o[key].get<int>();
            else
                out = static_cast<int>(o[key].get<double>());
            return true;
        };
        readInt(j, "container", t.container);
        readInt(j, "slot", t.slot);
        readInt(j, "id", t.id);
        int x = 0, y = 0, z = 0;
        if (readInt(j, "x", x) && readInt(j, "y", y) && readInt(j, "z", z)) {
            t.hasPos = true;
            t.x = x;
            t.y = y;
            t.z = z;
        }
        if (j.contains("creatureId")) {
            t.hasCreature = true;
            if (j["creatureId"].is_number_unsigned())
                t.creatureId = j["creatureId"].get<std::uint32_t>();
            else if (j["creatureId"].is_number())
                t.creatureId = static_cast<std::uint32_t>(j["creatureId"].get<double>());
        }
        const ActionResult r = actionLook(t);
        json extra;
        if (r.ok) {
            const LookInfo info = currentLook();
            if (info.valid) {
                extra = json::parse(lastLookJson(), nullptr, false);
                if (extra.is_discarded())
                    extra = json::object();
            }
        }
        replyAction(j, action, r, extra);
        return;
    }
    if (action == "click") {
        int x = 0;
        int y = 0;
        int z = 0;
        auto readCoord = [](const json& o, const char* key, int& out) {
            if (!o.contains(key) || !o[key].is_number())
                return false;
            if (o[key].is_number_integer())
                out = o[key].get<int>();
            else
                out = static_cast<int>(o[key].get<double>());
            return true;
        };
        if (!readCoord(j, "x", x) || !readCoord(j, "y", y) || !readCoord(j, "z", z)) {
            replyAction(j, action, ActionResult{false, "missing coordinates"});
            return;
        }
        replyAction(j, action, actionClick(x, y, z));
        return;
    }

    LOG("ignored action=%s", action.c_str());
}

} // namespace otc

