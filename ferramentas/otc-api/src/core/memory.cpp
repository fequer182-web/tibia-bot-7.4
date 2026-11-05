#include "otc_api/memory.h"

#include "otc_api/logger.h"

#include <cstring>
#include <string>

namespace otc {
namespace {

bool sehCopy(const void* src, void* dst, std::size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool memReadable(const void* p, std::size_t n) {
    if (!p || n == 0)
        return false;

    auto addr = reinterpret_cast<std::uintptr_t>(p);
    std::size_t left = n;
    while (left > 0) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)))
            return false;
        if (mbi.State != MEM_COMMIT)
            return false;
        if (memPageGuarded(mbi.Protect))
            return false;

        const auto regionEnd =
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        const std::size_t chunk = static_cast<std::size_t>(regionEnd - addr);
        if (chunk >= left)
            return true;
        addr += chunk;
        left -= chunk;
    }
    return true;
}

bool memRead(const void* src, void* dst, std::size_t n) {
    if (!memReadable(src, n))
        return false;
    return sehCopy(src, dst, n);
}

bool memCopy(const void* src, void* dst, std::size_t n) {
    if (!src || !dst || n == 0)
        return false;
    return sehCopy(src, dst, n);
}

HMODULE hostModule() {
    return GetModuleHandleW(nullptr);
}

std::uintptr_t moduleBase() {
    return reinterpret_cast<std::uintptr_t>(hostModule());
}

std::size_t moduleImageSize() {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hostModule());
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<std::uint8_t*>(dos) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    return nt->OptionalHeader.SizeOfImage;
}

bool looksLikeHeapPtr(std::uintptr_t p) {
    if (p < 0x10000)
        return false;
    const std::uintptr_t mod = moduleBase();
    const std::size_t img = moduleImageSize();
    if (img && p >= mod && p < mod + img)
        return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi)))
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    if (memPageGuarded(mbi.Protect))
        return false;
    return mbi.Type == MEM_PRIVATE || mbi.Type == MEM_MAPPED;
}

bool looksLikeSharedRep(std::uintptr_t rep) {
    if (!looksLikeHeapPtr(rep))
        return false;
    std::uintptr_t vt = 0;
    std::uintptr_t fn = 0;
    if (!readPtr(rep, vt) || !readPtr(vt, fn) || !fn)
        return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(fn), &mbi, sizeof(mbi)))
        return false;
    if (memPageGuarded(mbi.Protect))
        return false;
    const DWORD prot = mbi.Protect & 0xFF;
    if (prot != PAGE_EXECUTE && prot != PAGE_EXECUTE_READ &&
        prot != PAGE_EXECUTE_READWRITE && prot != PAGE_EXECUTE_WRITECOPY)
        return false;
    std::uint32_t uses = 0;
    std::uint32_t weaks = 0;
    const std::uintptr_t usesOffset = sizeof(void*);
    if (!memReadT(rep + usesOffset, uses) ||
        !memReadT(rep + usesOffset + sizeof(uses), weaks))
        return false;
    return uses >= 1 && uses < 1000000 && weaks >= 1 && weaks < 1000000;
}

bool recoverSharedRep(std::uintptr_t obj, std::uintptr_t& rep) {
    rep = 0;
    if (!obj)
        return false;
    const int backs[] = {16, 12, 8, 20, 24, 28, 32};
    for (int back : backs) {
        if (obj < static_cast<std::uintptr_t>(back))
            continue;
        const std::uintptr_t block = obj - static_cast<std::uintptr_t>(back);
        if (!looksLikeSharedRep(block))
            continue;
        rep = block;
        return true;
    }
    for (int off = static_cast<int>(sizeof(void*)); off <= 2048;
         off += static_cast<int>(sizeof(void*))) {
        std::uintptr_t fieldPtr = 0;
        std::uintptr_t fieldRep = 0;
        if (!readPtr(obj + static_cast<std::uintptr_t>(off), fieldPtr) || fieldPtr != obj)
            continue;
        readPtr(obj + static_cast<std::uintptr_t>(off) + sizeof(void*), fieldRep);
        if (!looksLikeSharedRep(fieldRep))
            continue;
        rep = fieldRep;
        return true;
    }
    return false;
}

namespace {

bool inHostImage(std::uintptr_t p) {
    const std::uintptr_t mod = moduleBase();
    const std::size_t img = moduleImageSize();
    return img && p >= mod && p < mod + img;
}

std::string imageRttiName(std::uintptr_t obj) {
    std::uintptr_t vtbl = 0;
    if (!readPtr(obj, vtbl) || !inHostImage(vtbl))
        return {};
    std::uintptr_t col = 0;
    if (!readPtr(vtbl - sizeof(void*), col) || !inHostImage(col))
        return {};
    std::uintptr_t td = 0;
#if defined(_WIN64)
    // MSVC x64 stores image-relative 32-bit references in the complete object
    // locator. x86 stores an absolute TypeDescriptor pointer at the same slot.
    std::uint32_t tdRva = 0;
    if (!memReadT(col + 12, tdRva))
        return {};
    td = moduleBase() + static_cast<std::uintptr_t>(tdRva);
#else
    if (!readPtr(col + 12, td))
        return {};
#endif
    if (!inHostImage(td))
        return {};
    char name[96]{};
    if (!memRead(reinterpret_cast<const void*>(td + sizeof(void*) * 2), name,
                 sizeof(name) - 1))
        return {};
    name[sizeof(name) - 1] = 0;
    return name;
}

bool isRefCountBlock(std::uintptr_t p) {
    const std::string n = imageRttiName(p);
    return n.find("_Ref_count") != std::string::npos;
}

std::uintptr_t embeddedFromRefCount(std::uintptr_t block) {
    const int offs[] = {16, 12, 20, 24, 8};
    for (int off : offs) {
        const std::uintptr_t t = block + static_cast<std::uintptr_t>(off);
        const std::string n = imageRttiName(t);
        if (n.empty() || n.find("_Ref_count") != std::string::npos)
            continue;
        return t;
    }
    return 0;
}

} // namespace

bool completeSharedPtr(std::uintptr_t& ptr, std::uintptr_t& rep) {
    if (!ptr)
        return false;

    if (isRefCountBlock(ptr) && looksLikeSharedRep(ptr)) {
        const std::uintptr_t obj = embeddedFromRefCount(ptr);
        if (obj) {
            rep = ptr;
            ptr = obj;
            return true;
        }
    }

    if (looksLikeSharedRep(rep) && !isRefCountBlock(ptr))
        return true;

    const std::uintptr_t old = rep;
    std::uintptr_t recovered = 0;
    if (isRefCountBlock(ptr) || !recoverSharedRep(ptr, recovered))
        return false;
    LOG("shared_ptr recovered ptr=%p oldRep=%p newRep=%p", reinterpret_cast<void*>(ptr),
        reinterpret_cast<void*>(old), reinterpret_cast<void*>(recovered));
    rep = recovered;
    return true;
}

} // namespace otc

