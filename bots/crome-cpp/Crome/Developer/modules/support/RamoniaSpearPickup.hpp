#pragma once
#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>

namespace crome {

struct RamoniaSpearCycleResult {
    bool moved{};
    bool uncovered{};
    bool transportFailed{};
    std::uint32_t destinationSlot{};
    std::uint32_t blockerId{};
    std::string error;
};

// One cycle performs at most one game action. No target/stack pointer survives
// between cycles; the bridge re-reads TopMove for every pickup or uncover.
template<class Request>
RamoniaSpearCycleResult RunRamoniaSpearPickupCycle(
    std::uint32_t itemId, Request&& request) {
    RamoniaSpearCycleResult result;
    using Json=nlohmann::json;
    auto send=[&](const Json& command,Json& response) {
        if (!request(command,response)) {
            result.transportFailed=true;
            result.error="pipe_unavailable";
            return false;
        }
        if (!response.is_object()) {
            result.error="invalid_bridge_response";
            return false;
        }
        if (!response.value("ok",false)) {
            result.error=response.value("error",std::string("pickup_failed"));
            return false;
        }
        return true;
    };
    try {
        if (!itemId || itemId>0xFFFFu) {
            result.error="invalid_item_id";
            return result;
        }
        Json hands;
        if (!send({{"cmd","GET_HANDS"}},hands)) return result;
        bool usableHand=false;
        for (const char* side:{"right","left"}) {
            const auto& hand=hands.at(side);
            if (!hand.at("occupied").get<bool>() ||
                hand.value("id",0u)==itemId)
                usableHand=true;
        }
        if (!usableHand) {
            result.error="both_hands_occupied_by_other_items";
            return result;
        }

        Json pickup;
        if (send({{"cmd","MOVE_NEARBY_ITEM_TO_HAND"},{"itemId",itemId}},pickup)) {
            result.moved=pickup.value("moved",false);
            result.destinationSlot=pickup.value("destinationSlot",0u);
            if (!result.moved) result.error="pickup_not_dispatched";
            return result;
        }
        if (result.transportFailed || result.error!="item_covered")
            return result;

        // Use only coordinates from the game-thread-validated pickup refusal.
        // Diagnostic stackIndex/accessibility fields never authorize uncover.
        const auto& source=pickup.at("source");
        const auto x=source.at("x").get<std::int64_t>();
        const auto y=source.at("y").get<std::int64_t>();
        const auto z=source.at("z").get<std::int64_t>();
        if (x<=0 || x>0xFFFFFFFFLL || y<=0 || y>0xFFFFFFFFLL ||
            z<0 || z>15 || pickup.at("itemId").get<std::uint32_t>()!=itemId) {
            result.error="invalid_pickup_source";
            return result;
        }
        Json uncover;
        result.error.clear();
        if (!send({{"cmd","UNCOVER_TILE_TARGET_AT"},
                   {"x",x},{"y",y},{"z",z},{"itemId",itemId}},uncover))
            return result;
        result.uncovered=uncover.value("moved",false);
        if (result.uncovered)
            result.blockerId=uncover.at("blocker").at("id").get<std::uint32_t>();
        else if (!uncover.value("exposed",false))
            result.error="uncover_not_dispatched";
        // Even exposed=true stops this cycle. Pickup revalidates on the next.
        return result;
    } catch (const Json::exception&) {
        result.error="invalid_bridge_response";
        return result;
    }
}

} // namespace crome
