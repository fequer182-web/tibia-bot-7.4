#pragma once

#include <cstdint>
#include <string>

namespace otc {

struct BuildId {
    std::string id;
    std::uint32_t textCrc = 0;
    std::uint32_t timeDateStamp = 0;
    std::uint32_t sizeOfImage = 0;
    std::uint32_t sizeOfCode = 0;
    std::uint16_t machine = 0;
};

// Fingerprint of the host EXE **on disk** (never the mapped .text).
bool computeHostBuildId(BuildId& out);

std::wstring hostExePath();
std::wstring thisDllDir();

} // namespace otc

