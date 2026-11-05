#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../actions/OldersInternalActions.hpp"
#include "../profiles/ProfileConfig.hpp"
#include "../profiles/MiniJson.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace {

HMODULE g_module{};
std::uintptr_t g_base{};
volatile bool g_running = true;

std::string moduleDirectory(HMODULE module) {
    char path[MAX_PATH * 4]{};
    const DWORD n = GetModuleFileNameA(module, path, static_cast<DWORD>(sizeof(path)));
    if (!n || n >= sizeof(path)) return ".";
    std::string s(path, path + n);
    const auto pos = s.find_last_of("\\/");
    return pos == std::string::npos ? "." : s.substr(0, pos);
}

std::string currentExecutableName() {
    char path[MAX_PATH * 4]{};
    const DWORD n = GetModuleFileNameA(nullptr, path, static_cast<DWORD>(sizeof(path)));
    if (!n || n >= sizeof(path)) return {};
    std::string s(path, path + n);
    const auto pos = s.find_last_of("\\/");
    return pos == std::string::npos ? s : s.substr(pos + 1);
}

std::size_t currentImageSize() {
    if (!g_base) return 0;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(
        g_base + static_cast<std::uint32_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return static_cast<std::size_t>(nt->OptionalHeader.SizeOfImage);
}

bool equalsIgnoreCase(const std::string& a, const std::string& b) {
    return _stricmp(a.c_str(), b.c_str()) == 0;
}

const crome::profiles::RuntimeProfile* selectInternalProfile(
    const crome::profiles::ProfilesFile& file) {
    const auto exe = currentExecutableName();
    const auto imageSize = currentImageSize();

    auto matches = [&](const crome::profiles::RuntimeProfile& p) {
        if (!p.enabled) return false;
        bool exeOk = false;
        for (const auto& e : p.executables) {
            if (equalsIgnoreCase(e, exe)) { exeOk = true; break; }
        }
        if (!exeOk) return false;
        if (p.moduleSize && *p.moduleSize != imageSize) return false;
        return true;
    };

    if (!file.activeProfile.empty() && file.activeProfile != "auto") {
        if (const auto* p = crome::profiles::findProfileById(file, file.activeProfile);
            p && matches(*p)) return p;
        return nullptr;
    }
    for (const auto& p : file.profiles) if (matches(p)) return &p;
    return nullptr;
}

std::string readText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::optional<crome::Position> loadWalkTarget(const std::string& path) {
    try {
        const auto root = crome::json::parse(readText(path));
        if (!root.isObject()) return std::nullopt;
        const auto* x = root.get("x");
        const auto* y = root.get("y");
        const auto* z = root.get("z");
        if (!x || !y || !z || !x->isNumber() || !y->isNumber() || !z->isNumber())
            return std::nullopt;
        return crome::Position{
            static_cast<std::int32_t>(x->number()),
            static_cast<std::int32_t>(y->number()),
            static_cast<std::int32_t>(z->number())
        };
    } catch (const std::exception& e) {
        std::cout << "[F9] Erro lendo walk_target.json: " << e.what() << "\n";
        return std::nullopt;
    }
}

struct WalkVerification {
    bool active{};
    bool everMoved{};
    crome::Position start{};
    crome::Position target{};
    crome::Position last{};
    std::chrono::steady_clock::time_point began{};
    std::chrono::steady_clock::time_point lastChange{};
};

bool samePosition(const crome::Position& a, const crome::Position& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

void printPosition(const crome::Position& p) {
    std::cout << p.x << "," << p.y << "," << p.z;
}

void beginVerify(WalkVerification& v, crome::Position start, crome::Position target) {
    const auto now = std::chrono::steady_clock::now();
    v.active = true;
    v.everMoved = false;
    v.start = start;
    v.target = target;
    v.last = start;
    v.began = now;
    v.lastChange = now;
    std::cout << "[MONITOR] Acompanhando rota por ate 15 s...\n";
}

void tickVerify(WalkVerification& v, crome::OldersInternalActions& actions) {
    if (!v.active) return;

    const auto now = std::chrono::steady_clock::now();
    const auto p = actions.currentPosition();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - v.began).count();

    if (!samePosition(p, v.last)) {
        v.everMoved = true;
        const bool floorChanged = p.z != v.last.z;
        v.last = p;
        v.lastChange = now;

        std::cout << "[PATH " << elapsed << " ms] ";
        printPosition(p);
        if (floorChanged) std::cout << "  <== FLOOR CHANGE";
        std::cout << "\n";
    }

    if (samePosition(p, v.target)) {
        std::cout << "[VERIFY] ";
        printPosition(p);
        std::cout << " -> REACHED in " << elapsed << " ms\n";
        v.active = false;
        return;
    }

    const auto quiet = std::chrono::duration_cast<std::chrono::milliseconds>(now - v.lastChange).count();

    // autoWalk is asynchronous.  Consider the route completed/stopped only
    // after we observed movement and the position has then remained stable.
    if (v.everMoved && quiet >= 1500 && elapsed >= 1800) {
        std::cout << "[VERIFY] stopped at ";
        printPosition(p);
        std::cout << " after " << elapsed << " ms";
        if (p.z != v.start.z)
            std::cout << " -> MOVED + FLOOR CHANGED (transition/path result)";
        else
            std::cout << " -> MOVED, TARGET NOT EXACT";
        std::cout << "\n";
        v.active = false;
        return;
    }

    if (!v.everMoved && elapsed >= 3500) {
        std::cout << "[VERIFY] no movement after " << elapsed << " ms -> NO MOVEMENT\n";
        v.active = false;
        return;
    }

    if (elapsed >= 15000) {
        std::cout << "[VERIFY] timeout at ";
        printPosition(p);
        std::cout << " after " << elapsed << " ms -> TIMEOUT\n";
        v.active = false;
    }
}

DWORD WINAPI worker(LPVOID) {
    AllocConsole();
    std::freopen("CONOUT$", "w", stdout);
    std::freopen("CONOUT$", "w", stderr);

    std::cout << "============================================================\n"
              << "       CROME SDK V8 - INTERNAL ACTIONS / WALK MONITOR\n"
              << "============================================================\n";

    g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));

    try {
        const auto baseDir = moduleDirectory(g_module);
        const auto jsonPath = baseDir + "\\profiles\\builds.json";
        const auto targetPath = baseDir + "\\config\\walk_target.json";
        auto config = crome::profiles::loadProfiles(jsonPath);
        const auto* profile = selectInternalProfile(config);
        if (!profile) {
            std::cout << "[-] Nenhum profile corresponde ao executavel/module_size atual.\n"
                      << "    JSON: " << jsonPath << "\n";
            goto done;
        }

        std::cout << "[+] Profile          : " << profile->id << "\n"
                  << "[+] Executable       : " << currentExecutableName() << "\n"
                  << std::hex << std::uppercase
                  << "[+] ModuleBase       : 0x" << g_base << "\n"
                  << "[+] ModuleSize       : 0x" << currentImageSize() << "\n";
        if (profile->runtime.localPlayerPtr)
            std::cout << "[+] LocalPlayer ptr  : RVA 0x" << *profile->runtime.localPlayerPtr << "\n";
        if (profile->actions.walk)
            std::cout << "[+] walkTo wrapper   : RVA 0x" << *profile->actions.walk << "\n";
        std::cout << std::dec;

        crome::OldersInternalActions actions(g_base, *profile);

        std::cout << "\n[WAIT] Aguardando personagem/world ready...\n";
        while (g_running && !actions.isWorldReady()) {
            if (GetAsyncKeyState(VK_END) & 1) { g_running = false; break; }
            Sleep(250);
        }
        if (!g_running) goto done;

        if (!actions.validateRuntime(true)) {
            std::cout << "\n[-] Runtime nao passou na validacao. Nenhuma acao sera habilitada.\n";
            goto done;
        }
        if (!actions.installDispatcher()) goto done;

        std::cout << "\n============================================================\n"
                  << "                         READY\n"
                  << "============================================================\n"
                  << "F6  = validar runtime\n"
                  << "F7  = mostrar LocalPlayer/Position\n"
                  << "F8  = walkTo(current + 1 EAST) + monitorar rota\n"
                  << "F9  = walkTo(x,y,z) de config\\walk_target.json\n"
                  << "END = descarregar DLL\n"
                  << "============================================================\n"
                  << "Target JSON: " << targetPath << "\n";

        bool oldF6=false, oldF7=false, oldF8=false, oldF9=false, oldEnd=false;
        WalkVerification verify{};

        while (g_running) {
            const bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
            const bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
            const bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
            const bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
            const bool end = (GetAsyncKeyState(VK_END) & 0x8000) != 0;

            if (f6 && !oldF6) actions.validateRuntime(true);

            if (f7 && !oldF7) {
                const auto player = actions.localPlayer();
                const auto p = actions.currentPosition();
                std::cout << "\n[F7] LocalPlayer=0x" << std::hex << std::uppercase << player << std::dec
                          << " Position=" << p.x << "," << p.y << "," << p.z << "\n";
            }

            if (f8 && !oldF8) {
                const auto start = actions.currentPosition();
                const crome::Position target{start.x + 1, start.y, start.z};
                std::cout << "\n[F8] walkTo -> " << target.x << "," << target.y << "," << target.z << "\n";
                if (actions.walkTo(target)) beginVerify(verify, start, target);
            }

            if (f9 && !oldF9) {
                const auto target = loadWalkTarget(targetPath);
                if (!target) {
                    std::cout << "\n[F9] config\\walk_target.json invalido.\n";
                } else {
                    const auto start = actions.currentPosition();
                    std::cout << "\n[F9] walkTo JSON -> " << target->x << "," << target->y << "," << target->z << "\n";
                    if (actions.walkTo(*target)) beginVerify(verify, start, *target);
                }
            }

            tickVerify(verify, actions);

            if (end && !oldEnd) g_running = false;
            oldF6=f6; oldF7=f7; oldF8=f8; oldF9=f9; oldEnd=end;
            Sleep(10);
        }

        actions.uninstallDispatcher();
    } catch (const std::exception& e) {
        std::cout << "[-] Exception: " << e.what() << "\n";
    }

done:
    std::cout << "\n[UNLOAD] Encerrando Crome SDK V8...\n";
    Sleep(150);
    FreeConsole();
    FreeLibraryAndExitThread(g_module, 0);
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = hModule;
        DisableThreadLibraryCalls(hModule);
        const HANDLE h = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
