#pragma once
#include "../profiles/ProfileConfig.hpp"
#include "WindowsProcess.hpp"
#include <optional>
#include <string>

#if defined(_WIN32)
namespace crome::win {

struct SelectedProfileProcess {
    const profiles::RuntimeProfile* runtimeProfile{};
    ProcessInfo process;
};

inline std::optional<SelectedProfileProcess> selectConfiguredProcess(
    const profiles::ProfilesFile& config,
    const std::string& overrideProfile = {}) {

    auto tryProfile = [](const profiles::RuntimeProfile& p) -> std::optional<SelectedProfileProcess> {
        if (!p.enabled) return std::nullopt;
        auto info = findProcess(p.executables);
        if (!info) return std::nullopt;
        if (p.moduleSize && info->moduleSize != *p.moduleSize) return std::nullopt;
        return SelectedProfileProcess{&p, *info};
    };

    const std::string wanted = !overrideProfile.empty() ? overrideProfile : config.activeProfile;
    if (!wanted.empty() && wanted != "auto") {
        const auto* p = profiles::findProfileById(config, wanted);
        if (!p || !p->enabled) return std::nullopt;
        return tryProfile(*p);
    }

    for (const auto& p : config.profiles) {
        if (auto hit = tryProfile(p)) return hit;
    }
    return std::nullopt;
}

} // namespace crome::win
#endif
