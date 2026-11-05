#include "core/profile/ProfileRegistry.hpp"
#include "core/profile/ProfileValidator.hpp"
#include "core/profile/Capabilities.hpp"
#include "ui/DiagnosticsModel.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static fs::path executableBaseDir(const char* argv0) {
    std::error_code ec;
    fs::path p = fs::absolute(fs::path(argv0), ec);
    if (ec) return fs::current_path();
    return p.parent_path();
}

int main(int argc, char** argv) {
    std::cout << "============================================================\n";
    std::cout << "              Crome Universal v6.1\n";
    std::cout << "        Profile/Core bootstrap executable\n";
    std::cout << "============================================================\n\n";

    const fs::path base = executableBaseDir(argv[0]);
    fs::path profilesDir = base / "profiles";

    if (argc >= 2) {
        profilesDir = fs::path(argv[1]);
    }

    crome::ProfileRegistry registry;
    registry.loadDirectory(profilesDir);

    if (registry.all().empty()) {
        std::cerr << "[ERRO] Nenhum profile JSON encontrado em:\n  "
                  << profilesDir.string() << "\n\n";
        std::cerr << "Coloque os OTCLab_BuildProfile_*.json na pasta profiles.\n";
        return 2;
    }

    std::cout << "[OK] Profiles carregados: " << registry.all().size() << "\n";
    std::cout << "[DIR] " << profilesDir.string() << "\n\n";

    std::size_t usable = 0;
    for (const auto& profile : registry.all()) {
        const auto report = crome::ProfileValidator::validate(profile);
        const auto caps = crome::Capabilities::from(profile);

        std::cout << "------------------------------------------------------------\n";
        std::cout << crome::diagnosticsText(profile);
        std::cout << " Validator: " << (report.usable ? "USABLE" : "INVALID") << "\n";
        std::cout << " Missing capabilities: " << caps.missing.size() << "\n";

        for (const auto& warning : report.warnings)
            std::cout << " WARN: " << warning << "\n";
        for (const auto& error : report.errors)
            std::cout << " ERROR: " << error << "\n";

        if (report.usable) ++usable;
    }

    std::cout << "\n============================================================\n";
    std::cout << "Profiles utilizaveis: " << usable << "/" << registry.all().size() << "\n";
    std::cout << "Core preparado para receber novos BuildProfile v2.\n";
    std::cout << "============================================================\n";

    return usable ? 0 : 1;
}
