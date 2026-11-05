#pragma once
#include <nlohmann/json.hpp>
namespace otc {
void startCorpseReader();
nlohmann::json corpseState();
nlohmann::json monsterEndState();
nlohmann::json watchCorpse(const nlohmann::json& request);
nlohmann::json openCorpse(const nlohmann::json& request);
}

