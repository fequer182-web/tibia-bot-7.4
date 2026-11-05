#pragma once
#include "../profiles/MiniJson.hpp"
#include "../sdk/Position.hpp"

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

namespace crome {

enum class RouteWaypointType { Walk, Wait, Label, Goto, Use, UseWithSelf, UseWithGround, Rope, Shovel, Ladder, Fish, Say };

struct RouteWaypoint {
    RouteWaypointType type{RouteWaypointType::Walk};
    Position position{};
    std::uint32_t waitMs{};
    std::string label;
    std::string text;
    // -1 = use RouteConfig::reachDistance.
    int reachDistance{-1};
    std::uint32_t itemId{};
    // Some actions (notably "use") may target a concrete map tile. This keeps
    // legacy inventory-use nodes compatible while preserving coordinates written
    // by the Cavebot editor.
    bool hasPosition{false};
    // 0 = use RouteConfig::actionWaitMs.
    std::uint32_t actionWaitMs{};
    // Ground/tool nodes may resolve Z from the player at execution time.
    bool useCurrentZ{false};
};

struct RouteConfig {
    bool enabled{false};
    bool loop{true};
    int reachDistance{0};
    std::uint32_t timeoutMs{15000};
    std::uint32_t stuckMs{3500};
    std::uint32_t retryDelayMs{650};
    int maxRetries{2};
    std::uint32_t actionWaitMs{450};
    // Editable defaults. Individual nodes can still override item_id.
    std::uint32_t ropeItemId{3003};
    std::uint32_t shovelItemId{3457};
    std::uint32_t fishingRodItemId{3483};
    std::vector<RouteWaypoint> waypoints;
};

inline std::string readRouteText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open route: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline int jsonInt(const json::Value* value, int fallback) {
    return value && value->isNumber() ? static_cast<int>(value->number()) : fallback;
}

inline std::uint32_t jsonU32(const json::Value* value, std::uint32_t fallback) {
    if (!value || !value->isNumber() || value->number() < 0) return fallback;
    return static_cast<std::uint32_t>(value->number());
}

inline RouteConfig loadRouteConfig(const std::string& path) {
    const auto root = json::parse(readRouteText(path));
    if (!root.isObject()) throw std::runtime_error("route root must be object");

    RouteConfig out;
    if (const auto* v = root.get("enabled"); v && v->isBool()) out.enabled = v->boolean();
    if (const auto* v = root.get("loop"); v && v->isBool()) out.loop = v->boolean();
    out.reachDistance = jsonInt(root.get("reach_distance"), out.reachDistance);
    out.timeoutMs = jsonU32(root.get("timeout_ms"), out.timeoutMs);
    out.stuckMs = jsonU32(root.get("stuck_ms"), out.stuckMs);
    out.retryDelayMs = jsonU32(root.get("retry_delay_ms"), out.retryDelayMs);
    out.maxRetries = jsonInt(root.get("max_retries"), out.maxRetries);
    out.actionWaitMs = jsonU32(root.get("action_wait_ms"), out.actionWaitMs);
    if (const auto* tools = root.get("tool_ids"); tools && tools->isObject()) {
        out.ropeItemId = jsonU32(tools->get("rope"), out.ropeItemId);
        out.shovelItemId = jsonU32(tools->get("shovel"), out.shovelItemId);
        out.fishingRodItemId = jsonU32(tools->get("fishing_rod"), out.fishingRodItemId);
    }

    const auto* points = root.get("waypoints");
    if (!points || !points->isArray()) throw std::runtime_error("route needs waypoints array");

    for (const auto& item : points->array()) {
        if (!item.isObject()) continue;
        std::string type = "walk";
        if (const auto* t = item.get("type"); t && t->isString()) type = t->string();

        if (type == "wait") {
            RouteWaypoint w;
            w.type = RouteWaypointType::Wait;
            w.waitMs = jsonU32(item.get("ms"), 1000);
            out.waypoints.push_back(w);
            continue;
        }

        if (type == "label") {
            const auto* name = item.get("name");
            if (!name || !name->isString() || name->string().empty()) continue;
            RouteWaypoint w;
            w.type = RouteWaypointType::Label;
            w.label = name->string();
            out.waypoints.push_back(std::move(w));
            continue;
        }

        if (type == "goto") {
            const auto* label = item.get("label");
            if (!label || !label->isString() || label->string().empty()) continue;
            RouteWaypoint w;
            w.type = RouteWaypointType::Goto;
            w.label = label->string();
            out.waypoints.push_back(std::move(w));
            continue;
        }

        if (type == "say") {
            const auto* text = item.get("text");
            if (!text || !text->isString() || text->string().empty()) continue;
            RouteWaypoint w;
            w.type = RouteWaypointType::Say;
            w.text = text->string();
            w.actionWaitMs = jsonU32(item.get("after_ms"), 0);
            out.waypoints.push_back(std::move(w));
            continue;
        }

        if (type == "use" || type == "use_with_self") {
            const auto itemId = jsonU32(item.get("item_id"), 0);
            if (!itemId) continue;
            RouteWaypoint w;
            w.type = type == "use" ? RouteWaypointType::Use : RouteWaypointType::UseWithSelf;
            w.itemId = itemId;
            w.actionWaitMs = jsonU32(item.get("after_ms"), 0);

            // The Cavebot UI writes x/y/z for "use" waypoints. Older runtime
            // versions discarded those coordinates and consequently used the
            // inventory item instead of the object on the selected tile.
            if (w.type == RouteWaypointType::Use) {
                const auto* x = item.get("x");
                const auto* y = item.get("y");
                const auto* z = item.get("z");
                if (x && y && z && x->isNumber() && y->isNumber()) {
                    w.useCurrentZ = z->isString() && z->string() == "current";
                    if (w.useCurrentZ || z->isNumber()) {
                        w.position = Position{
                            static_cast<std::int32_t>(x->number()),
                            static_cast<std::int32_t>(y->number()),
                            w.useCurrentZ ? 0 : static_cast<std::int32_t>(z->number())
                        };
                        w.hasPosition = true;
                    }
                }
            }

            out.waypoints.push_back(w);
            continue;
        }

        if (type == "use_with_ground" || type == "rope" || type == "shovel" || type == "ladder" || type == "fish") {
            const auto* x = item.get("x");
            const auto* y = item.get("y");
            const auto* z = item.get("z");
            if (!x || !y || !z || !x->isNumber() || !y->isNumber()) continue;

            RouteWaypoint w;
            if (type == "rope") {
                w.type = RouteWaypointType::Rope;
                w.itemId = jsonU32(item.get("item_id"), out.ropeItemId);
            } else if (type == "shovel") {
                w.type = RouteWaypointType::Shovel;
                w.itemId = jsonU32(item.get("item_id"), out.shovelItemId);
            } else if (type == "ladder") {
                w.type = RouteWaypointType::Ladder;
                w.itemId = jsonU32(item.get("item_id"), 0);
            } else if (type == "fish") {
                w.type = RouteWaypointType::Fish;
                w.itemId = jsonU32(item.get("item_id"), out.fishingRodItemId);
            } else {
                w.type = RouteWaypointType::UseWithGround;
                w.itemId = jsonU32(item.get("item_id"), 0);
            }
            if (!w.itemId) continue;

            w.useCurrentZ = z->isString() && z->string() == "current";
            if (!w.useCurrentZ && !z->isNumber()) continue;
            w.position = Position{
                static_cast<std::int32_t>(x->number()),
                static_cast<std::int32_t>(y->number()),
                w.useCurrentZ ? 0 : static_cast<std::int32_t>(z->number())
            };
            w.actionWaitMs = jsonU32(item.get("after_ms"), 0);
            out.waypoints.push_back(w);
            continue;
        }

        const auto* x = item.get("x");
        const auto* y = item.get("y");
        const auto* z = item.get("z");
        if (!x || !y || !z || !x->isNumber() || !y->isNumber() || !z->isNumber()) continue;

        RouteWaypoint w;
        w.type = RouteWaypointType::Walk;
        w.position = Position{
            static_cast<std::int32_t>(x->number()),
            static_cast<std::int32_t>(y->number()),
            static_cast<std::int32_t>(z->number())
        };
        w.reachDistance = jsonInt(item.get("reach_distance"), -1);
        if (w.reachDistance < -1) w.reachDistance = -1;
        out.waypoints.push_back(w);
    }

    if (out.reachDistance < 0) out.reachDistance = 0;
    if (out.timeoutMs < 1000) out.timeoutMs = 1000;
    if (out.stuckMs < 750) out.stuckMs = 750;
    if (out.maxRetries < 0) out.maxRetries = 0;
    if (out.actionWaitMs < 50) out.actionWaitMs = 50;
    return out;
}

inline bool saveRouteConfig(const std::string& path, const RouteConfig& route) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;

