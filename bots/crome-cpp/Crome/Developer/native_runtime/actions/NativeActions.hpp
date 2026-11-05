#pragma once
#include "IActions.hpp"
#include <functional>
#include <string>

namespace crome {

// x64-safe bridge: signatures/calling conventions are supplied only after a
// specific build has been validated. No action silently falls back to keyboard/chat.
struct NativeActionTable {
    std::function<bool(std::uint32_t)> attack;
    std::function<bool(std::uint32_t)> follow;
    std::function<bool(Position)> walkTo;
    std::function<bool(Position)> look;
    std::function<bool(const Item&)> use;
    std::function<bool(const Item&, Position)> useWith;
    std::function<bool(const Item&, Position)> useWithGround;
    std::function<bool(std::uint32_t, Position)> useGroundObject;
    std::function<bool(const Item&, Position, Position, std::uint32_t)> move;
    std::function<bool(Position)> open;
    std::function<bool(std::size_t)> close;
    std::function<bool(std::string_view)> castSpell;
    std::function<bool(std::string_view)> sayDefault;
};

class NativeActions final : public IActions {
public:
    explicit NativeActions(NativeActionTable table) : table_(std::move(table)) {}
    bool attack(std::uint32_t id) override { return table_.attack ? table_.attack(id) : false; }
    bool follow(std::uint32_t id) override { return table_.follow ? table_.follow(id) : false; }
    bool walkTo(Position p) override { return table_.walkTo ? table_.walkTo(p) : false; }
    bool look(Position p) override { return table_.look ? table_.look(p) : false; }
    bool use(const Item& i) override { return table_.use ? table_.use(i) : false; }
    bool useWith(const Item& i, Position p) override { return table_.useWith ? table_.useWith(i, p) : false; }
    bool useWithGround(const Item& i, Position p) override { return table_.useWithGround ? table_.useWithGround(i, p) : false; }
    bool useGroundObject(std::uint32_t id, Position p) override { return table_.useGroundObject ? table_.useGroundObject(id, p) : false; }
    bool move(const Item& i, Position f, Position t, std::uint32_t c) override { return table_.move ? table_.move(i, f, t, c) : false; }
    bool open(Position p) override { return table_.open ? table_.open(p) : false; }
    bool close(std::size_t i) override { return table_.close ? table_.close(i) : false; }
    bool castSpell(std::string_view s) override { return table_.castSpell ? table_.castSpell(s) : false; }
    bool sayDefault(std::string_view s) override { return table_.sayDefault ? table_.sayDefault(s) : false; }
private:
    NativeActionTable table_;
};

} // namespace crome
