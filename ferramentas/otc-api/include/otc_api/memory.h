#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>

namespace otc {

bool memReadable(const void* p, std::size_t n);
bool memRead(const void* src, void* dst, std::size_t n);
bool memCopy(const void* src, void* dst, std::size_t n);

template <typename T>
bool memReadT(std::uintptr_t addr, T& out) {
    return memRead(reinterpret_cast<const void*>(addr), &out, sizeof(T));
}

inline bool readPtr(std::uintptr_t addr, std::uintptr_t& out) {
    return memReadT(addr, out);
}

inline bool memPageGuarded(DWORD protect) {
    return (protect & PAGE_GUARD) != 0 || (protect & 0xFF) == PAGE_NOACCESS;
}

HMODULE hostModule();
std::uintptr_t moduleBase();
std::size_t moduleImageSize();

bool looksLikeHeapPtr(std::uintptr_t p);
bool looksLikeSharedRep(std::uintptr_t rep);
bool recoverSharedRep(std::uintptr_t obj, std::uintptr_t& rep);
bool completeSharedPtr(std::uintptr_t& ptr, std::uintptr_t& rep);

} // namespace otc

