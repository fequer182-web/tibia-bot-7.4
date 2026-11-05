#include <Windows.h>

#include "otc_api/commands.h"
#include "otc_api/corpse.h"
#include "otc_api/container_reader.h"
#include "otc_api/creature_reader.h"
#include "otc_api/logger.h"
#include "otc_api/map_reader.h"
#include "otc_api/pipe_server.h"
#include "otc_api/player_reader.h"

namespace {

DWORD WINAPI apiThread(LPVOID param) {
    const HMODULE self = static_cast<HMODULE>(param);

    otc::Logger::instance().init();
    LOG("=== OTCApi loaded pid=%lu module=%p ===", GetCurrentProcessId(), self);

    auto& pipe = otc::PipeServer::instance();
    pipe.setCommandHandler(otc::dispatchCommand);
    otc::startPlayerReader();
    otc::startContainerReader();
    otc::startCreatureReader();
    otc::startMapReader();
    otc::startCorpseReader();
    pipe.start();
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        {
            const HANDLE t = CreateThread(nullptr, 0, apiThread, module, 0, nullptr);
            if (t)
                CloseHandle(t);
            else
                return FALSE;
        }
        break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

