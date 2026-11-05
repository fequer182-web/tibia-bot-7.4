#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

struct Candidate
{
    DWORD pid = 0;
    std::wstring exe;
    std::wstring path;
    std::wstring title;
    bool is64 = false;
    bool archKnown = false;
};

static std::string Utf8(const std::wstring& s)
{
    if (s.empty())
        return {};

    int needed = WideCharToMultiByte(
        CP_UTF8,
        0,
        s.c_str(),
        static_cast<int>(s.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );

    if (needed <= 0)
        return {};

    std::string out(
        static_cast<size_t>(needed),
        '\0'
    );

    WideCharToMultiByte(
        CP_UTF8,
        0,
        s.c_str(),
        static_cast<int>(s.size()),
        out.data(),
        needed,
        nullptr,
        nullptr
    );

    return out;
}

static std::wstring Lower(std::wstring s)
{
    std::transform(
        s.begin(),
        s.end(),
        s.begin(),
        [](wchar_t c)
        {
            return static_cast<wchar_t>(
                std::towlower(c)
            );
        }
    );

    return s;
}

static bool ContainsAnyTibiaHint(
    const Candidate& c)
{
    const std::wstring haystack =
        Lower(
            c.exe + L" " +
            c.path + L" " +
            c.title
        );

    static const wchar_t* hints[] =
    {
        L"tibia",
        L"tibiascape",
        L"otclient",
        L"otserv",
        L"olders",
        L"opengl",
        L"dx9",
        L"dx11",
        L"dx12"
    };

    for (const auto* hint : hints)
    {
        if (haystack.find(hint) !=
            std::wstring::npos)
        {
            return true;
        }
    }

    return false;
}

static BOOL CALLBACK EnumWindowsProc(
    HWND hwnd,
    LPARAM lParam)
{
    if (!IsWindowVisible(hwnd))
        return TRUE;

    int len =
        GetWindowTextLengthW(hwnd);

    if (len <= 0)
        return TRUE;

    DWORD pid = 0;

    GetWindowThreadProcessId(
        hwnd,
        &pid
    );

    if (!pid)
        return TRUE;

    auto* titles =
        reinterpret_cast<
            std::map<DWORD, std::wstring>*
        >(lParam);

    if (titles->find(pid) !=
        titles->end())
    {
        return TRUE;
    }

    std::wstring title(
        static_cast<size_t>(len) + 1,
        L'\0'
    );

    GetWindowTextW(
        hwnd,
        title.data(),
        len + 1
    );

    title.resize(
        static_cast<size_t>(len)
    );

    (*titles)[pid] =
        title;

    return TRUE;
}

static bool QueryProcessPath(
    DWORD pid,
    std::wstring& outPath)
{
    outPath.clear();

    HANDLE process =
        OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            pid
        );

    if (!process)
        return false;

    std::wstring buffer(
        32768,
        L'\0'
    );

    DWORD size =
        static_cast<DWORD>(
            buffer.size()
        );

    BOOL ok =
        QueryFullProcessImageNameW(
            process,
            0,
            buffer.data(),
            &size
        );

    CloseHandle(
        process
    );

    if (!ok)
        return false;

    buffer.resize(size);

    outPath =
        buffer;

    return true;
}

static bool QueryProcessIs64(
    DWORD pid,
    bool& is64)
{
    is64 = false;

    HANDLE process =
        OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            pid
        );

    if (!process)
        return false;

    using IsWow64Process2Fn =
        BOOL (WINAPI*)(
            HANDLE,
            USHORT*,
            USHORT*
        );

    auto pIsWow64Process2 =
        reinterpret_cast<
            IsWow64Process2Fn
        >(
            GetProcAddress(
                GetModuleHandleW(
                    L"kernel32.dll"
                ),
                "IsWow64Process2"
            )
        );

    bool ok = false;

    if (pIsWow64Process2)
    {
        USHORT processMachine = 0;
        USHORT nativeMachine = 0;

        if (pIsWow64Process2(
                process,
                &processMachine,
                &nativeMachine))
        {
            is64 =
                processMachine ==
                    IMAGE_FILE_MACHINE_UNKNOWN &&
                (
                    nativeMachine ==
                        IMAGE_FILE_MACHINE_AMD64 ||
                    nativeMachine ==
                        IMAGE_FILE_MACHINE_ARM64
                );

            ok = true;
        }
    }
    else
    {
        BOOL wow64 = FALSE;

        if (IsWow64Process(
                process,
                &wow64))
        {
#if defined(_WIN64)
            is64 =
                wow64 == FALSE;
#else
            is64 = false;
#endif
            ok = true;
        }
    }

    CloseHandle(
        process
    );

    return ok;
}

