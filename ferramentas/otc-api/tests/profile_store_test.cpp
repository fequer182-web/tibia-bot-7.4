#include "otc_api/profile_store.h"
#include "otc_api/player_recovery.h"

#include <Windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    fs::path root;
    const auto previousCwd = fs::current_path();
    try {
        root = fs::temp_directory_path() /
            ("otc-profile-store-test-" + std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(GetTickCount64()));
        require(fs::create_directory(root), "unique test directory");
        const auto dll = root / L"pacote com espaços";
        const auto game = root / L"game";
        fs::create_directory(dll);
        fs::create_directories(game / L"otc-api");
        fs::current_path(game);

        otc::Profile original;
        original.buildId = "pe-test-build";
        original.chain.push_back({1234, true});
        original.hasPosition = true;
        original.positionOffset = -12;
        original.inventory.present = true;
        original.inventory.ptrSize = 4;
        original.actions.present = true;
        original.actions.gameRva = 100;
        original.actions.attackingCreatureOffset = 8;
        original.actions.safeLogoutRva = 150;
        original.actions.ambientLightRva = 200;
        original.actions.ambientLightGetRva = 300;
        original.chain.push_back({1200, false});
        const auto recovery = otc::gamePlayerRecoveryChain(original, 100);
        require(recovery.size() == 3 && !recovery[0].deref && recovery[1].deref &&
                recovery[1].offset == 0 && !recovery[2].deref && recovery[2].offset == 1200,
                "recovery chain preserves calibrated stats offset");
        const auto adjacent = otc::gamePlayerRecoveryChain(original, 100, sizeof(void*));
        require(adjacent.size() == 3 && adjacent[1].deref && adjacent[1].offset == sizeof(void*),
                "adjacent Game member is represented in profile");
        require(otc::gamePlayerRecoveryChain(original, 100, 1).empty() &&
                otc::gamePlayerRecoveryChain(original, 100, 68).empty(), "bounded aligned Game members");
        auto incomplete = original;
        incomplete.hasPosition = false;
        require(otc::gamePlayerRecoveryChain(incomplete, 100).empty(), "reject missing position");
        incomplete = original;
        incomplete.chain.back().deref = true;
        require(otc::gamePlayerRecoveryChain(incomplete, 100).empty(), "reject unknown object offset");
        otc::PlayerRecoveryConfirmation confirmation;
        require(!confirmation.observe(1000, 100), "first observation is not enough");
        require(!confirmation.observe(2000, 100), "changed object resets confirmation");
        require(confirmation.observe(2000, 100), "two observations confirm object");
        require(!confirmation.observe(0, 0) && !confirmation.observe(2000, 100), "miss resets confirmation");
        otc::Profile loaded;
        require(otc::profileToJsonFile(original, (game / L"otc-api/pe-test-build.json").wstring()),
                "legacy fixture");
        require(!otc::loadCalibration(loaded, dll.wstring(), original.buildId), "ignore game override");
        otc::Profile embedded;
        require(otc::loadEmbeddedCalibration(
                    embedded, "pe-55ddf475-6a82c258-00fdb000-009d6200-8664"),
                "compiled calibration catalog contains TibiaScape");
        require(embedded.inventory.present && embedded.inventory.arrayOffset == -384,
                "embedded profile preserves latest inventory layout");
        otc::Profile recalibrated;
        recalibrated.buildId = original.buildId;
        recalibrated.chain = {{4321, true}};
        otc::preserveCalibratedSurfaces(recalibrated, original);
        require(recalibrated.chain.size() == 1 && recalibrated.chain[0].offset == 4321,
                "P1 recalibration keeps the new chain");
        require(recalibrated.hasPosition && recalibrated.inventory.present &&
                    recalibrated.actions.present,
                "P1 recalibration preserves existing refined surfaces");
        otc::Profile differentBuild = recalibrated;
        differentBuild.buildId = "pe-different-build";
        differentBuild.inventory = {};
        otc::preserveCalibratedSurfaces(differentBuild, original);
        require(!differentBuild.inventory.present,
                "surface preservation never crosses buildIds");
        otc::CalibrationSource embeddedSource = otc::CalibrationSource::None;
        require(otc::loadCalibration(
                    loaded, dll.wstring(), "pe-55ddf475-6a82c258-00fdb000-009d6200-8664",
                    &embeddedSource),
                "load embedded profile without external calibration directory");
        require(embeddedSource == otc::CalibrationSource::Embedded,
                "reports embedded calibration source");
        fs::create_directories(dll / L"calibrations");
        std::ofstream(otc::calibrationPath(
            dll.wstring(), "pe-55ddf475-6a82c258-00fdb000-009d6200-8664")) << "invalid json";
        embeddedSource = otc::CalibrationSource::None;
        require(otc::loadCalibration(
                    loaded, dll.wstring(), "pe-55ddf475-6a82c258-00fdb000-009d6200-8664",
                    &embeddedSource) && embeddedSource == otc::CalibrationSource::Embedded,
                "invalid external profile falls back to embedded calibration");
        require(!otc::saveCalibration(original, L""), "reject missing DLL directory");
        require(!otc::saveCalibration(original, L"relative"), "reject relative DLL directory");
        require(otc::calibrationPath(dll.wstring(), "../escape").empty(), "reject traversal");
        require(!fs::exists(game / L"calibrations"), "no CWD fallback");
        require(otc::saveCalibration(original, dll.wstring()), "save beside DLL");
        require(otc::loadCalibration(loaded, dll.wstring(), original.buildId), "load beside DLL");
        require(otc::profileToJson(loaded) == otc::profileToJson(original), "round trip all fields");
        otc::Profile legacyActions;
        require(otc::profileFromJson(
                    legacyActions,
                    R"({"actions":{"gameRva":100,"attackRva":200}})"),
                "legacy action profile parses");
        require(legacyActions.actions.attackingCreatureOffset ==
                    static_cast<int>(sizeof(void*) * 2),
                "legacy attack profile gains documented target offset");
        original.capacityScale = 100;
        require(otc::saveCalibration(original, dll.wstring()), "update profile");
        require(otc::loadCalibration(loaded, dll.wstring(), original.buildId) &&
                loaded.capacityScale == 100, "load updated profile");

        // A package relocated to another machine/path has no dependency on the original root.
        fs::rename(dll, root / L"relocated");
        require(otc::loadCalibration(loaded, (root / L"relocated").wstring(), original.buildId),
                "relocated package loads");
        const auto path = otc::calibrationPath((root / L"relocated").wstring(), original.buildId);
        loaded.buildId = "pe-other-build";
        require(otc::profileToJsonFile(loaded, path.wstring()), "mismatch fixture");
        require(!otc::loadCalibration(loaded, (root / L"relocated").wstring(), original.buildId),
                "reject mismatched build");
        loaded.buildId.clear();
        require(otc::profileToJsonFile(loaded, path.wstring()), "legacy empty id fixture");
        require(otc::loadCalibration(loaded, (root / L"relocated").wstring(), original.buildId),
                "preserve legacy empty id support");
        fs::create_directory(root / L"blocked");
        std::ofstream(root / L"blocked/calibrations") << "not a directory";
        require(!otc::saveCalibration(original, (root / L"blocked").wstring()), "save error propagated");

        unsigned fixtures = 0;
        if (argc == 2) {
            for (const auto& entry : fs::recursive_directory_iterator(argv[1])) {
                if (!entry.is_regular_file() || entry.path().extension() != L".bak")
                    continue;
                otc::Profile profile, roundtrip;
                require(otc::profileFromJsonFile(profile, entry.path().wstring()), "existing profile parses");
                require(otc::saveCalibration(profile, (root / L"relocated").wstring()), "existing profile saves");
                require(otc::loadCalibration(roundtrip, (root / L"relocated").wstring(), profile.buildId),
                        "existing profile loads");
                require(otc::profileToJson(profile) == otc::profileToJson(roundtrip), "existing profile unchanged");
                ++fixtures;
            }
            require(fixtures > 0, "no compatibility fixtures found");
        }
        fs::current_path(previousCwd);
        fs::remove_all(root); // Only this test's newly created, unique directory.
        std::cout << "profile_store passed; compatibility fixtures=" << fixtures << '\n';
        return 0;
    } catch (const std::exception& error) {
        fs::current_path(previousCwd);
        std::cerr << error.what() << " (test files retained at " << root << ")\n";
        return 1;
    }
}

