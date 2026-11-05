#pragma once
#include <nlohmann/json.hpp>
namespace crome { using Json=nlohmann::json; inline Json ok(Json data=Json::object()){return {{"ok",true},{"data",std::move(data)}};} inline Json error(const std::string& e){return {{"ok",false},{"error",e}};} }