static std::vector<Candidate>
EnumerateProcesses()
{
    std::map<DWORD, std::wstring>
        titles;

    EnumWindows(
        EnumWindowsProc,
        reinterpret_cast<LPARAM>(
            &titles
        )
    );

    std::vector<Candidate>
        out;

    HANDLE snapshot =
        CreateToolhelp32Snapshot(
            TH32CS_SNAPPROCESS,
            0
        );

    if (snapshot ==
        INVALID_HANDLE_VALUE)
    {
        return out;
    }

    PROCESSENTRY32W pe{};
    pe.dwSize =
        sizeof(pe);

    if (Process32FirstW(
            snapshot,
            &pe))
    {
        do
        {
            if (!pe.th32ProcessID)
                continue;

            Candidate c{};
            c.pid =
                pe.th32ProcessID;
            c.exe =
                pe.szExeFile;

            QueryProcessPath(
                c.pid,
                c.path
            );

            auto titleIt =
                titles.find(
                    c.pid
                );

            if (titleIt !=
                titles.end())
            {
                c.title =
                    titleIt->second;
            }

            c.archKnown =
                QueryProcessIs64(
                    c.pid,
                    c.is64
                );

            out.push_back(
                std::move(c)
            );

        } while (
            Process32NextW(
                snapshot,
                &pe
            )
        );
    }

    CloseHandle(
        snapshot
    );

    std::sort(
        out.begin(),
        out.end(),
        [](
            const Candidate& a,
            const Candidate& b)
        {
            return a.pid < b.pid;
        }
    );

    return out;
}

static void PrintCandidate(
    size_t index,
    const Candidate& c)
{
    std::cout
        << "["
        << index
        << "] PID "
        << c.pid
        << " | ";

    if (c.archKnown)
    {
        std::cout
            << (
                c.is64
                    ? "x64"
                    : "x86"
            );
    }
    else
    {
        std::cout
            << "arch?";
    }

    std::cout
        << " | "
        << Utf8(c.exe);

    if (!c.title.empty())
    {
        std::cout
            << "\n    Window: "
            << Utf8(c.title);
    }

    if (!c.path.empty())
    {
        std::cout
            << "\n    Path  : "
            << Utf8(c.path);
    }

    std::cout
        << "\n";
}

static std::vector<Candidate>
FilterTibiaCandidates(
    const std::vector<Candidate>& all)
{
    std::vector<Candidate>
        filtered;

    for (const auto& c :
         all)
    {
        if (ContainsAnyTibiaHint(c))
        {
            filtered.push_back(
                c
            );
        }
    }

    return filtered;
}

static bool FileExists(
    const std::wstring& path)
{
    DWORD attrs =
        GetFileAttributesW(
            path.c_str()
        );

    return
        attrs !=
            INVALID_FILE_ATTRIBUTES &&
        (
            attrs &
            FILE_ATTRIBUTE_DIRECTORY
        ) == 0;
}

static std::wstring TrimQuotes(
    std::wstring s)
{
    while (!s.empty() &&
           (
               s.front() == L' ' ||
               s.front() == L'\t'
           ))
    {
        s.erase(
            s.begin()
        );
    }

    while (!s.empty() &&
           (
               s.back() == L' ' ||
               s.back() == L'\t'
           ))
    {
        s.pop_back();
    }

    if (s.size() >= 2 &&
        s.front() == L'"' &&
        s.back() == L'"')
    {
        s =
            s.substr(
                1,
                s.size() - 2
            );
    }

    return s;
}

static bool InjectorIs64()
{
#if defined(_WIN64)
    return true;
#else
    return false;
#endif
}

