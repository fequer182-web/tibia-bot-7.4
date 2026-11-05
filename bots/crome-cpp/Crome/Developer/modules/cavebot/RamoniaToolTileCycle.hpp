#pragma once
#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>

namespace crome {
struct RamoniaToolTileResult {
    bool moved{};
    bool used{};
    bool transportFailed{};
    std::uint32_t blockerId{};
    std::string error;
};

template<class Request>
RamoniaToolTileResult RunRamoniaToolTileCycle(
    std::uint32_t toolId, std::uint32_t targetId,
    std::uint32_t x, std::uint32_t y, std::uint32_t z,
    Request&& request) {
    RamoniaToolTileResult result;
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
            result.error=response.value("error",std::string("tool_tile_failed"));
            return false;
        }
        return true;
    };
    try {
        if (!toolId || toolId>0xFFFFu) {
            result.error="invalid_tool_id";
            return result;
        }
        if (!targetId || targetId>0xFFFFu) {
            result.error="target_item_id_required";
            return result;
        }
        if (!x || !y || z>15u) {
            result.error="invalid_position";
            return result;
        }
        Json uncover;
        if (!send({{"cmd","UNCOVER_TILE_TARGET_AT"},{"itemId",targetId},
                   {"x",x},{"y",y},{"z",z}},uncover))
            return result;
        if (uncover.value("moved",false)) {
            result.moved=true;
            result.blockerId=uncover.at("blocker").at("id").get<std::uint32_t>();
            return result; // A new cycle must re-read Tile + TopMove.
        }
        if (!uncover.value("exposed",false)) {
            result.error="target_not_exposed";
            return result;
        }
        Json use;
        // Exposure is checked again on the game thread immediately before
        // the existing useWith backend, closing the gap between requests.
        if (send({{"cmd","USE_WITH_TILE_AT"},{"toolItemId",toolId},
                  {"targetItemId",targetId},{"requireExposed",1},
                  {"x",x},{"y",y},{"z",z}},use))
            result.used=true;
        return result;
    } catch (const Json::exception&) {
        result.error="invalid_bridge_response";
        return result;
    }
}
} // namespace crome
