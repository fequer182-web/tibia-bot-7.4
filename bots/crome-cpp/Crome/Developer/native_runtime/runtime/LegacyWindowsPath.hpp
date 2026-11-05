#pragma once
#include <windows.h>
#include <filesystem>
#include <string>
#include <stdexcept>
namespace crome {
// Legacy config loaders use GetModuleFileNameA/CRT filenames (Windows ACP).
// std::filesystem expects a native wide path instead of those narrow bytes.
inline std::filesystem::path legacyWindowsPath(const std::string& path) {
    if(path.empty())return {};
    const int size=MultiByteToWideChar(CP_ACP,0,path.data(),static_cast<int>(path.size()),nullptr,0);
    if(!size)throw std::runtime_error("Cannot decode Windows config path");
    std::wstring wide(size,L'\0');
    if(!MultiByteToWideChar(CP_ACP,0,path.data(),static_cast<int>(path.size()),wide.data(),size))throw std::runtime_error("Cannot decode Windows config path");
    return std::filesystem::path(wide);
}
}
