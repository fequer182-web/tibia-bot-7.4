#pragma once
#include "../sdk/Item.hpp"
#include "../sdk/Position.hpp"
#include <cstdint>
#include <string_view>

namespace crome {
class IActions {
public:
    virtual ~IActions() = default;
    virtual bool attack(std::uint32_t creatureId) = 0;
    virtual bool follow(std::uint32_t creatureId) = 0;
    virtual bool walkTo(Position p) = 0;
    virtual bool look(Position p) = 0;
    virtual bool use(const Item& item) = 0;
    virtual bool useWith(const Item& item, Position p) = 0;
    virtual bool useWithGround(const Item& item, Position p) = 0;
    virtual bool useGroundObject(std::uint32_t itemId, Position p) = 0;
    virtual bool move(const Item& item, Position from, Position to, std::uint32_t count) = 0;
    virtual bool open(Position p) = 0;
    virtual bool close(std::size_t containerIndex) = 0;
    virtual bool castSpell(std::string_view spell) = 0;
    virtual bool sayDefault(std::string_view text) = 0;
};
}
