#include "otc_api/build_id.h"

#include "otc_api/logger.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace otc {
namespace {

std::uint32_t crc32(const std::uint8_t* data, std::size_t len) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

std::wstring moduleFilePath(HMODULE mod) {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(mod, buf, MAX_PATH);
    return n ? std::wstring(buf, n) : std::wstring{};
}

bool readFile(const std::wstring& path, std::vector<std::uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.seekg(0, std::ios::end);
    const auto sz = static_cast<std::size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    out.resize(sz);
    return sz == 0 || static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()),
                                               static_cast<std::streamsize>(sz)));
}

} // namespace

std::wstring hostExePath() {
    return moduleFilePath(GetModuleHandleW(nullptr));
}

std::wstring thisDllDir() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&thisDllDir), &self))
        return {};
    const std::wstring path = moduleFilePath(self);
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(0, slash);
}

bool computeHostBuildId(BuildId& out) {
    out = {};
    const std::wstring path = hostExePath();
    std::vector<std::uint8_t> bytes;
    if (path.empty() || !readFile(path, bytes) || bytes.size() < sizeof(IMAGE_DOS_HEADER)) {
        LOG("buildId: failed to read host EXE on disk");
        return false;
    }

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(bytes.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    if (static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS) > bytes.size())
        return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(bytes.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    out.timeDateStamp = nt->FileHeader.TimeDateStamp;
    out.sizeOfImage = nt->OptionalHeader.SizeOfImage;
    out.sizeOfCode = nt->OptionalHeader.SizeOfCode;
    out.machine = nt->FileHeader.Machine;

    const auto* sec = IMAGE_FIRST_SECTION(nt);
    const int nsec = nt->FileHeader.NumberOfSections;
    for (int i = 0; i < nsec; ++i) {
        char name[9]{};
        std::memcpy(name, sec[i].Name, 8);
        if (std::strcmp(name, ".text") != 0)
            continue;
        const std::size_t off = sec[i].PointerToRawData;
        const std::size_t sz = sec[i].SizeOfRawData;
        if (off + sz > bytes.size())
            return false;
        out.textCrc = crc32(bytes.data() + off, sz);
        break;
    }

    char id[80];
    std::snprintf(id, sizeof(id), "pe-%08x-%08x-%08x-%08x-%04x",
                  out.textCrc, out.timeDateStamp, out.sizeOfImage, out.sizeOfCode,
                  out.machine);
    out.id = id;
    LOG("buildId %s (disk EXE)", out.id.c_str());
    return true;
}

} // namespace otc

