#include <Windows.h>
#include <TlHelp32.h>
#include <Psapi.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace {

struct ProcessEntry {
    DWORD pid = 0;
    std::wstring name;
    std::wstring path;
};

std::string utf8(const std::wstring& ws) {
    if (ws.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        default:   out += c; break;
        }
    }
    return out;
}

bool injectorIs64() {
#if defined(_WIN64)
    return true;
#else
    return false;
#endif
}

bool processIs64(DWORD pid, bool& out64, DWORD& err) {
    out64 = false;
    err = 0;
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        err = GetLastError();
        return false;
    }
    BOOL wow64 = FALSE;
    if (!IsWow64Process(proc, &wow64)) {
        err = GetLastError();
        CloseHandle(proc);
        return false;
    }
    CloseHandle(proc);

    BOOL selfWow64 = FALSE;
    IsWow64Process(GetCurrentProcess(), &selfWow64);
    const bool host64 = injectorIs64() || selfWow64;
    if (!host64) {
        out64 = false;
        return true;
    }
    out64 = !wow64;
    return true;
}

DWORD findPidByName(const std::wstring& exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName.c_str()) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

std::wstring canonical(const std::wstring& path) {
    try {
        return std::filesystem::weakly_canonical(path).wstring();
    } catch (...) {
        return path;
    }
}

DWORD findPidByPath(const std::wstring& exePath) {
    const std::wstring target = canonical(exePath);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!proc)
                continue;
            wchar_t image[MAX_PATH]{};
            DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(proc, 0, image, &n) && canonical(image) == target)
                pid = pe.th32ProcessID;
            CloseHandle(proc);
            if (pid)
                break;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

void printProcessListJson() {
    std::vector<ProcessEntry> entries;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        std::cout << "{\"ok\":false,\"error\":\"snapshot failed\"}\n";
        return;
    }

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!proc)
                continue;
            wchar_t image[MAX_PATH]{};
            DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(proc, 0, image, &n)) {
                std::wstring path(image);
                if (path.size() >= 4 && _wcsicmp(path.c_str() + path.size() - 4, L".exe") == 0)
                    entries.push_back({pe.th32ProcessID, pe.szExeFile, path});
            }
            CloseHandle(proc);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    std::sort(entries.begin(), entries.end(), [](const ProcessEntry& a, const ProcessEntry& b) {
        const int c = _wcsicmp(a.name.c_str(), b.name.c_str());
        return c != 0 ? c < 0 : a.pid < b.pid;
    });

    std::cout << "{\"ok\":true,\"processes\":[";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i)
            std::cout << ',';
        std::cout << "{\"pid\":" << entries[i].pid
                  << ",\"name\":\"" << jsonEscape(utf8(entries[i].name)) << '"'
                  << ",\"path\":\"" << jsonEscape(utf8(entries[i].path)) << "\"}";
    }
    std::cout << "]}\n";
}

bool enableDebugPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;

    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
        CloseHandle(token);
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const BOOL ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    const DWORD err = GetLastError();
    CloseHandle(token);
    return ok && err == ERROR_SUCCESS;
}

bool dllAlreadyLoaded(DWORD pid, const std::wstring& dllPath) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc)
        return false;

    const std::wstring dllName = std::filesystem::path(dllPath).filename().wstring();
    HMODULE mods[1024];
    DWORD needed = 0;
    bool found = false;
    if (EnumProcessModules(proc, mods, sizeof(mods), &needed)) {
        const DWORD count = needed / sizeof(HMODULE);
        for (DWORD i = 0; i < count; ++i) {
            wchar_t name[MAX_PATH]{};
            if (GetModuleFileNameExW(proc, mods[i], name, MAX_PATH) &&
                _wcsicmp(std::filesystem::path(name).filename().wstring().c_str(),
                         dllName.c_str()) == 0) {
                found = true;
                break;
            }
        }
    }
    CloseHandle(proc);
    return found;
}

