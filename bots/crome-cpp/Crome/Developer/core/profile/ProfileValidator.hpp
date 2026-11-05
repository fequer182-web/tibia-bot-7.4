#pragma once
#include "core/profile/BuildProfile.hpp"
#include <string>
#include <vector>
namespace crome { struct ValidationReport { bool usable{}; std::vector<std::string> errors,warnings; }; class ProfileValidator { public: static ValidationReport validate(const BuildProfile&); }; }
