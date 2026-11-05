#pragma once
#include "core/profile/BuildProfile.hpp"
#include <filesystem>
namespace crome { class ProfileLoader { public: static BuildProfile load(const std::filesystem::path& path); }; }