    f << "{\n"
      << "  \"enabled\": " << (route.enabled ? "true" : "false") << ",\n"
      << "  \"loop\": " << (route.loop ? "true" : "false") << ",\n"
      << "  \"reach_distance\": " << route.reachDistance << ",\n"
      << "  \"timeout_ms\": " << route.timeoutMs << ",\n"
      << "  \"stuck_ms\": " << route.stuckMs << ",\n"
      << "  \"retry_delay_ms\": " << route.retryDelayMs << ",\n"
      << "  \"max_retries\": " << route.maxRetries << ",\n"
      << "  \"action_wait_ms\": " << route.actionWaitMs << ",\n"
      << "  \"tool_ids\": { \"rope\": " << route.ropeItemId
      << ", \"shovel\": " << route.shovelItemId
      << ", \"fishing_rod\": " << route.fishingRodItemId << " },\n"
      << "  \"waypoints\": [\n";

    for (std::size_t i = 0; i < route.waypoints.size(); ++i) {
        const auto& w = route.waypoints[i];
        f << "    ";
        if (w.type == RouteWaypointType::Wait) {
            f << "{ \"type\": \"wait\", \"ms\": " << w.waitMs << " }";
        } else if (w.type == RouteWaypointType::Label) {
            f << "{ \"type\": \"label\", \"name\": \"" << w.label << "\" }";
        } else if (w.type == RouteWaypointType::Goto) {
            f << "{ \"type\": \"goto\", \"label\": \"" << w.label << "\" }";
        } else if (w.type == RouteWaypointType::Say) {
            f << "{ \"type\": \"say\", \"text\": \"" << w.text << "\"";
            if (w.actionWaitMs > 0) f << ", \"after_ms\": " << w.actionWaitMs;
            f << " }";
        } else if (w.type == RouteWaypointType::Use || w.type == RouteWaypointType::UseWithSelf) {
            f << "{ \"type\": \""
              << (w.type == RouteWaypointType::Use ? "use" : "use_with_self")
              << "\", \"item_id\": " << w.itemId;
            if (w.type == RouteWaypointType::Use && w.hasPosition) {
                f << ", \"x\": " << w.position.x
                  << ", \"y\": " << w.position.y;
                if (w.useCurrentZ) f << ", \"z\": \"current\"";
                else f << ", \"z\": " << w.position.z;
            }
            if (w.actionWaitMs > 0) f << ", \"after_ms\": " << w.actionWaitMs;
            f << " }";
        } else if (w.type == RouteWaypointType::UseWithGround ||
                   w.type == RouteWaypointType::Rope ||
                   w.type == RouteWaypointType::Shovel ||
                   w.type == RouteWaypointType::Ladder ||
                   w.type == RouteWaypointType::Fish) {
            const char* typeName = "use_with_ground";
            if (w.type == RouteWaypointType::Rope) typeName = "rope";
            else if (w.type == RouteWaypointType::Shovel) typeName = "shovel";
            else if (w.type == RouteWaypointType::Ladder) typeName = "ladder";
            else if (w.type == RouteWaypointType::Fish) typeName = "fish";
            f << "{ \"type\": \"" << typeName
              << "\", \"item_id\": " << w.itemId
              << ", \"x\": " << w.position.x
              << ", \"y\": " << w.position.y;
            if (w.useCurrentZ) f << ", \"z\": \"current\"";
            else f << ", \"z\": " << w.position.z;
            if (w.actionWaitMs > 0) f << ", \"after_ms\": " << w.actionWaitMs;
            f << " }";
        } else {
            f << "{ \"type\": \"walk\", \"x\": " << w.position.x
              << ", \"y\": " << w.position.y
              << ", \"z\": " << w.position.z;
            if (w.reachDistance >= 0)
                f << ", \"reach_distance\": " << w.reachDistance;
            f << " }";
        }
        if (i + 1 != route.waypoints.size()) f << ',';
        f << "\n";
    }

    f << "  ]\n}\n";
    return static_cast<bool>(f);
}

} // namespace crome