HANDLE publishPipeName(DWORD pid, const std::wstring& pipeName) {
    wchar_t mappingName[128]{};
    _snwprintf_s(mappingName, _countof(mappingName), _TRUNCATE,
                 L"Local\\OTCApiPipe_%lu", pid);
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        0, 512, mappingName);
    if (!mapping) {
        std::wcerr << L"CreateFileMapping pipe name failed err=" << GetLastError() << L"\n";
        return nullptr;
    }
    void* view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 512);
    if (!view) {
        std::wcerr << L"MapViewOfFile pipe name failed err=" << GetLastError() << L"\n";
        CloseHandle(mapping);
        return nullptr;
    }
    std::wmemset(static_cast<wchar_t*>(view), 0, 256);
    wcsncpy_s(static_cast<wchar_t*>(view), 256, pipeName.c_str(), _TRUNCATE);
    UnmapViewOfFile(view);
    return mapping;
}

HANDLE createPipeReadyEvent(DWORD pid) {
    wchar_t eventName[128]{};
    _snwprintf_s(eventName, _countof(eventName), _TRUNCATE,
                 L"Local\\OTCApiPipeReady_%lu", pid);
    return CreateEventW(nullptr, TRUE, FALSE, eventName);
}

bool inject(DWORD pid, const std::wstring& dllPath, const std::wstring& pipeName) {
    bool target64 = false;
    DWORD archErr = 0;
    if (processIs64(pid, target64, archErr)) {
        if (target64 != injectorIs64()) {
            std::wcerr << L"Architecture mismatch: process is "
                       << (target64 ? L"x64" : L"x86") << L", injector is "
                       << (injectorIs64() ? L"x64" : L"x86") << L".\n";
            return false;
        }
    } else if (archErr) {
        std::wcerr << L"Could not read process architecture (err " << archErr << L")\n";
    }

    enableDebugPrivilege();

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                  PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) {
        std::wcerr << L"OpenProcess pid=" << pid << L" failed err=" << GetLastError() << L"\n";
        return false;
    }

    HANDLE pipeMapping = nullptr;
    HANDLE pipeReady = nullptr;
    if (!pipeName.empty()) {
        pipeReady = createPipeReadyEvent(pid);
        if (!pipeReady) {
            std::wcerr << L"CreateEvent pipe ready failed err=" << GetLastError() << L"\n";
            CloseHandle(proc);
            return false;
        }
        pipeMapping = publishPipeName(pid, pipeName);
        if (!pipeMapping) {
            CloseHandle(pipeReady);
            CloseHandle(proc);
            return false;
        }
    }

    const std::size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        std::wcerr << L"VirtualAllocEx failed err=" << GetLastError() << L"\n";
        CloseHandle(proc);
        if (pipeMapping) CloseHandle(pipeMapping);
        if (pipeReady) CloseHandle(pipeReady);
        return false;
    }

    if (!WriteProcessMemory(proc, remote, dllPath.c_str(), bytes, nullptr)) {
        std::wcerr << L"WriteProcessMemory failed err=" << GetLastError() << L"\n";
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        if (pipeMapping) CloseHandle(pipeMapping);
        if (pipeReady) CloseHandle(pipeReady);
        return false;
    }

    const auto loadLibraryW = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE thread = CreateRemoteThread(proc, nullptr, 0, loadLibraryW, remote, 0, nullptr);
    if (!thread) {
        std::wcerr << L"CreateRemoteThread failed err=" << GetLastError() << L"\n";
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        if (pipeMapping) CloseHandle(pipeMapping);
        if (pipeReady) CloseHandle(pipeReady);
        return false;
    }

    WaitForSingleObject(thread, 8000);
    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);

    if (pipeReady)
        WaitForSingleObject(pipeReady, 5000);
    if (pipeMapping)
        CloseHandle(pipeMapping);
    if (pipeReady)
        CloseHandle(pipeReady);

    if (exitCode == 0) {
        std::wcerr << L"LoadLibraryW returned NULL\n";
        return false;
    }
    return true;
}

bool launchSuspended(const std::wstring& exePath, PROCESS_INFORMATION& process, DWORD& err) {
    process = {};
    err = ERROR_SUCCESS;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    std::wstring commandLine = L"\"" + exePath + L"\"";
    const std::wstring workingDirectory = std::filesystem::path(exePath).parent_path().wstring();

    if (!CreateProcessW(exePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr,
                        workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startup,
                        &process)) {
        err = GetLastError();
        return false;
    }
    return true;
}

