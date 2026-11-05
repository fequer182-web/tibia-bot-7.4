#include "otc_api/profile_store.h"
#include "otc_api/embedded_calibrations.h"

#include <algorithm>

namespace otc {

std::filesystem::path calibrationPath(const std::wstring& dllDir, const std::string& buildId) {
    const std::filesystem::path dir(dllDir);
    if (!dir.is_absolute() || buildId.empty() ||
        !std::all_of(buildId.begin(), buildId.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-';
        }))
        return {};
    return dir / L"calibrations" / (buildId + ".json");
}

bool loadCalibration(Profile& profile, const std::wstring& dllDir, const std::string& buildId,
                     CalibrationSource* source) {
    if (source)
        *source = CalibrationSource::None;
    const auto path = calibrationPath(dllDir, buildId);
    if (!path.empty()) {
        Profile candidate;
        if (profileFromJsonFile(candidate, path.wstring()) &&
            (candidate.buildId.empty() || candidate.buildId == buildId)) {
            candidate.buildId = buildId;
            profile = std::move(candidate);
            if (source)
                *source = CalibrationSource::External;
            return true;
        }
    }
    if (!loadEmbeddedCalibration(profile, buildId))
        return false;
    if (source)
        *source = CalibrationSource::Embedded;
    return true;
}

bool loadEmbeddedCalibration(Profile& profile, const std::string& buildId) {
    const std::string_view text = embeddedCalibrationJson(buildId);
    if (text.empty())
        return false;
    Profile candidate;
    if (!profileFromJson(candidate, std::string(text)) ||
        (!candidate.buildId.empty() && candidate.buildId != buildId))
        return false;
    candidate.buildId = buildId;
    profile = std::move(candidate);
    return true;
}

bool saveCalibration(const Profile& profile, const std::wstring& dllDir) {
    const auto path = calibrationPath(dllDir, profile.buildId);
    if (path.empty())
        return false;
    std::error_code error;
    std::filesystem::create_directory(path.parent_path(), error);
    if (error)
        return false;
    return profileToJsonFile(profile, path.wstring());
}

} // namespace otc