static bool InjectDll(
    DWORD pid,
    const std::wstring& dllPath)
{
    if (!FileExists(
            dllPath))
    {
        std::cout
            << "[ERRO] DLL nao encontrada:\n"
            << Utf8(dllPath)
            << "\n";

        return false;
    }

    bool target64 = false;

    if (!QueryProcessIs64(
            pid,
            target64))
    {
        std::cout
            << "[ERRO] Nao consegui determinar a arquitetura do alvo.\n";

        return false;
    }

    if (target64 !=
        InjectorIs64())
    {
        std::cout
            << "[ERRO] Arquitetura diferente.\n"
            << "Injector : "
            << (
                InjectorIs64()
                    ? "x64"
                    : "x86"
            )
            << "\n"
            << "Target   : "
            << (
                target64
                    ? "x64"
                    : "x86"
            )
            << "\n";

        return false;
    }

    HANDLE process =
        OpenProcess(
            PROCESS_CREATE_THREAD |
            PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE |
            PROCESS_VM_READ,
            FALSE,
            pid
        );

    if (!process)
    {
        std::cout
            << "[ERRO] OpenProcess falhou. Win32="
            << GetLastError()
            << "\n"
            << "Se o processo estiver protegido pelo Windows/anticheat, "
            << "este loader nao tenta contornar a protecao.\n";

        return false;
    }

    const SIZE_T bytes =
        (
            dllPath.size() + 1
        ) *
        sizeof(wchar_t);

    void* remote =
        VirtualAllocEx(
            process,
            nullptr,
            bytes,
            MEM_COMMIT |
            MEM_RESERVE,
            PAGE_READWRITE
        );

    if (!remote)
    {
        std::cout
            << "[ERRO] VirtualAllocEx falhou. Win32="
            << GetLastError()
            << "\n";

        CloseHandle(
            process
        );

        return false;
    }

    SIZE_T written = 0;

    if (!WriteProcessMemory(
            process,
            remote,
            dllPath.c_str(),
            bytes,
            &written) ||
        written != bytes)
    {
        std::cout
            << "[ERRO] WriteProcessMemory falhou. Win32="
            << GetLastError()
            << "\n";

        VirtualFreeEx(
            process,
            remote,
            0,
            MEM_RELEASE
        );

        CloseHandle(
            process
        );

        return false;
    }

    HMODULE kernel32 =
        GetModuleHandleW(
            L"kernel32.dll"
        );

    FARPROC loadLibrary =
        GetProcAddress(
            kernel32,
            "LoadLibraryW"
        );

    if (!loadLibrary)
    {
        std::cout
            << "[ERRO] LoadLibraryW nao encontrado.\n";

        VirtualFreeEx(
            process,
            remote,
            0,
            MEM_RELEASE
        );

        CloseHandle(
            process
        );

        return false;
    }

    HANDLE thread =
        CreateRemoteThread(
            process,
            nullptr,
            0,
            reinterpret_cast<
                LPTHREAD_START_ROUTINE
            >(loadLibrary),
            remote,
            0,
            nullptr
        );

    if (!thread)
    {
        std::cout
            << "[ERRO] CreateRemoteThread falhou. Win32="
            << GetLastError()
            << "\n";

        VirtualFreeEx(
            process,
            remote,
            0,
            MEM_RELEASE
        );

        CloseHandle(
            process
        );

        return false;
    }

    DWORD wait =
        WaitForSingleObject(
            thread,
            15000
        );

    bool ok = false;

    if (wait ==
        WAIT_OBJECT_0)
    {
        DWORD exitCode = 0;

        if (GetExitCodeThread(
                thread,
                &exitCode) &&
            exitCode != 0)
        {
            ok = true;
        }
    }

    CloseHandle(
        thread
    );

    VirtualFreeEx(
        process,
        remote,
        0,
        MEM_RELEASE
    );

    CloseHandle(
        process
    );

    if (!ok)
    {
        std::cout
            << "[ERRO] A thread remota terminou sem confirmar LoadLibraryW.\n";

        return false;
    }

    std::cout
        << "[OK] DLL carregada no PID "
        << pid
        << ".\n";

    return true;
}

static std::wstring ExeDirectory()
{
    std::wstring path(
        32768,
        L'\0'
    );

    DWORD len =
        GetModuleFileNameW(
            nullptr,
            path.data(),
            static_cast<DWORD>(
                path.size()
            )
        );

    if (!len)
        return L".";

    path.resize(len);

    std::filesystem::path p(
        path
    );

    return
        p.parent_path().wstring();
}

static bool ParseUnsigned(
    const std::string& s,
    unsigned long& value)
{
    try
    {
        size_t used = 0;

        value =
            std::stoul(
                s,
                &used,
                10
            );

        return
            used ==
            s.size();
    }
    catch (...)
    {
        return false;
    }
}