void usage() {
    std::wcerr << L"OTCInjector\n"
               << L"  OTCInjector.exe --pid <pid>\n"
               << L"  OTCInjector.exe --name <process.exe>\n"
               << L"  OTCInjector.exe --path <full\\path\\to.exe>\n"
               << L"  OTCInjector.exe --launch <full\\path\\to.exe>\n"
               << L"  OTCInjector.exe --list\n"
               << L"  OTCInjector.exe --dll <OTCApi.dll>  "
                  L"(combine with --pid/--name/--path/--launch)\n"
               << L"  OTCInjector.exe --pipe-name <OTCApi_<pid>>\n";
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc >= 2 && std::wstring(argv[1]) == L"--list") {
        printProcessListJson();
        return 0;
    }

    std::filesystem::path dllPath =
        std::filesystem::path(argv[0]).parent_path() /
#if defined(_WIN64)
        L"OTCApi-x64.dll";
#else
        L"OTCApi-Win32.dll";
#endif

    DWORD pid = 0;
    std::wstring launchPath;
    std::wstring pipeName;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--dll" && i + 1 < argc)
            dllPath = argv[++i];
        else if (a == L"--pipe-name" && i + 1 < argc)
            pipeName = argv[++i];
        else if (a == L"--pid" && i + 1 < argc)
            pid = static_cast<DWORD>(wcstoul(argv[++i], nullptr, 10));
        else if (a == L"--name" && i + 1 < argc) {
            pid = findPidByName(argv[++i]);
            if (!pid) {
                std::wcerr << L"Process not found\n";
                return 1;
            }
        } else if (a == L"--path" && i + 1 < argc) {
            pid = findPidByPath(argv[++i]);
            if (!pid) {
                std::wcerr << L"No running process for that path\n";
                return 1;
            }
        } else if (a == L"--launch" && i + 1 < argc) {
            launchPath = canonical(argv[++i]);
        } else if (a == L"--help" || a == L"-h") {
            usage();
            return 0;
        }
    }

    if (!pid && launchPath.empty()) {
        usage();
        return 1;
    }

    if (!std::filesystem::exists(dllPath)) {
        std::wcerr << L"DLL not found: " << dllPath << L"\n";
        return 1;
    }

    const std::wstring absDll = std::filesystem::absolute(dllPath).wstring();

    if (!launchPath.empty()) {
        if (!std::filesystem::exists(launchPath)) {
            std::wcerr << L"EXE not found: " << launchPath << L"\n";
            return 1;
        }

        PROCESS_INFORMATION process{};
        DWORD launchErr = ERROR_SUCCESS;
        if (!launchSuspended(launchPath, process, launchErr)) {
            std::wcerr << L"CreateProcess suspended failed err=" << launchErr << L"\n";
            return 1;
        }

        std::wcout << L"DLL " << absDll << L"\nEXE " << launchPath << L"\nPID "
                   << process.dwProcessId << L" (suspended)\n";
        if (!inject(process.dwProcessId, absDll, pipeName)) {
            std::wcerr << L"Inject before resume failed; terminating suspended process\n";
            TerminateProcess(process.hProcess, ERROR_DLL_INIT_FAILED);
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return 1;
        }

        if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
            std::wcerr << L"ResumeThread failed err=" << GetLastError() << L"\n";
            TerminateProcess(process.hProcess, ERROR_PROCESS_ABORTED);
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return 1;
        }

        const DWORD launchedPid = process.dwProcessId;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        std::wcout << L"Launched and injected\nPID " << launchedPid
                   << L"\npipe \\\\.\\pipe\\OTCApi\n";
        return 0;
    }

    std::wcout << L"DLL " << absDll << L"\nPID " << pid << L"\n";

    if (dllAlreadyLoaded(pid, absDll)) {
        std::wcout << L"OTCApi.dll already loaded in PID " << pid
                   << L" — close the game to inject a new build\npipe \\\\.\\pipe\\OTCApi\n";
        return 0;
    }

    if (!inject(pid, absDll, pipeName)) {
        std::wcerr << L"Inject failed\n";
        return 1;
    }

    std::wcout << L"Injected\npipe \\\\.\\pipe\\OTCApi\n";
    return 0;
}

