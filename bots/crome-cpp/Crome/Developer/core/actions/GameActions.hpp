#pragma once
#include "core/sdk/Snapshots.hpp"
#include <cstdint>
#include <string>
namespace crome {
struct ActionResult { bool ok{}; std::string message; static ActionResult success(std::string m={}){return {true,std::move(m)};} static ActionResult fail(std::string m){return {false,std::move(m)};} };
class IGameActions { public: virtual ~IGameActions()=default; virtual ActionResult walkTo(Position)=0; virtual ActionResult attack(uint32_t)=0; virtual ActionResult cancelAttack()=0; virtual ActionResult follow(uint32_t)=0; virtual ActionResult cancelFollow()=0; virtual ActionResult say(const std::string&)=0; virtual ActionResult use(uint32_t)=0; virtual ActionResult useWithCreature(uint32_t,uint32_t)=0; virtual ActionResult useWithGround(uint32_t,Position)=0; virtual ActionResult moveItem(uint32_t,int,int)=0; virtual ActionResult look(Position)=0; virtual ActionResult logout()=0; };
}