int main()
{
    SetConsoleOutputCP(
        CP_UTF8
    );

    SetConsoleCP(
        CP_UTF8
    );

    SetConsoleTitleW(
        L"OTCLab PID Injector"
    );

    std::cout
        << "============================================================\n"
        << "                   OTCLab PID Injector\n"
        << "============================================================\n"
        << "Escolhe o PID do cliente real. O launcher nao e aberto.\n"
        << "Sem bypass/stealth: se o processo bloquear acesso, o loader para.\n"
        << "============================================================\n\n";

        std::wstring defaultDll =
        ExeDirectory() +
    #if defined(_WIN64)
        L"\\OTCLab_BuildMigrator_V3_FINAL.dll";
    #else
        L"\\OTCLab_BuildMigrator_V4_9_DIRECT_BIND_x86.dll";
    #endif

    std::wstring dllPath;

    if (FileExists(
            defaultDll))
    {
        dllPath =
            defaultDll;

        std::cout
            << "[DLL DEFAULT] "
            << Utf8(dllPath)
            << "\n";
    }
    else
    {
        std::cout
            << "Cole o caminho completo da DLL:\n> ";

        std::string line;
        std::getline(
            std::cin,
            line
        );

        dllPath =
            TrimQuotes(
                std::filesystem::path(
                    line
                ).wstring()
            );
    }

    if (!FileExists(
            dllPath))
    {
        std::cout
            << "\n[ERRO] DLL inexistente: "
            << Utf8(dllPath)
            << "\n";

        return 1;
    }

    for (;;)
    {
        auto all =
            EnumerateProcesses();

        auto candidates =
            FilterTibiaCandidates(
                all
            );

        std::cout
            << "\n============================================================\n"
            << "                PROCESSOS TIBIA / OT CANDIDATOS\n"
            << "============================================================\n";

        if (candidates.empty())
        {
            std::cout
                << "Nenhum candidato por nome/titulo foi encontrado.\n";
        }
        else
        {
            for (size_t i = 0;
                 i < candidates.size();
                 ++i)
            {
                PrintCandidate(
                    i + 1,
                    candidates[i]
                );
            }
        }

        std::cout
            << "\nDigite:\n"
            << "  numero = escolher candidato\n"
            << "  P <pid> = escolher PID exato\n"
            << "  A = mostrar todos os processos com janela\n"
            << "  R = atualizar lista\n"
            << "  Q = sair\n"
            << "> ";

        std::string input;

        std::getline(
            std::cin,
            input
        );

        if (input.empty())
            continue;

        std::string upper =
            input;

        std::transform(
            upper.begin(),
            upper.end(),
            upper.begin(),
            [](unsigned char c)
            {
                return
                    static_cast<char>(
                        std::toupper(c)
                    );
            }
        );

        if (upper == "Q")
            return 0;

        if (upper == "R")
            continue;

        if (upper == "A")
        {
            std::cout
                << "\n============================================================\n"
                << "                    TODOS COM JANELA\n"
                << "============================================================\n";

            size_t shown = 0;

            for (const auto& c :
                 all)
            {
                if (c.title.empty())
                    continue;

                ++shown;

                PrintCandidate(
                    shown,
                    c
                );
            }

            continue;
        }

        DWORD selectedPid = 0;
        Candidate selected{};
        bool haveSelected = false;

        if (upper.size() > 2 &&
            upper[0] == 'P' &&
            upper[1] == ' ')
        {
            unsigned long pidValue = 0;

            if (!ParseUnsigned(
                    input.substr(2),
                    pidValue))
            {
                std::cout
                    << "[ERRO] PID invalido.\n";

                continue;
            }

            selectedPid =
                static_cast<DWORD>(
                    pidValue
                );

            for (const auto& c :
                 all)
            {
                if (c.pid ==
                    selectedPid)
                {
                    selected =
                        c;

                    haveSelected = true;
                    break;
                }
            }

            if (!haveSelected)
            {
                std::cout
                    << "[ERRO] PID nao encontrado na lista atual.\n";

                continue;
            }
        }
        else
        {
            unsigned long index = 0;

            if (!ParseUnsigned(
                    input,
                    index) ||
                index == 0 ||
                index >
                    candidates.size())
            {
                std::cout
                    << "[ERRO] Opcao invalida.\n";

                continue;
            }

            selected =
                candidates[
                    static_cast<size_t>(
                        index - 1
                    )
                ];

            selectedPid =
                selected.pid;

            haveSelected = true;
        }

        if (!haveSelected)
            continue;

        std::cout
            << "\n============================================================\n"
            << "                     TARGET SELECIONADO\n"
            << "============================================================\n";

        PrintCandidate(
            1,
            selected
        );

        std::cout
            << "DLL   : "
            << Utf8(dllPath)
            << "\n"
            << "Injetar neste PID? [S/N]: ";

        std::string confirm;

        std::getline(
            std::cin,
            confirm
        );

        if (confirm.empty() ||
            (
                confirm[0] != 's' &&
                confirm[0] != 'S'
            ))
        {
            std::cout
                << "[CANCELADO]\n";

            continue;
        }

        if (InjectDll(
                selectedPid,
                dllPath))
        {
            std::cout
                << "\nO BuildMigrator agora deve imprimir o fingerprint "
                << "do executavel REAL desse PID.\n";

            return 0;
        }

        std::cout
            << "\nFalhou. Pressione ENTER para voltar a lista...";

        std::string dummy;
        std::getline(
            std::cin,
            dummy
        );
    }
}
