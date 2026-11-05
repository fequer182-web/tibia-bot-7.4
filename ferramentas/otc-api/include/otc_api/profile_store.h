#pragma once

#include "otc_api/profile.h"
#include <filesystem>

namespace otc {

enum class CalibrationSource { None, External, Embedded };

// Only the loaded DLL's absolute directory is accepted; never fall back to CWD/game.
std::filesystem::path calibrationPath(const std::wstring& dllDir, const std::string& buildId);
bool loadCalibration(Profile& profile, const std::wstring& dllDir, const std::string& buildId,
                     CalibrationSource* source = nullptr);
bool loadEmbeddedCalibration(Profile& profile, const std::string& buildId);
bool saveCalibration(const Profile& profile, const std::wstring& dllDir);

} // namespace otc

