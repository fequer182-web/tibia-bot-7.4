#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include "MiniJson.hpp"

#include <cstdint>
#include <cctype>
#include <cwctype>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <optional>
#include <chrono>
#include <algorithm>

namespace {

DWORD findRamoniaPid() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring exe = pe.szExeFile;
            for (auto& c : exe) c = static_cast<wchar_t>(towlower(c));
            if (exe.rfind(L"ramonia_gl-", 0) == 0 && exe.size() >= 4 &&
                exe.substr(exe.size() - 4) == L".exe") {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

bool requestRaw(DWORD pid, const std::string& json, std::string& response, bool printResponse = true) {
    const std::string pipeName = "\\\\.\\pipe\\Crome.Ramonia." + std::to_string(pid);

    HANDLE pipe = INVALID_HANDLE_VALUE;
    DWORD lastError = ERROR_SUCCESS;

    // The bridge serves one request per pipe instance. Between Disconnect/Close
    // and the next CreateNamedPipe there is a tiny window where Windows reports
    // FILE_NOT_FOUND instead of waiting. Retry that transport-only race locally.
    for (int attempt = 0; attempt < 20; ++attempt) {
        pipe = CreateFileA(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;

        lastError = GetLastError();
        if (lastError != ERROR_PIPE_BUSY && lastError != ERROR_FILE_NOT_FOUND) break;

        if (lastError == ERROR_PIPE_BUSY)
            WaitNamedPipeA(pipeName.c_str(), 100);

        Sleep(25);
    }

    if (pipe == INVALID_HANDLE_VALUE) {
        if (printResponse)
            std::cout << "[ERRO] Pipe indisponivel: " << pipeName
                      << " Win32=" << lastError << "\n";
        return false;
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    DWORD written{};
    if (!WriteFile(pipe, json.data(), static_cast<DWORD>(json.size()), &written, nullptr)) {
        if (printResponse)
            std::cout << "[ERRO] WriteFile Win32=" << GetLastError() << "\n";
        CloseHandle(pipe);
        return false;
    }

    char buffer[8192]{};
    DWORD read{};
    const BOOL ok = ReadFile(pipe, buffer, sizeof(buffer) - 1, &read, nullptr);
    if (!ok && GetLastError() != ERROR_MORE_DATA) {
        if (printResponse)
            std::cout << "[ERRO] ReadFile Win32=" << GetLastError() << "\n";
        CloseHandle(pipe);
        return false;
    }

    response.assign(buffer, buffer + read);
    CloseHandle(pipe);

    if (printResponse) std::cout << response << "\n";

    if (response.find("\"ok\":false") != std::string::npos) {
        if (printResponse)
            std::cout << "[ERRO] A bridge respondeu, mas o comando falhou.\n";
        return false;
    }
    return true;
}

bool request(DWORD pid, const std::string& json) {
    std::string response;
    return requestRaw(pid, json, response, true);
}

struct MonsterView {
    std::uint32_t id{};
    std::string name;
    std::uint32_t hpPercent{};
    std::uint32_t distance{};
};

struct ContainerItemView {
    std::uint32_t slot{};
    std::uint32_t id{};
    std::uint32_t count{};
};

struct ContainerView {
    std::uint32_t id{};
    std::string name;
    std::uint32_t capacity{};
    std::vector<ContainerItemView> items;
};

std::vector<ContainerView> parseOpenContainers(const std::string& json) {
    std::vector<ContainerView> out;
    try {
        const auto root = crome::json::parse(json);
        const auto* containers = root.get("containers");
        if (!containers || !containers->isArray()) return out;

        for (const auto& v : containers->array()) {
            if (!v.isObject()) continue;
            const auto* id = v.get("id");
            const auto* name = v.get("name");
            const auto* capacity = v.get("capacity");
            const auto* items = v.get("items");
            if (!id || !id->isNumber() || !items || !items->isArray())
                continue;

            ContainerView c;
            c.id = static_cast<std::uint32_t>(id->number());
            if (name && name->isString()) c.name = name->string();
            if (capacity && capacity->isNumber())
                c.capacity = static_cast<std::uint32_t>(capacity->number());

            for (const auto& iv : items->array()) {
                if (!iv.isObject()) continue;
                const auto* slot = iv.get("slot");
                const auto* itemId = iv.get("id");
                const auto* count = iv.get("count");
                if (!slot || !slot->isNumber() || !itemId || !itemId->isNumber())
                    continue;

                ContainerItemView item;
                item.slot = static_cast<std::uint32_t>(slot->number());
                item.id = static_cast<std::uint32_t>(itemId->number());
                item.count = (count && count->isNumber())
                    ? static_cast<std::uint32_t>(count->number()) : 1u;
                c.items.push_back(item);
            }
            out.push_back(std::move(c));
        }
    } catch (...) {}
    return out;
}

bool containerHasItem(const ContainerView& c, std::uint32_t itemId) {
    for (const auto& item : c.items)
        if (item.id == itemId) return true;
    return false;
}

bool hasContainerId(const std::vector<ContainerView>& containers,
                    std::uint32_t id) {
    for (const auto& c : containers)
        if (c.id == id) return true;
    return false;
}

bool lootDiffHasCorpseCandidate(const std::string& json,
                                std::uint32_t corpseItemId,
                                std::string& removedMonsterName) {
    removedMonsterName.clear();
    try {
        const auto root = crome::json::parse(json);
        const auto* removed = root.get("removedMonsters");
        const auto* added = root.get("addedItems");
        if (!removed || !removed->isArray() || removed->array().empty() ||
            !added || !added->isArray())
            return false;

        for (const auto& rv : removed->array()) {
            if (!rv.isObject()) continue;
            const auto* name = rv.get("name");
            if (name && name->isString()) {
                removedMonsterName = name->string();
                break;
            }
        }

        for (const auto& av : added->array()) {
            if (!av.isObject()) continue;
            const auto* id = av.get("id");
            if (id && id->isNumber() &&
                static_cast<std::uint32_t>(id->number()) == corpseItemId)
                return true;
        }
    } catch (...) {}
    return false;
}

std::string lowerCopy(std::string value) {
    for (auto& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

std::vector<MonsterView> parseVisibleMonsters(const std::string& json) {
    std::vector<MonsterView> out;
    try {
        const auto root = crome::json::parse(json);
        const auto* monsters = root.get("monsters");
        if (!monsters || !monsters->isArray()) return out;

        for (const auto& v : monsters->array()) {
            if (!v.isObject()) continue;
            const auto* id = v.get("id");
            const auto* name = v.get("name");
            const auto* hp = v.get("hpPercent");
            const auto* distance = v.get("distance");
            if (!id || !id->isNumber() || !name || !name->isString() ||
                !hp || !hp->isNumber() || !distance || !distance->isNumber())
                continue;

            MonsterView m;
            m.id = static_cast<std::uint32_t>(id->number());
            m.name = name->string();
            m.hpPercent = static_cast<std::uint32_t>(hp->number());
            m.distance = static_cast<std::uint32_t>(distance->number());
            if (m.id && m.hpPercent <= 100) out.push_back(std::move(m));
        }
    } catch (...) {}
    return out;
}

std::uint32_t parseAttackTargetId(const std::string& json) {
    try {
        const auto root = crome::json::parse(json);
        const auto* attack = root.get("attack");
        if (!attack || !attack->isObject()) return 0;
        const auto* id = attack->get("id");
        return id && id->isNumber() ? static_cast<std::uint32_t>(id->number()) : 0;
    } catch (...) {
        return 0;
    }
}

std::optional<MonsterView> chooseTarget(
    const std::vector<MonsterView>& monsters,
    const std::string& wantedName,
    std::uint32_t currentAttackId) {

    const auto wanted = lowerCopy(wantedName);
    const bool wildcard = wanted == "*";

    // Once Game::AttackTarget is nonzero, do not switch because of a
    // transient Tile::things miss or low HP. The game itself decides when the
    // active target is truly gone by clearing AttackTarget.
    if (currentAttackId) {
        for (const auto& m : monsters) {
            if (m.id == currentAttackId) return m;
        }
        return std::nullopt;
    }

    std::optional<MonsterView> best;
    for (const auto& m : monsters) {
        if (m.hpPercent == 0) continue;
        if (!wildcard && lowerCopy(m.name) != wanted) continue;
        if (!best || m.distance < best->distance) best = m;
    }
    return best;
}

int runTargetLoop(DWORD pid, const std::string& wantedName) {
    std::cout << "[TARGET] Ativo para: " << wantedName << "\n"
              << "[TARGET] CTRL+C para encerrar.\n\n";

    std::uint32_t lastTargetId = 0;
    std::uint32_t lastHp = 101;
    bool waitingShown = false;

    for (;;) {
        std::string monstersJson;
        if (!requestRaw(pid, R"({"cmd":"GET_VISIBLE_MONSTERS"})", monstersJson, false)) {
            std::cout << "[TARGET] Falha ao ler monsters; tentando novamente...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }

        std::string combatJson;
        if (!requestRaw(pid, R"({"cmd":"GET_COMBAT_TARGETS"})", combatJson, false)) {
            std::cout << "[TARGET] Falha ao ler combat target; tentando novamente...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }

        const auto monsters = parseVisibleMonsters(monstersJson);
        const auto currentAttackId = parseAttackTargetId(combatJson);
        const auto chosen = chooseTarget(monsters, wantedName, currentAttackId);

        if (!chosen) {
            if (!waitingShown) {
                std::cout << "[TARGET] Nenhum alvo correspondente visivel. Aguardando...\n";
                waitingShown = true;
            }
            lastTargetId = 0;
            lastHp = 101;
            std::this_thread::sleep_for(std::chrono::milliseconds(750));
            continue;
        }

        waitingShown = false;

        if (chosen->id != currentAttackId) {
            const std::string cmd =
                "{\"cmd\":\"ATTACK_ID\",\"creatureId\":" +
                std::to_string(chosen->id) + "}";
            std::string attackResponse;
            std::cout << "[TARGET] Atacando " << chosen->name
                      << " id=" << chosen->id
                      << " hp=" << chosen->hpPercent << "%"
                      << " dist=" << chosen->distance << "\n";
            if (!requestRaw(pid, cmd, attackResponse, false)) {
                if (attackResponse.find("\"creature_id_not_visible\"") != std::string::npos) {
                    std::cout << "[TARGET] Alvo sumiu/morreu antes do ataque; procurando o proximo.\n";
                } else {
                    std::cout << "[TARGET] ATTACK_ID falhou; nova tentativa no proximo ciclo.\n";
                }
            }
        } else if (chosen->id != lastTargetId || chosen->hpPercent != lastHp) {
            std::cout << "[TARGET] " << chosen->name
                      << " id=" << chosen->id
                      << " hp=" << chosen->hpPercent << "%"
                      << " dist=" << chosen->distance << "\n";
        }

        lastTargetId = chosen->id;
        lastHp = chosen->hpPercent;
        std::this_thread::sleep_for(std::chrono::milliseconds(750));
    }
}

int runTargetScanLoop(DWORD pid, const std::string& wantedName) {
    std::cout << "[TARGET SCAN - READ ONLY]\n"
              << "Filtro: " << wantedName << "\n"
              << "NAO chama ATTACK_ID. NAO instala hook.\n"
              << "CTRL+C para encerrar.\n\n";

    std::uint64_t cycles = 0;
    std::uint32_t lastVisible = 0;
    std::uint32_t lastAttackId = 0;

    for (;;) {
        std::string monstersJson;
        std::string combatJson;

        const bool monstersOk =
            requestRaw(pid, R"({"cmd":"GET_VISIBLE_MONSTERS"})", monstersJson, false);
        const bool combatOk =
            requestRaw(pid, R"({"cmd":"GET_COMBAT_TARGETS"})", combatJson, false);

        if (!monstersOk || !combatOk) {
            std::cout << "[SCAN] Falha de leitura; tentando novamente...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        const auto monsters = parseVisibleMonsters(monstersJson);
        const auto attackId = parseAttackTargetId(combatJson);

        std::uint32_t matching = 0;
        const auto wanted = lowerCopy(wantedName);
        const bool wildcard = wanted == "*";
        for (const auto& m : monsters) {
            if (wildcard || lowerCopy(m.name) == wanted)
                ++matching;
        }

        ++cycles;
        if (matching != lastVisible || attackId != lastAttackId ||
            cycles % 10 == 0) {
            std::cout << "[SCAN] cycle=" << cycles
                      << " matching=" << matching
                      << " attackTargetId=" << attackId
                      << " visibleTotal=" << monsters.size()
                      << "\n";
        }

        lastVisible = matching;
        lastAttackId = attackId;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}

int runAutoLootCycle(DWORD pid,
                     std::uint32_t corpseItemId,
                     std::uint32_t targetContainerId,
                     std::uint32_t lootItemId,
                     std::uint32_t waitMs) {
    std::cout << "[AUTO LOOT - ONE CYCLE]\n"
              << "Corpse itemId : " << corpseItemId << "\n"
              << "Loot itemId   : " << lootItemId << "\n"
              << "Target bag ID : " << targetContainerId << "\n"
              << "Wait timeout  : " << waitMs << " ms\n\n";

    std::string beforeJson;
    if (!requestRaw(pid, R"({"cmd":"GET_OPEN_CONTAINERS"})", beforeJson, false)) {
        std::cout << "[FALHOU] Nao foi possivel ler os containers antes do loot.\n";
        return 2;
    }
    const auto before = parseOpenContainers(beforeJson);
    if (!hasContainerId(before, targetContainerId)) {
        std::cout << "[FALHOU] A bag destino ID " << targetContainerId
                  << " nao esta aberta. Abra a bag e tente novamente.\n";
        return 2;
    }

    std::string markJson;
    if (!requestRaw(pid, R"({"cmd":"LOOT_MARK"})", markJson, false)) {
        std::cout << "[FALHOU] LOOT_MARK nao foi aceito.\n";
        return 2;
    }

    std::cout << "[LOOT] Snapshot criado. Agora mate UMA criatura.\n"
              << "[LOOT] Aguardando corpse " << corpseItemId << "...\n";

    const auto started = std::chrono::steady_clock::now();
    std::string removedMonsterName;
    bool corpseFound = false;

    while (!corpseFound) {
        std::string diffJson;
        if (requestRaw(pid, R"({"cmd":"LOOT_DIFF"})", diffJson, false) &&
            lootDiffHasCorpseCandidate(
                diffJson, corpseItemId, removedMonsterName)) {
            corpseFound = true;
            break;
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        if (elapsed >= static_cast<long long>(waitMs)) {
            std::cout << "[FALHOU] Timeout aguardando corpse " << corpseItemId
                      << ".\n";
            return 2;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "[LOOT] Corpse detectado";
    if (!removedMonsterName.empty())
        std::cout << " apos morte de " << removedMonsterName;
    std::cout << ". Abrindo...\n";

    const std::string useCmd =
        "{\"cmd\":\"USE_LOOT_ITEM\",\"itemId\":" +
        std::to_string(corpseItemId) + "}";

    std::string useJson;
    if (!requestRaw(pid, useCmd, useJson, false)) {
        std::cout << "[FALHOU] USE_LOOT_ITEM falhou: " << useJson << "\n";
        return 2;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    std::string afterOpenJson;
    if (!requestRaw(pid, R"({"cmd":"GET_OPEN_CONTAINERS"})", afterOpenJson, false)) {
        std::cout << "[FALHOU] Corpse foi usado, mas nao consegui reler os containers.\n";
        return 2;
    }
    const auto afterOpen = parseOpenContainers(afterOpenJson);

    const ContainerView* source = nullptr;

    // Prefer a container that did not exist before the corpse was opened.
    for (const auto& c : afterOpen) {
        if (c.id == targetContainerId || hasContainerId(before, c.id))
            continue;
        if (containerHasItem(c, lootItemId)) {
            source = &c;
            break;
        }
    }

    // Fallback: any non-target open container containing the requested loot.
    if (!source) {
        for (const auto& c : afterOpen) {
            if (c.id == targetContainerId) continue;
            if (containerHasItem(c, lootItemId)) {
                source = &c;
                break;
            }
        }
    }

    if (!source) {
        std::cout << "[LOOT] Corpse abriu, mas item " << lootItemId
                  << " nao foi encontrado nele. Containers atuais:\n";
        for (const auto& c : afterOpen)
            std::cout << "       id=" << c.id << " name=" << c.name
                      << " items=" << c.items.size() << "\n";
        return 3;
    }

    std::cout << "[LOOT] Source container id=" << source->id
              << " name=" << source->name
              << " contem item " << lootItemId << ". Movendo...\n";

    const std::string moveCmd =
        "{\"cmd\":\"MOVE_CONTAINER_ITEM\",\"sourceContainerId\":" +
        std::to_string(source->id) +
        ",\"targetContainerId\":" + std::to_string(targetContainerId) +
        ",\"itemId\":" + std::to_string(lootItemId) + "}";

    std::string moveJson;
    if (!requestRaw(pid, moveCmd, moveJson, false)) {
        std::cout << "[FALHOU] MOVE_CONTAINER_ITEM falhou: " << moveJson << "\n";
        return 2;
    }

    std::cout << "[OK] Loot movido. " << moveJson << "\n";

    std::string finalJson;
    if (requestRaw(pid, R"({"cmd":"GET_OPEN_CONTAINERS"})", finalJson, false)) {
        const auto finalContainers = parseOpenContainers(finalJson);
        for (const auto& c : finalContainers) {
            if (c.id != targetContainerId) continue;
            std::uint32_t total = 0;
            for (const auto& item : c.items)
                if (item.id == lootItemId) total += item.count;
            std::cout << "[LOOT] Bag destino agora possui total="
                      << total << " do item " << lootItemId << ".\n";
            break;
        }
    }

    return 0;
}

int runAutoFoodLoop(DWORD pid,
                    std::uint32_t itemId,
                    std::uint32_t successDelayMs,
                    std::uint32_t retryDelayMs) {
    std::cout << "[AUTO FOOD] Ativo. itemId=" << itemId
              << " successDelayMs=" << successDelayMs
              << " retryDelayMs=" << retryDelayMs << "\n"
              << "[AUTO FOOD] Procura em QUALQUER container aberto.\n"
              << "[AUTO FOOD] CTRL+C para encerrar.\n\n";

    bool waitingShown = false;
    std::uint64_t successCount = 0;

    for (;;) {
        const std::string cmd =
            "{\"cmd\":\"USE_CONTAINER_ITEM\",\"itemId\":" +
            std::to_string(itemId) + "}";

        std::string response;
        const bool ok = requestRaw(pid, cmd, response, false);

        if (ok) {
            ++successCount;
            waitingShown = false;

            std::cout << "[AUTO FOOD] FOOD USADO"
                      << " itemId=" << itemId
                      << " total=" << successCount << "\n";

            try {
                const auto root = crome::json::parse(response);
                const auto* container = root.get("sourceContainerId");
                const auto* slot = root.get("slot");
                if (container && container->isNumber() &&
                    slot && slot->isNumber()) {
                    std::cout << "            container="
                              << static_cast<std::uint32_t>(container->number())
                              << " slot="
                              << static_cast<std::uint32_t>(slot->number())
                              << "\n";
                }
            } catch (...) {}

            std::this_thread::sleep_for(
                std::chrono::milliseconds(successDelayMs));
            continue;
        }

        if (response.find("\"item_not_found_in_open_containers\"") != std::string::npos) {
            if (!waitingShown) {
                std::cout << "[AUTO FOOD] Nenhum item " << itemId
                          << " nos containers abertos. Aguardando...\n";
                waitingShown = true;
            }
        } else if (response.find("\"source_container_not_open\"") != std::string::npos) {
            if (!waitingShown) {
                std::cout << "[AUTO FOOD] Container indisponivel. Aguardando...\n";
                waitingShown = true;
            }
        } else if (response.find("\"dispatcher_timeout\"") != std::string::npos) {
            std::cout << "[AUTO FOOD] Dispatcher timeout; nova tentativa.\n";
        } else if (!response.empty()) {
            std::cout << "[AUTO FOOD] USE_CONTAINER_ITEM falhou: "
                      << response << "\n";
        } else {
            std::cout << "[AUTO FOOD] Pipe indisponivel; tentando novamente...\n";
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(retryDelayMs));
    }
}

} // namespace

int main(int argc, char** argv) {
    DWORD pid = 0;
    std::string mode = "baseline";
    std::uint32_t creatureId = 0;
    std::string wantedName;
    std::string sayMessage="exura";
    std::uint32_t walkX{},walkY{},walkZ{};
    std::uint32_t tileActionX{},tileActionY{},tileActionZ{};
    std::uint32_t tileActionItemId{};
    std::uint32_t tileToolItemId{};
    std::uint32_t nearbyItemId=2389;
    std::uint32_t nearbyRadius=1;
    std::uint32_t pickupItemId=2389;
    std::uint32_t lootItemId = 2813;
    std::uint32_t sourceContainerId = 0;
    std::uint32_t targetContainerId = 0;
    std::uint32_t moveItemId = 2148;
    std::uint32_t useContainerItemId = 2696;
    std::uint32_t useContainerId = 0;
    std::uint32_t autoFoodItemId = 2696;
    std::uint32_t autoFoodSuccessDelayMs = 60000;
    std::uint32_t autoFoodRetryDelayMs = 2000;
    std::uint32_t lootCycleCorpseId = 2813;
    std::uint32_t lootCycleTargetContainerId = 0;
    std::uint32_t lootCycleItemId = 2148;
    std::uint32_t lootCycleWaitMs = 30000;
    bool useContainerProvided = false;
    bool sourceContainerProvided = false;
    bool targetContainerProvided = false;

    if (argc >= 2) {
        const std::string first = argv[1];
        try {
            std::size_t used{};
            const auto parsed = std::stoul(first, &used, 10);
            if (used == first.size()) pid = static_cast<DWORD>(parsed);
            else mode = first;
        } catch (...) {
            mode = first;
        }
    }

    for (auto& c : mode)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (mode == "attackid") {
        if (argc >= 3) {
            try { creatureId = static_cast<std::uint32_t>(std::stoul(argv[2])); }
            catch (...) {}
        }
        if (argc >= 4) {
            try { pid = static_cast<DWORD>(std::stoul(argv[3])); }
            catch (...) {}
        }
    } else if (mode == "target" || mode == "targetscan") {
        if (argc >= 3) wantedName = argv[2];
        if (argc >= 4) {
            try { pid = static_cast<DWORD>(std::stoul(argv[3])); }
            catch (...) {}
        }
    } else if (mode == "say" || mode == "spell") {
        if (argc >= 3) sayMessage = argv[2];
        if (argc >= 4) {
            try { pid = static_cast<DWORD>(std::stoul(argv[3])); }
            catch (...) {}
        }
    } else if (mode == "walkto" || mode == "autowalk") {
        if (argc >= 5) {
            try {
                walkX=static_cast<std::uint32_t>(std::stoul(argv[2]));
                walkY=static_cast<std::uint32_t>(std::stoul(argv[3]));
                walkZ=static_cast<std::uint32_t>(std::stoul(argv[4]));
            } catch (...) {}
        }
        if (argc >= 6) {
            try { pid=static_cast<DWORD>(std::stoul(argv[5])); }
            catch (...) {}
        }
    } else if (mode == "topmoveat" || mode == "topmovetest") {
        if (argc >= 5) {
            try {
                tileActionX=static_cast<std::uint32_t>(std::stoul(argv[2]));
                tileActionY=static_cast<std::uint32_t>(std::stoul(argv[3]));
                tileActionZ=static_cast<std::uint32_t>(std::stoul(argv[4]));
            } catch (...) {}
        }
        if (argc >= 6) {
            try { pid=static_cast<DWORD>(std::stoul(argv[5])); }
            catch (...) {}
        }
    } else if (mode == "pickupitem" || mode == "pickupspear") {
        if (argc >= 3) {
            try { pickupItemId=static_cast<std::uint32_t>(std::stoul(argv[2])); }
            catch (...) {}
        }
        if (argc >= 4) {
            try { pid=static_cast<DWORD>(std::stoul(argv[3])); }
            catch (...) {}
        }
    } else if (mode == "uncoveritem" || mode == "uncoverspear") {
        if (argc >= 3) {
            try { pickupItemId=static_cast<std::uint32_t>(std::stoul(argv[2])); }
            catch (...) {}
        }
        if (argc >= 4) {
            try { pid=static_cast<DWORD>(std::stoul(argv[3])); }
            catch (...) {}
        }
    } else if (mode == "uncovertile") {
        if (argc >= 6) {
            try {
                tileActionX=static_cast<std::uint32_t>(std::stoul(argv[2]));
                tileActionY=static_cast<std::uint32_t>(std::stoul(argv[3]));
                tileActionZ=static_cast<std::uint32_t>(std::stoul(argv[4]));
                tileActionItemId=static_cast<std::uint32_t>(std::stoul(argv[5]));
            } catch (...) {}
        }
        if (argc >= 7) {
            try { pid=static_cast<DWORD>(std::stoul(argv[6])); }
            catch (...) {}
        }
    } else if (mode == "nearbyitem") {
        if (argc >= 3) {
            try { nearbyItemId=static_cast<std::uint32_t>(std::stoul(argv[2])); }
            catch (...) {}
        }
        if (argc >= 4) {
            try { nearbyRadius=static_cast<std::uint32_t>(std::stoul(argv[3])); }
            catch (...) {}
        }
        if (argc >= 5) {
            try { pid=static_cast<DWORD>(std::stoul(argv[4])); }
            catch (...) {}
        }
    } else if (mode == "tileitems") {
        if (argc >= 5) {
            try {
                tileActionX=static_cast<std::uint32_t>(std::stoul(argv[2]));
                tileActionY=static_cast<std::uint32_t>(std::stoul(argv[3]));
                tileActionZ=static_cast<std::uint32_t>(std::stoul(argv[4]));
            } catch (...) {}
        }
        if (argc >= 6) {
            try { pid=static_cast<DWORD>(std::stoul(argv[5])); }
            catch (...) {}
        }
    } else if (mode == "usetile") {
        if (argc >= 6) {
            try {
                tileActionX=static_cast<std::uint32_t>(std::stoul(argv[2]));
                tileActionY=static_cast<std::uint32_t>(std::stoul(argv[3]));
                tileActionZ=static_cast<std::uint32_t>(std::stoul(argv[4]));
                tileActionItemId=static_cast<std::uint32_t>(std::stoul(argv[5]));
            } catch (...) {}
        }
        if (argc >= 7) {
            try { pid=static_cast<DWORD>(std::stoul(argv[6])); }
            catch (...) {}
        }
    } else if (mode == "usewithtile") {
        if (argc >= 6) {
            try {
                tileToolItemId=static_cast<std::uint32_t>(std::stoul(argv[2]));
                tileActionX=static_cast<std::uint32_t>(std::stoul(argv[3]));
                tileActionY=static_cast<std::uint32_t>(std::stoul(argv[4]));
                tileActionZ=static_cast<std::uint32_t>(std::stoul(argv[5]));
            } catch (...) {}
        }
        if (argc >= 7) {
            try { pid=static_cast<DWORD>(std::stoul(argv[6])); }
            catch (...) {}
        }
    } else if (mode == "usecorpse" || mode == "useloot") {
        if (argc >= 3) {
            try { lootItemId = static_cast<std::uint32_t>(std::stoul(argv[2])); }
            catch (...) {}
        }
        if (argc >= 4) {
            try { pid = static_cast<DWORD>(std::stoul(argv[3])); }
            catch (...) {}
        }
    } else if (mode == "autoloot" || mode == "lootcycle") {
        if (argc >= 3) {
            try { lootCycleCorpseId = static_cast<std::uint32_t>(std::stoul(argv[2])); }
            catch (...) {}
        }
        if (argc >= 4) {
            try { lootCycleTargetContainerId = static_cast<std::uint32_t>(std::stoul(argv[3])); }
            catch (...) {}
        }
        if (argc >= 5) {
            try { lootCycleItemId = static_cast<std::uint32_t>(std::stoul(argv[4])); }
            catch (...) {}
        }
        if (argc >= 6) {
            try { lootCycleWaitMs = static_cast<std::uint32_t>(std::stoul(argv[5])); }
            catch (...) {}
        }
        if (argc >= 7) {
            try { pid = static_cast<DWORD>(std::stoul(argv[6])); }
            catch (...) {}
        }
    } else if (mode == "autofood") {
        if (argc >= 3) {
            try { autoFoodItemId = static_cast<std::uint32_t>(std::stoul(argv[2])); }
            catch (...) {}
        }
        if (argc >= 4) {
            try { autoFoodSuccessDelayMs = static_cast<std::uint32_t>(std::stoul(argv[3])); }
            catch (...) {}
        }
        if (argc >= 5) {
            try { autoFoodRetryDelayMs = static_cast<std::uint32_t>(std::stoul(argv[4])); }
            catch (...) {}
        }
        if (argc >= 6) {
            try { pid = static_cast<DWORD>(std::stoul(argv[5])); }
            catch (...) {}
        }
    } else if (mode == "useitem" || mode == "usefood" ||
               mode == "useself" || mode == "usewithself") {
        if (argc >= 3) {
            std::string containerArg = argv[2];
            for (auto& c : containerArg)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (containerArg != "any" && containerArg != "*") {
                try {
                    useContainerId = static_cast<std::uint32_t>(std::stoul(containerArg));
                    useContainerProvided = true;
                } catch (...) {}
            }
        }
        if (argc >= 4) {
            try { useContainerItemId = static_cast<std::uint32_t>(std::stoul(argv[3])); }
            catch (...) {}
        }
        if (argc >= 5) {
            try { pid = static_cast<DWORD>(std::stoul(argv[4])); }
            catch (...) {}
        }
    } else if (mode == "movegold" || mode == "moveitem") {
        if (argc >= 3) {
            try {
                sourceContainerId = static_cast<std::uint32_t>(std::stoul(argv[2]));
                sourceContainerProvided = true;
            } catch (...) {}
        }
        if (argc >= 4) {
            try {
                targetContainerId = static_cast<std::uint32_t>(std::stoul(argv[3]));
                targetContainerProvided = true;
            } catch (...) {}
        }
        if (argc >= 5) {
            try { moveItemId = static_cast<std::uint32_t>(std::stoul(argv[4])); }
            catch (...) {}
        }
        if (argc >= 6) {
            try { pid = static_cast<DWORD>(std::stoul(argv[5])); }
            catch (...) {}
        }
    } else if (argc >= 3 && !pid) {
        try { pid = static_cast<DWORD>(std::stoul(argv[2])); }
        catch (...) {}
    }

    if (!pid) pid = findRamoniaPid();

    if (!pid) {
        std::cout << "[ERRO] Ramonia nao encontrado.\n";
        return 1;
    }

    std::cout << "============================================================\n"
              << "       Crome Pipe Test - external x64 -> Ramonia x86\n"
              << "============================================================\n"
              << "PID  : " << pid << "\n"
              << "PIPE : \\\\.\\pipe\\Crome.Ramonia." << pid << "\n\n";

    bool ok = true;

    if (mode == "lookarm") {
        std::cout << "[REAL LOOK ID - ARM]\n";
        ok &= request(pid, R"({"cmd":"ARM_LOOK_ID"})");
        std::cout << "\n" << (ok
            ? "[OK] Capture armado por ate 8s. Agora de Look normal em um item no Ramonia.\n"
            : "[FALHOU] Nao foi possivel armar o Look ID.\n");
        return ok ? 0 : 2;
    }

    if (mode == "lookget") {
        std::cout << "[REAL LOOK ID - RESULT]\n";
        ok &= request(pid, R"({"cmd":"GET_LOOK_ID"})");
        return ok ? 0 : 2;
    }

    if (mode == "walkto" || mode == "autowalk") {
        if (!walkX || !walkY || walkZ>0xFFu) {
            std::cout << "[ERRO] Uso: CromePipeTest_x64.exe walkto <x> <y> <z> [PID]\n";
            return 1;
        }
        std::cout << "[AUTO_WALK_TO - Game::autoWalk]\n";
        const std::string cmd=
            "{\"cmd\":\"AUTO_WALK_TO\",\"x\":"+std::to_string(walkX)+
            ",\"y\":"+std::to_string(walkY)+
            ",\"z\":"+std::to_string(walkZ)+"}";
        ok &= request(pid,cmd);
        std::cout << "\n" << (ok
            ? "[OK] Rota enviada ao Game::autoWalk. Confira o movimento.\n"
            : "[FALHOU] AUTO_WALK_TO nao foi confirmado.\n");
        return ok ? 0 : 2;
    }

    if (mode == "topmoveabi" || mode == "topmove") {
        std::cout << "[GET_TOP_MOVE_ABI - READ ONLY]\n";
        ok &= request(pid,R"({"cmd":"GET_TOP_MOVE_ABI"})");
        std::cout << "\n" << (ok
            ? "[OK] Diagnostico read-only coletado; nenhum metodo interno foi chamado.\n"
            : "[FALHOU] Nao foi possivel ler o ABI do getTopMoveThing.\n");
        return ok ? 0 : 2;
    }

    if (mode == "topmoveat" || mode == "topmovetest") {
        if (!tileActionX || !tileActionY || tileActionZ>0xFFu) {
            std::cout << "[ERRO] Uso: CromePipeTest_x64.exe topmoveat <x> <y> <z> [PID]\n";
            return 1;
        }

        std::cout << "[GET_TOP_MOVE_AT - RUNTIME DIAGNOSTIC / NO MOVE]\n";
        const std::string cmd=
            "{\"cmd\":\"GET_TOP_MOVE_AT\",\"x\":"+
            std::to_string(tileActionX)+
            ",\"y\":"+std::to_string(tileActionY)+
            ",\"z\":"+std::to_string(tileActionZ)+"}";
        ok &= request(pid,cmd);
        std::cout << "\n" << (ok
            ? "[OK] getTopMoveThing executado no game thread; nenhum item foi movido.\n"
            : "[FALHOU] Nao foi possivel validar o retorno de getTopMoveThing.\n");
        return ok ? 0 : 2;
    }

    if (mode == "hands") {
        std::cout << "[GET_HANDS - INVENTORY SLOTS 5/6]\n";
        ok &= request(pid,R"({"cmd":"GET_HANDS"})");
        return ok ? 0 : 2;
    }

    if (mode == "pickupitem" || mode == "pickupspear") {
        if (!pickupItemId || pickupItemId>0xFFFFu) {
            std::cout << "[ERRO] Uso: CromePipeTest_x64.exe pickupitem <itemId=2389> [PID]\n";
            return 1;
        }

        std::cout << "[MOVE_NEARBY_ITEM_TO_HAND]\n";
        const std::string cmd=
            "{\"cmd\":\"MOVE_NEARBY_ITEM_TO_HAND\",\"itemId\":"+
            std::to_string(pickupItemId)+"}";
        ok &= request(pid,cmd);
        std::cout << "\n" << (ok
            ? "[OK] Movimento para a mao solicitado. Confira o item com hands; item coberto retorna item_covered.\n"
            : "[FALHOU] O pickup foi bloqueado ou a bridge retornou erro.\n");
        return ok ? 0 : 2;
    }

    if (mode == "uncoveritem" || mode == "uncoverspear") {
        if (!pickupItemId || pickupItemId>0xFFFFu) {
            std::cout << "[ERRO] Uso: CromePipeTest_x64.exe uncoveritem <itemId=2389> [PID]\n";
            return 1;
        }

        std::cout << "[UNCOVER_NEARBY_ITEM - MOVE ONE BLOCKER]\n";
        const std::string cmd=
            "{\"cmd\":\"UNCOVER_NEARBY_ITEM\",\"itemId\":"+
            std::to_string(pickupItemId)+"}";
        ok &= request(pid,cmd);
        std::cout << "\n" << (ok
            ? "[OK] Se havia bloqueador, um item foi enviado para um SQM vizinho. Confira no jogo.\n"
            : "[FALHOU] Nao foi possivel remover o bloqueador.\n");
        return ok ? 0 : 2;
    }

    if (mode == "uncovertile") {
        if (!tileActionX || !tileActionY || tileActionZ>0xFFu ||
            !tileActionItemId || tileActionItemId>0xFFFFu) {
            std::cout << "[ERRO] Uso: CromePipeTest_x64.exe uncovertile <x> <y> <z> <targetItemId> [PID]\n";
            return 1;
        }

        std::cout << "[UNCOVER_TILE_TARGET_AT - MOVE ONE BLOCKER]\n";
        const std::string cmd=
            "{\"cmd\":\"UNCOVER_TILE_TARGET_AT\",\"x\":"+
            std::to_string(tileActionX)+
            ",\"y\":"+std::to_string(tileActionY)+
            ",\"z\":"+std::to_string(tileActionZ)+
            ",\"itemId\":"+std::to_string(tileActionItemId)+"}";
        ok &= request(pid,cmd);
        return ok ? 0 : 2;
    }

    if (mode == "nearbyitem") {
        if (!nearbyItemId || nearbyItemId>0xFFFFu || nearbyRadius>7u) {
            std::cout << "[ERRO] Uso: CromePipeTest_x64.exe nearbyitem <itemId> [radius=1] [PID]\n";
            return 1;
        }

        std::cout << "[GET_NEARBY_ITEM - LOCAL TILE WINDOW]\n";
        const std::string cmd=
            "{\"cmd\":\"GET_NEARBY_ITEM\",\"itemId\":"+
            std::to_string(nearbyItemId)+
            ",\"radius\":"+std::to_string(nearbyRadius)+"}";
        ok &= request(pid,cmd);
        return ok ? 0 : 2;
    }

    if (mode == "tileitems") {
        if (!tileActionX || !tileActionY || tileActionZ>0xFFu) {
            std::cout << "[ERRO] Uso: CromePipeTest_x64.exe tileitems <x> <y> <z> [PID]\n";
            return 1;
        }

        std::cout << "[GET_TILE_ITEMS_AT - FRESH]\n";
        const std::string cmd=
            "{\"cmd\":\"GET_TILE_ITEMS_AT\",\"x\":"+
            std::to_string(tileActionX)+
            ",\"y\":"+std::to_string(tileActionY)+
            ",\"z\":"+std::to_string(tileActionZ)+"}";
        ok &= request(pid,cmd);
        return ok ? 0 : 2;
    }

    if (mode == "usetile") {
        if (!tileActionX || !tileActionY || tileActionZ>0xFFu ||
            tileActionItemId>0xFFFFu) {
            std::cout << "[ERRO] Uso: CromePipeTest_x64.exe usetile <x> <y> <z> <itemId|0> [PID]\n";
            return 1;
        }

        std::cout << "[USE_TILE_ITEM_AT]\n";
        const std::string cmd=
            "{\"cmd\":\"USE_TILE_ITEM_AT\",\"x\":"+
            std::to_string(tileActionX)+
            ",\"y\":"+std::to_string(tileActionY)+
            ",\"z\":"+std::to_string(tileActionZ)+
            ",\"itemId\":"+std::to_string(tileActionItemId)+"}";
        ok &= request(pid,cmd);
        return ok ? 0 : 2;
    }

    if (mode == "usewithtile") {
        if (!tileToolItemId || tileToolItemId>0xFFFFu ||
            !tileActionX || !tileActionY || tileActionZ>0xFFu) {
            std::cout << "[ERRO] Uso: CromePipeTest_x64.exe usewithtile <toolItemId> <x> <y> <z> [PID]\n";
            return 1;
        }

        std::cout << "[USE_WITH_TILE_AT]\n";
        const std::string cmd=
            "{\"cmd\":\"USE_WITH_TILE_AT\",\"toolItemId\":"+
            std::to_string(tileToolItemId)+
            ",\"x\":"+std::to_string(tileActionX)+
            ",\"y\":"+std::to_string(tileActionY)+
            ",\"z\":"+std::to_string(tileActionZ)+"}";
        ok &= request(pid,cmd);
        return ok ? 0 : 2;
    }

    if (mode == "say" || mode == "spell") {
        if (sayMessage.empty() || sayMessage.size()>15) {
            std::cout << "[ERRO] Uso: CromePipeTest_x64.exe say \"exura\" [PID]\n";
            return 1;
        }
        for(const unsigned char c:sayMessage) {
            if(c<0x20 || c>0x7E || c=='"' || c=='\\') {
                std::cout << "[ERRO] Mensagem deve ser ASCII simples, sem aspas/backslash.\n";
                return 1;
            }
        }
        std::cout << "[SAY - Game::talkChannel]\n";
        const std::string cmd=
            "{\"cmd\":\"SAY\",\"message\":\""+sayMessage+"\"}";
        ok &= request(pid,cmd);
        std::cout << "\n" << (ok
            ? "[OK] SAY enviado pelo talkChannel interno. Confira a magia no jogo.\n"
            : "[FALHOU] SAY nao foi confirmado.\n");
        return ok ? 0 : 2;
    }

    if (mode == "lightstatus") {
        std::cout << "[GET_LIGHT - PROFILE DRIVEN]\n";
        ok &= request(pid, R"({"cmd":"GET_LIGHT"})");
        return ok ? 0 : 2;
    }

    if (mode == "lighton") {
        std::cout << "[SET_LIGHT ON - ambient 1.0]\n";
        ok &= request(pid, R"({"cmd":"SET_LIGHT","enabled":1})");
        std::cout << "\n" << (ok
            ? "[OK] Light solicitado. Confira a claridade no mapa.\n"
            : "[FALHOU] Light nao foi aplicado.\n");
        return ok ? 0 : 2;
    }

    if (mode == "lightoff") {
        std::cout << "[SET_LIGHT OFF - RESTORE]\n";
        ok &= request(pid, R"({"cmd":"SET_LIGHT","enabled":0})");
        std::cout << "\n" << (ok
            ? "[OK] Light restaurado para o valor original.\n"
            : "[FALHOU] Restore do Light nao foi confirmado.\n");
        return ok ? 0 : 2;
    }

    if (mode == "targetscan") {
        if (wantedName.empty()) {
            std::cout << "[ERRO] Uso: CromePipeTest_x64.exe targetscan <nome|*> [PID]\n";
            return 1;
        }
        return runTargetScanLoop(pid, wantedName);
    }

    if (mode == "target") {
        if (wantedName.empty()) {
            std::cout << "[ERRO] Uso: CromePipeTest_x64.exe target <nome|*> [PID]\n";
            return 1;
        }
        return runTargetLoop(pid, wantedName);
    }

    if (mode == "monsters") {
        std::cout << "[GET_VISIBLE_MONSTERS]\n";
        ok &= request(pid, R"({"cmd":"GET_VISIBLE_MONSTERS"})");
        std::cout << "\n" << (ok ? "[OK] Monsters visiveis lidos sem Follow manual.\n"
                                   : "[FALHOU] Nao foi possivel listar os monsters.\n");
        return ok ? 0 : 2;
    }

    if (mode == "tilemon" || mode == "tilemonsters") {
        std::cout << "[GET_TILE_MONSTERS - READ ONLY]\n";
        ok &= request(pid, R"({"cmd":"GET_TILE_MONSTERS"})");
        std::cout << "\n" << (ok ? "[OK] Tile::things lido sem ataque e sem Follow manual.\n"
                                   : "[FALHOU] Nao foi possivel ler Monsters via Tile::things.\n");
        return ok ? 0 : 2;
    }

    if (mode == "lootmark") {
        std::cout << "[LOOT_MARK - READ ONLY]\n";
        ok &= request(pid, R"({"cmd":"LOOT_MARK"})");
        std::cout << "\n" << (ok ? "[OK] Snapshot de Tile::things salvo. Agora mate UMA criatura sem andar longe.\n"
                                   : "[FALHOU] Nao foi possivel criar o snapshot de loot.\n");
        return ok ? 0 : 2;
    }

    if (mode == "lootdiff") {
        std::cout << "[LOOT_DIFF - READ ONLY]\n";
        ok &= request(pid, R"({"cmd":"LOOT_DIFF"})");
        std::cout << "\n" << (ok ? "[OK] Diferenca de Tile::things calculada.\n"
                                   : "[FALHOU] Nao foi possivel comparar o snapshot de loot.\n");
        return ok ? 0 : 2;
    }

    if (mode == "containers") {
        std::cout << "[GET_OPEN_CONTAINERS - READ ONLY]\n";
        ok &= request(pid, R"({"cmd":"GET_OPEN_CONTAINERS"})");
        std::cout << "\n" << (ok ? "[OK] Containers abertos e itens lidos.\n"
                                   : "[FALHOU] Nao foi possivel ler containers abertos.\n");
        return ok ? 0 : 2;
    }

    if (mode == "usecorpse" || mode == "useloot") {
        if (!lootItemId || lootItemId > 0xFFFFu) {
            std::cout << "[ERRO] Uso: CromePipeTest_x64.exe usecorpse [itemId=2813] [PID]\n";
            return 1;
        }
        std::cout << "[USE_LOOT_ITEM - EXPERIMENTAL]\n";
        const std::string cmd =
            "{\"cmd\":\"USE_LOOT_ITEM\",\"itemId\":" +
            std::to_string(lootItemId) + "}";
        ok &= request(pid, cmd);
        std::cout << "\n" << (ok ? "[OK] Game::use foi chamado no holder do Item. Confira se o corpse abriu.\n"
                                   : "[FALHOU] USE_LOOT_ITEM nao foi confirmado.\n");
        return ok ? 0 : 2;
    }

    if (mode == "autoloot" || mode == "lootcycle") {
        if (!lootCycleCorpseId || lootCycleCorpseId > 0xFFFFu ||
            !lootCycleItemId || lootCycleItemId > 0xFFFFu) {
            std::cout << "[ERRO] Uso: CromePipeTest_x64.exe autoloot [corpseId=2813] [targetBagId=0] [lootItemId=2148] [waitMs=30000] [PID]\n";
            return 1;
        }
        if (lootCycleWaitMs < 1000) lootCycleWaitMs = 1000;
        return runAutoLootCycle(
            pid,
            lootCycleCorpseId,
            lootCycleTargetContainerId,
            lootCycleItemId,
            lootCycleWaitMs);
    }

    if (mode == "autofood") {
        if (!autoFoodItemId || autoFoodItemId > 0xFFFFu) {
            std::cout << "[ERRO] Uso: CromePipeTest_x64.exe autofood [itemId=2696] [successDelayMs=60000] [retryDelayMs=2000] [PID]\n";
            return 1;
        }
        if (autoFoodSuccessDelayMs < 1000) autoFoodSuccessDelayMs = 1000;
        if (autoFoodRetryDelayMs < 250) autoFoodRetryDelayMs = 250;
        return runAutoFoodLoop(
            pid,
            autoFoodItemId,
            autoFoodSuccessDelayMs,
            autoFoodRetryDelayMs);
    }

    if (mode == "useself" || mode == "usewithself") {
        if (!useContainerItemId || useContainerItemId > 0xFFFFu) {
            std::cout << "[ERRO] Uso: CromePipeTest_x64.exe useself <containerId|any> <itemId> [PID]\n";
            return 1;
        }

        std::cout << "[USE_CONTAINER_ITEM_ON_SELF - EXPERIMENTAL]\n";
        std::string cmd =
            "{\"cmd\":\"USE_CONTAINER_ITEM_ON_SELF\",\"itemId\":" +
            std::to_string(useContainerItemId);
        if (useContainerProvided)
            cmd += ",\"sourceContainerId\":" + std::to_string(useContainerId);
        cmd += "}";

        ok &= request(pid,cmd);
        std::cout << "\n" << (ok
            ? "[OK] Game::useWith(item,self) foi chamado. Confira se UH/potion foi consumido no personagem.\n"
            : "[FALHOU] USE_CONTAINER_ITEM_ON_SELF nao foi confirmado.\n");
        return ok ? 0 : 2;
    }

    if (mode == "useitem" || mode == "usefood") {
        if (!useContainerItemId || useContainerItemId > 0xFFFFu) {
            std::cout << "[ERRO] Uso: CromePipeTest_x64.exe usefood <containerId|any> [itemId=2696] [PID]\n";
            return 1;
        }

        std::cout << "[USE_CONTAINER_ITEM - EXPERIMENTAL]\n";
        std::string cmd = "{\"cmd\":\"USE_CONTAINER_ITEM\",\"itemId\":" +
                          std::to_string(useContainerItemId);
        if (useContainerProvided)
            cmd += ",\"sourceContainerId\":" + std::to_string(useContainerId);
        cmd += "}";

        ok &= request(pid, cmd);
        std::cout << "\n" << (ok
            ? "[OK] Game::use foi chamado no item do container. Confira se o food foi consumido.\n"
            : "[FALHOU] USE_CONTAINER_ITEM nao foi confirmado.\n");
        return ok ? 0 : 2;
    }

    if (mode == "movegold" || mode == "moveitem") {
        if (!sourceContainerProvided || !targetContainerProvided ||
            !moveItemId || moveItemId > 0xFFFFu) {
            std::cout << "[ERRO] Uso: CromePipeTest_x64.exe movegold <corpseContainerId> <bagContainerId> [itemId=2148] [PID]\n";
            return 1;
        }
        std::cout << "[MOVE_CONTAINER_ITEM - EXPERIMENTAL]\n";
        const std::string cmd =
            "{\"cmd\":\"MOVE_CONTAINER_ITEM\",\"sourceContainerId\":" +
            std::to_string(sourceContainerId) +
            ",\"targetContainerId\":" + std::to_string(targetContainerId) +
            ",\"itemId\":" + std::to_string(moveItemId) + "}";
        ok &= request(pid, cmd);
        std::cout << "\n" << (ok ? "[OK] Game::move foi chamado. Confira o item na bag.\n"
                                   : "[FALHOU] MOVE_CONTAINER_ITEM nao foi confirmado.\n");
        return ok ? 0 : 2;
    }

    if (mode == "attackid") {
        if (!creatureId) {
            std::cout << "[ERRO] Uso: CromePipeTest_x64.exe attackid <creatureId> [PID]\n";
            return 1;
        }

        std::cout << "[GET_COMBAT_TARGETS - BEFORE]\n";
        ok &= request(pid, R"({"cmd":"GET_COMBAT_TARGETS"})");
        if (!ok) return 2;

        std::cout << "\n[ATTACK_ID]\n";
        const std::string attack =
            "{\"cmd\":\"ATTACK_ID\",\"creatureId\":" + std::to_string(creatureId) + "}";
        ok &= request(pid, attack);

        std::cout << "\n[GET_COMBAT_TARGETS - AFTER]\n";
        ok &= request(pid, R"({"cmd":"GET_COMBAT_TARGETS"})");

        std::cout << "\n" << (ok ? "[OK] ATTACK_ID executado sem Follow manual.\n"
                                   : "[FALHOU] ATTACK_ID nao foi confirmado.\n");
        return ok ? 0 : 2;
    }

    if (mode == "targets") {
        std::cout << "[GET_COMBAT_TARGETS]\n";
        ok &= request(pid, R"({"cmd":"GET_COMBAT_TARGETS"})");
        std::cout << "\n" << (ok ? "[OK] Combat targets lidos.\n"
                                   : "[FALHOU] Nao foi possivel ler combat targets.\n");
        return ok ? 0 : 2;
    }

    if (mode == "attack") {
        std::cout << "[GET_COMBAT_TARGETS - BEFORE]\n";
        ok &= request(pid, R"({"cmd":"GET_COMBAT_TARGETS"})");
        if (!ok) return 2;

        std::cout << "\n[ATTACK_SELECTED]\n";
        ok &= request(pid, R"({"cmd":"ATTACK_SELECTED"})");

        std::cout << "\n[GET_COMBAT_TARGETS - AFTER]\n";
        ok &= request(pid, R"({"cmd":"GET_COMBAT_TARGETS"})");

        std::cout << "\n" << (ok ? "[OK] ATTACK_SELECTED executado no game thread.\n"
                                   : "[FALHOU] ATTACK_SELECTED nao foi confirmado.\n");
        return ok ? 0 : 2;
    }

    std::cout << "[HELLO]\n";
    ok &= request(pid, R"({"cmd":"HELLO"})");

    std::cout << "\n[BUILD_INFO]\n";
    ok &= request(pid, R"({"cmd":"BUILD_INFO"})");

    std::cout << "\n[GET_PLAYER]\n";
    ok &= request(pid, R"({"cmd":"GET_PLAYER"})");

    std::cout << "\n" << (ok ? "[OK] Pipe + profile + snapshot funcionando.\n"
                               : "[FALHOU] Veja a primeira mensagem de erro acima.\n");
    return ok ? 0 : 2;
}
