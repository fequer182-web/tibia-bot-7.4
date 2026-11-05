#include "core/runtime/WindowsProcessMemory.hpp"
namespace crome {
bool WindowsProcessMemory::read(uintptr_t address, void* out, size_t size) const {
#ifdef _WIN32
    if (!process_ || !address || !out || !size) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(process_, reinterpret_cast<LPCVOID>(address), out, size, &got) && got == size;
#else
    (void)address; (void)out; (void)size; return false;
#endif
}
bool WindowsProcessMemory::write(uintptr_t address, const void* data, size_t size) {
#ifdef _WIN32
    if (!process_ || !address || !data || !size) return false;
    SIZE_T put = 0;
    return WriteProcessMemory(process_, reinterpret_cast<LPVOID>(address), data, size, &put) && put == size;
#else
    (void)address; (void)data; (void)size; return false;
#endif
}
}
