#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <filesystem>
#include "GuidedFields.hpp"

namespace {

static_assert(sizeof(void*) == 8, "Compile este arquivo como x64.");

HANDLE g_process = nullptr;
DWORD g_pid = 0;
uintptr_t g_moduleBase = 0;
uintptr_t g_moduleEnd = 0;
uintptr_t g_textBegin = 0;
uintptr_t g_textEnd = 0;
std::string g_processPath;
std::map<std::string,guided::Evidence> g_guided;
uintptr_t g_guidedDirect=0,g_guidedIndirect=0,g_guidedOwnerOffset=0;

struct BuildInfo {
    DWORD timestamp = 0;
    DWORD sizeOfImage = 0;
    DWORD entryPointRva = 0;
    DWORD textRva = 0;
    DWORD textSize = 0;
    uint64_t textHash = 0;
};

struct ModuleSection {
    std::string name;
    uintptr_t begin = 0;
    uintptr_t end = 0;
    DWORD characteristics = 0;
    bool readable() const { return (characteristics & IMAGE_SCN_MEM_READ) != 0; }
    bool writable() const { return (characteristics & IMAGE_SCN_MEM_WRITE) != 0; }
    bool executable() const { return (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0; }
};

struct StatsInput {
    double health = 0;
    double maxHealth = 0;
    double mana = 0;
    double maxMana = 0;
    double experience = 0;
    double level = 0;
    double capacity = 0;
};

struct LocalCandidate {
    uintptr_t base = 0;
    uintptr_t vtable = 0;
    int score = 0;
    std::map<std::string, std::vector<uintptr_t>> fieldOffsets;
};

struct PosFields {
    uintptr_t x = 0;
    uintptr_t y = 0;
    uintptr_t z = 0;
    uintptr_t direction = 0;
};

struct IndirectPathCandidate {
    uintptr_t globalRva = 0;
    uintptr_t owner = 0;
    uintptr_t ownerOffset = 0;
    int score = 0;
};

struct PointerPath {
    uintptr_t directGlobalRva = 0;
    std::vector<uintptr_t> allDirectRvas;
    uintptr_t indirectGlobalRva = 0;
    uintptr_t indirectOwner = 0;
    uintptr_t ownerOffset = 0;
    std::vector<IndirectPathCandidate> indirectCandidates;
};

struct GameDiscovery {
    bool available = false;
    uintptr_t runtimeBase = 0;
    uintptr_t globalRva = 0;
    uintptr_t localPlayerOffset = 0;
    uintptr_t attackTargetOffset = 0;
    uintptr_t followTargetOffset = 0;
    std::string source;
};

struct MapCenterDiscovery {
    bool confirmed = false;
    uintptr_t centerRva = 0;
    uintptr_t candidateMapRva = 0;
};

struct FunctionResolution {
    std::string name;
    std::string signatureKey;
    uintptr_t rva = 0;
    int matches = 0;
    std::string status = "pending";
};

struct LightCandidate {
    uintptr_t offset = 0;
    uint8_t base = 0;
    uint8_t on = 0;
    uint8_t back = 0;
    bool alsoLux = false;
    uint8_t lux = 0;
    int score = 0;
};

std::string Hex(uint64_t v) {
    std::ostringstream ss;
    ss << "0x" << std::uppercase << std::hex << v;
    return ss.str();
}

std::string ExeDirectory() {
    wchar_t path[32768]{};
    if(!GetModuleFileNameW(nullptr,path,32768)) throw std::runtime_error("Nao consegui localizar a pasta do programa.");
    std::string s=std::filesystem::path(path).u8string();
    auto p = s.find_last_of("\\/");
    if (p == std::string::npos) return ".";
    return s.substr(0, p);
}

void WaitEnter(const std::string& msg) {
    std::cout << msg << "\n";
    std::string line;
    std::getline(std::cin, line);
}

bool AskYesNo(const std::string& msg) {
    for (;;) {
        std::cout << msg;
        std::string s;
        std::getline(std::cin, s);
        if (s.empty()) continue;
        char c = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
        if (c == 's' || c == 'y') return true;
        if (c == 'n') return false;
    }
}

double AskDouble(const std::string& msg) {
    for (;;) {
        std::cout << msg;
        std::string s;
        std::getline(std::cin, s);
        std::replace(s.begin(), s.end(), ',', '.');
        try { size_t used=0;double v=std::stod(s,&used);if(used==s.size() && std::isfinite(v) && v>=0 && v<=9007199254740991.0)return v; } catch (...) {}
        std::cout << "[INPUT] Numero invalido.\n";
    }
}

DWORD AskPid() {
    for (;;) {
        std::cout << "PID do cliente: ";
        std::string s;
        std::getline(std::cin, s);
        try {
            unsigned long v = std::stoul(s);
            if (v > 0) return static_cast<DWORD>(v);
        } catch (...) {}
        std::cout << "[INPUT] PID invalido.\n";
    }
}

bool IsReadableProtection(DWORD protect) {
    if (protect & PAGE_GUARD) return false;
    if (protect & PAGE_NOACCESS) return false;
    switch (protect & 0xFF) {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
    }
    return false;
}

bool ReadBytes(uintptr_t address, void* out, size_t size) {
    if (!g_process || !address || !out || !size) return false;
    SIZE_T got = 0;
    if (!ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(address), out, size, &got))
        return false;
    return got == size;
}

template<class T>
bool ReadT(uintptr_t address, T& out) {
    return ReadBytes(address, &out, sizeof(T));
}

uintptr_t ReadPtr(uintptr_t a) { uintptr_t v=0; ReadT(a,v); return v; }
uint32_t ReadU32(uintptr_t a) { uint32_t v=0; ReadT(a,v); return v; }
uint8_t ReadU8(uintptr_t a) { uint8_t v=0; ReadT(a,v); return v; }

bool QueryRemote(uintptr_t address, MEMORY_BASIC_INFORMATION& mbi) {
    return VirtualQueryEx(g_process, reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == sizeof(mbi);
}

bool RemoteRangeReadable(uintptr_t address, size_t size) {
    if (!address || !size) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!QueryRemote(address, mbi)) return false;
    if (mbi.State != MEM_COMMIT || !IsReadableProtection(mbi.Protect)) return false;
    uintptr_t b = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    uintptr_t e = b + mbi.RegionSize;
    return address >= b && address + size >= address && address + size <= e;
}

void PrintProcesses() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    std::cout << "\nProcessos visiveis (PID / EXE):\n";
    if (Process32First(snap, &pe)) {
        do {
            std::cout << std::setw(7) << pe.th32ProcessID << "  " << pe.szExeFile << "\n";
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
}

bool OpenTarget(DWORD pid) {
    g_pid = pid;
    g_process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!g_process) {
        std::cout << "[FAIL] OpenProcess READ-ONLY falhou. Win32=" << GetLastError() << "\n";
        std::cout << "O cliente pode estar bloqueando leitura externa. Nao sera tentado bypass.\n";
        return false;
    }

    char path[32768]{};
    DWORD size = static_cast<DWORD>(sizeof(path));
    if (QueryFullProcessImageNameA(g_process, 0, path, &size))
        g_processPath.assign(path, size);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) {
        std::cout << "[FAIL] Snapshot de modulos falhou. Win32=" << GetLastError() << "\n";
        return false;
    }

    MODULEENTRY32 me{};
    me.dwSize = sizeof(me);
    if (!Module32First(snap, &me)) {
        std::cout << "[FAIL] Module32First falhou. Win32=" << GetLastError() << "\n";
        CloseHandle(snap);
        return false;
    }

    g_moduleBase = reinterpret_cast<uintptr_t>(me.modBaseAddr);
    g_moduleEnd = g_moduleBase + me.modBaseSize;
    if (g_processPath.empty()) g_processPath = me.szExePath;
    CloseHandle(snap);
    return true;
}

uint64_t Fnv1a64Remote(uintptr_t address, size_t size) {
    constexpr uint64_t OFFSET = 14695981039346656037ull;
    constexpr uint64_t PRIME  = 1099511628211ull;
    uint64_t h = OFFSET;
    const size_t chunk = 1u << 20;
    std::vector<uint8_t> buf(chunk);
    size_t done = 0;
    while (done < size) {
        size_t want = std::min(chunk, size - done);
        SIZE_T got = 0;
        if (!ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(address + done), buf.data(), want, &got) || got == 0)
            return 0;
        for (SIZE_T i=0;i<got;++i) { h ^= buf[i]; h *= PRIME; }
        done += static_cast<size_t>(got);
        if (got < want) return 0;
    }
    return h;
}

bool ResolveBuild(BuildInfo& out) {
    IMAGE_DOS_HEADER dos{};
    if (!ReadT(g_moduleBase, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) return false;
    uintptr_t ntAddr = g_moduleBase + static_cast<uintptr_t>(dos.e_lfanew);
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadT(ntAddr, nt) || nt.Signature != IMAGE_NT_SIGNATURE) return false;
    if (nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        std::cout << "[FAIL] Cliente nao e x64 (Machine=" << Hex(nt.FileHeader.Machine) << ").\n";
        return false;
    }
    out.timestamp = nt.FileHeader.TimeDateStamp;
    out.sizeOfImage = nt.OptionalHeader.SizeOfImage;
    out.entryPointRva = nt.OptionalHeader.AddressOfEntryPoint;
    g_moduleEnd = g_moduleBase + out.sizeOfImage;

    uintptr_t secAddr = ntAddr + FIELD_OFFSET(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    for (unsigned i=0;i<nt.FileHeader.NumberOfSections;++i) {
        IMAGE_SECTION_HEADER sh{};
        if (!ReadT(secAddr + i*sizeof(sh), sh)) continue;
        char name[9]{}; std::memcpy(name, sh.Name, 8);
        if (std::strcmp(name, ".text") == 0) {
            out.textRva = sh.VirtualAddress;
            out.textSize = std::max(sh.Misc.VirtualSize, sh.SizeOfRawData);
            g_textBegin = g_moduleBase + out.textRva;
            g_textEnd = std::min<uintptr_t>(g_moduleEnd, g_textBegin + out.textSize);
            out.textHash = Fnv1a64Remote(g_textBegin, static_cast<size_t>(g_textEnd-g_textBegin));
            break;
        }
    }
    return g_textBegin && g_textEnd > g_textBegin;
}

std::vector<ModuleSection> GetSections() {
    std::vector<ModuleSection> out;
    IMAGE_DOS_HEADER dos{};
    if (!ReadT(g_moduleBase, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) return out;
    uintptr_t ntAddr = g_moduleBase + static_cast<uintptr_t>(dos.e_lfanew);
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadT(ntAddr, nt) || nt.Signature != IMAGE_NT_SIGNATURE) return out;
    uintptr_t secAddr = ntAddr + FIELD_OFFSET(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    for (unsigned i=0;i<nt.FileHeader.NumberOfSections;++i) {
        IMAGE_SECTION_HEADER sh{};
        if (!ReadT(secAddr + i*sizeof(sh), sh)) continue;
        char name[9]{}; std::memcpy(name, sh.Name, 8);
        ModuleSection s{};
        s.name = name;
        s.begin = g_moduleBase + sh.VirtualAddress;
        uintptr_t sz = std::max(sh.Misc.VirtualSize, sh.SizeOfRawData);
        s.end = std::min<uintptr_t>(g_moduleEnd, s.begin + sz);
        s.characteristics = sh.Characteristics;
        if (s.end > s.begin) out.push_back(s);
    }
    return out;
}

bool InExecutableSection(uintptr_t p, const std::vector<ModuleSection>& sections) {
    for (const auto& s: sections)
        if (s.executable() && p >= s.begin && p < s.end) return true;
    return false;
}

bool PlausibleVtable(uintptr_t vt, const std::vector<ModuleSection>& sections) {
    if (vt < g_moduleBase || vt >= g_moduleEnd) return false;
    int exec = 0;
    for (int i=0;i<6;++i) {
        uintptr_t fn = ReadPtr(vt + i*sizeof(uintptr_t));
        if (InExecutableSection(fn, sections)) ++exec;
    }
    return exec >= 2;
}

std::vector<std::pair<uintptr_t,size_t>> EnumeratePrivateRwRegions(size_t maxTotalBytes = 3ull*1024*1024*1024) {
    std::vector<std::pair<uintptr_t,size_t>> out;
    SYSTEM_INFO si{}; GetSystemInfo(&si);
    uintptr_t cur = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    uintptr_t max = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    size_t total = 0;
    while (cur < max && total < maxTotalBytes) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!QueryRemote(cur, mbi)) break;
        uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        size_t sz = static_cast<size_t>(mbi.RegionSize);
        uintptr_t next = base + sz;
        if (next <= cur) break;
        DWORD p = mbi.Protect & 0xFF;
        bool rw = p == PAGE_READWRITE || p == PAGE_WRITECOPY;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && rw && !(mbi.Protect & PAGE_GUARD)) {
            if (total + sz > maxTotalBytes) sz = maxTotalBytes-total;
            if (sz) { out.push_back({base,sz}); total += sz; }
        }
        cur = next;
    }
    return out;
}

bool SameDouble(double a, double b) {
    if (!std::isfinite(a) || !std::isfinite(b)) return false;
    double scale = std::max({1.0, std::fabs(a), std::fabs(b)});
    return std::fabs(a-b) <= 1e-9*scale;
}

std::vector<uintptr_t> ScanDouble(double needle, size_t maxHits = 12000) {
    std::vector<uintptr_t> hits;
    auto regions = EnumeratePrivateRwRegions();
    const size_t chunk = 4u*1024u*1024u;
    std::vector<uint8_t> buf(chunk + 8);
    size_t scanned = 0;
    for (const auto& r: regions) {
        uintptr_t cur = r.first, end = r.first+r.second;
        while (cur < end && hits.size() < maxHits) {
            size_t want = static_cast<size_t>(std::min<uintptr_t>(chunk, end-cur));
            SIZE_T got=0;
            if (!ReadProcessMemory(g_process, reinterpret_cast<LPCVOID>(cur), buf.data(), want, &got) || got < sizeof(double)) {
                cur += want; continue;
            }
            scanned += static_cast<size_t>(got);
            uintptr_t aligned = (cur+7)&~static_cast<uintptr_t>(7);
            size_t start = static_cast<size_t>(aligned-cur);
            for (size_t off=start; off+8<=got; off+=8) {
                double v=0; std::memcpy(&v,buf.data()+off,8);
                if (SameDouble(v,needle)) hits.push_back(cur+off);
                if (hits.size()>=maxHits) break;
            }
            cur += static_cast<size_t>(got);
        }
        if (hits.size()>=maxHits) break;
    }
    std::cout << "[SCAN] value=" << std::fixed << std::setprecision(4) << needle
              << " hits=" << hits.size() << " scanned=" << (scanned/(1024*1024)) << " MB\n";
    return hits;
}

std::vector<uintptr_t> ExactDoubleOffsets(uintptr_t base, double needle, uintptr_t begin=0, uintptr_t end=0x1200) {
    std::vector<uintptr_t> out;
    for (uintptr_t off=begin; off+8<=end; off+=8) {
        double v=0;
        if (ReadT(base+off,v) && SameDouble(v,needle)) out.push_back(off);
    }
    return out;
}

int PositionShapeScore(uintptr_t base) {
    int best = 0;
    for (uintptr_t off=0; off+9<0x120; off+=4) {
        uint32_t x=ReadU32(base+off), y=ReadU32(base+off+4);
        uint8_t z=ReadU8(base+off+8);
        if (x>=1000 && x<=100000 && y>=1000 && y<=100000 && z<=31) best = std::max(best, 8);
    }
    return best;
}

std::vector<LocalCandidate> DiscoverCandidates(const StatsInput& in) {
    std::cout << "\n============================================================\n"
              << "             EXTERNAL LOCALPLAYER DISCOVERY\n"
              << "============================================================\n";

    auto healthHits = ScanDouble(in.health);
    auto maxHits = ScanDouble(in.maxHealth);
    if (maxHits.empty()) return {};
    // HP may regenerate while the two full-memory scans run. Max HP also
    // anchors structural candidates; guided observations decide the real fields.
    healthHits.insert(healthHits.end(),maxHits.begin(),maxHits.end());
    std::sort(healthHits.begin(),healthHits.end());
    healthHits.erase(std::unique(healthHits.begin(),healthHits.end()),healthHits.end());

    const auto sections = GetSections();
    std::set<uintptr_t> seen;
    std::vector<LocalCandidate> candidates;

    std::sort(maxHits.begin(), maxHits.end());

    for (uintptr_t h : healthHits) {
        const uintptr_t low = h > 0x900 ? h - 0x900 : 0;
        const uintptr_t high = h + 0x900;
        auto it = std::lower_bound(maxHits.begin(), maxHits.end(), low);
        for (; it != maxHits.end() && *it <= high; ++it) {
            uintptr_t m = *it;
            uintptr_t first = std::min(h,m);
            MEMORY_BASIC_INFORMATION mbi{};
            if (!QueryRemote(first,mbi)) continue;
            uintptr_t regionBegin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            uintptr_t scanBegin = first > 0x1200 ? first-0x1200 : regionBegin;
            if (scanBegin < regionBegin) scanBegin = regionBegin;
            scanBegin = (scanBegin+7)&~static_cast<uintptr_t>(7);
            for (uintptr_t base=scanBegin; base<=first && base+8<=first+8; base+=8) {
                if (seen.count(base)) continue;
                uintptr_t vt = ReadPtr(base);
                if (!PlausibleVtable(vt,sections)) continue;

                LocalCandidate c{};
                c.base=base; c.vtable=vt;
                c.fieldOffsets["health"] = ExactDoubleOffsets(base,in.health);
                c.fieldOffsets["maxHealth"] = ExactDoubleOffsets(base,in.maxHealth);
                c.fieldOffsets["mana"] = ExactDoubleOffsets(base,in.mana);
                c.fieldOffsets["maxMana"] = ExactDoubleOffsets(base,in.maxMana);
                c.fieldOffsets["experience"] = ExactDoubleOffsets(base,in.experience);
                c.fieldOffsets["level"] = ExactDoubleOffsets(base,in.level);
                c.fieldOffsets["capacity"] = ExactDoubleOffsets(base,in.capacity);

                int fields=0, unique=0;
                for (const auto& kv:c.fieldOffsets) {
                    if (!kv.second.empty()) { ++fields; if (kv.second.size()==1) ++unique; }
                }
                int positionShape=PositionShapeScore(base);
                if (!guided::SeedAllowed(!c.fieldOffsets["maxHealth"].empty(),fields,positionShape)) continue;
                c.score = fields*20 + unique*8 + positionShape;
                if (h>=base && h-base<0x1200) c.score += 8;
                if (m>=base && m-base<0x1200) c.score += 8;
                seen.insert(base);
                candidates.push_back(std::move(c));
            }
        }
    }

    std::sort(candidates.begin(),candidates.end(),[](const LocalCandidate&a,const LocalCandidate&b){
        if (a.score!=b.score) return a.score>b.score;
        return a.base<b.base;
    });
    if (candidates.size()>20) candidates.resize(20);
    return candidates;
}

void PrintCandidate(const LocalCandidate& c, size_t index) {
    std::cout << "#" << index << " score=" << c.score << " base=" << Hex(c.base)
              << " vtRva=" << Hex(c.vtable-g_moduleBase) << "\n";
    for (const auto& kv:c.fieldOffsets) {
        std::cout << "   " << std::setw(12) << std::left << kv.first << ": ";
        if (kv.second.empty()) std::cout << "<none>";
        else {
            for (size_t i=0;i<kv.second.size() && i<4;++i) {
                if (i) std::cout << ", ";
                std::cout << "+" << Hex(kv.second[i]);
            }
            if (kv.second.size()>4) std::cout << " ...";
        }
        std::cout << "\n";
    }
}

size_t AskCandidateIndex(size_t count) {
    for (;;) {
        if(count==1)return 0;
        std::cout << "Escolha explicitamente o candidato LocalPlayer (0.." << (count-1) << "): ";
        std::string s; std::getline(std::cin,s);
        try { size_t v=static_cast<size_t>(std::stoul(s)); if (v<count) return v; } catch (...) {}
        std::cout << "[INPUT] Indice invalido.\n";
    }
}

std::vector<uint8_t> ReadGuidedSnapshot(const LocalCandidate& lc) {
    auto samePlayer=[&](){
        if(g_guidedDirect)return ReadPtr(g_moduleBase+g_guidedDirect)==lc.base;
        if(g_guidedIndirect){auto owner=ReadPtr(g_moduleBase+g_guidedIndirect);return owner && ReadPtr(owner+g_guidedOwnerOffset)==lc.base;}
        return false;
    };
    if(!samePlayer())throw std::runtime_error("O caminho do LocalPlayer mudou ou nao pode ser validado. Reinicie a descoberta.");
    if(ReadPtr(lc.base)!=lc.vtable) throw std::runtime_error("LocalPlayer mudou ou ficou indisponivel. Reinicie a descoberta apos entrar no personagem.");
    std::vector<uint8_t> bytes(0x1200);
    if(!ReadBytes(lc.base,bytes.data(),bytes.size()) || ReadPtr(lc.base)!=lc.vtable || !samePlayer()) throw std::runtime_error("Falha na leitura do personagem. Nenhum zero artificial sera usado como evidencia.");
    return bytes;
}

void GuideStats(LocalCandidate& lc) {
    struct Step {const char* key;const char* label;const char* action;};
    const Step steps[]={
        {"health","Vida atual","Tome um pouco de dano ou recupere vida. Espere estabilizar; nao se coloque em risco."},
        {"mana","Mana atual","Gaste mana com uma magia e espere estabilizar; na outra rodada recupere ou gaste mais mana."},
        {"capacity","Capacidade livre","Mova um item de peso conhecido entre o chao e a mochila. Na outra rodada, devolva ou mova outro item."},
        {"experience","Experiencia","Ganhe experiencia matando uma criatura. Repita com outra criatura na segunda rodada."},
        {"maxHealth","Vida maxima","Este valor exige uma mudanca real, como subir de nivel. Pode pular se nao conseguir agora."},
        {"maxMana","Mana maxima","Este valor exige uma mudanca real, como subir de nivel. Pode pular se nao conseguir agora."},
        {"level","Nivel","Este valor exige subir de nivel. Pode pular; nao sera declarado confirmado sem mudanca."}
    };
    std::cout<<"\n=== DESCOBERTA GUIADA DOS ATRIBUTOS ===\n"
             <<"Use os valores da tela. Nao relogue durante esta etapa. Cada campo pode ser pulado.\n"
             <<"Confirmacao exige um unico candidato apos DUAS mudancas reais.\n";
    for(const auto& step:steps) {
        auto& e=g_guided[step.key];
        std::cout<<"\n[CAMPO] "<<step.label<<"\nO que fazer: "<<step.action<<"\n";
        double current=AskDouble(std::string("Valor ATUAL de ")+step.label+": ");
        e.observations.push_back(current);e.candidates=guided::Find(ReadGuidedSnapshot(lc),current);
        std::cout<<"[ANTES] "<<e.candidates.size()<<" candidatos de leitura (double, float e inteiros).\n";
        e.reason="unchanged_value_only";
        for(unsigned round=1;round<=3 && !(e.candidates.size()==1 && e.transitions>=2);++round) {
            if(e.candidates.empty()){e.reason="no_matching_candidate";break;}
            if(!AskYesNo("Pode mudar este valor agora? [S/N]: ")){e.reason="user_skipped_changes";break;}
            WaitEnter(std::string("Faca a acao indicada para ")+step.label+"; espere estabilizar e pressione ENTER...");
            double value=AskDouble("Novo valor mostrado no jogo: ");
            if(std::abs(value-e.observations.back())<=0.0051) {std::cout<<"[SEM MUDANCA] Isso nao confirma o campo.\n";e.reason="value_did_not_change";continue;}
            guided::Narrow(e,ReadGuidedSnapshot(lc),value);
            std::cout<<"[COMPARACAO] "<<e.candidates.size()<<" candidatos restantes; "<<e.transitions<<" mudancas observadas.\n";
        }
        auto& offsets=lc.fieldOffsets[step.key];offsets.clear();
        // Legacy consumers only understand double. Typed candidates remain in guidedFields.
        if(e.candidates.size()==1 && e.candidates[0].type=="float64")offsets.push_back(e.candidates[0].offset);
        std::cout<<"[RESULTADO] "<<step.label<<": "<<(e.status()=="confirmed_read_transition"?"CONFIRMADO POR LEITURA":e.status()=="ambiguous"?"AINDA AMBIGUO":e.status()=="pending"?"NAO ENCONTRADO":"CANDIDATO; FALTA CONFIRMAR")<<"\n";
        if(e.candidates.size()==1)std::cout<<"  Offset +"<<Hex(e.candidates[0].offset)<<"; tipo "<<e.candidates[0].type<<"\n";
    }
}

void PrintGuidedSummary() {
    std::cout<<"\n=== RESUMO DOS ATRIBUTOS ===\n";
    for(const auto& kv:g_guided)std::cout<<kv.first<<": "<<kv.second.status()<<" | candidatos="<<kv.second.candidates.size()<<" | mudancas="<<kv.second.transitions<<"\n";
    std::cout<<"Confirmado por leitura NAO significa ABI de funcao validada.\n";
}

std::vector<uint32_t> SnapshotU32(uintptr_t base, uintptr_t end=0x120) {
    std::vector<uint32_t> v(end/4,0);
    for (uintptr_t off=0; off+4<=end; off+=4) ReadT(base+off,v[off/4]);
    return v;
}

std::vector<uint8_t> SnapshotU8(uintptr_t base, uintptr_t end=0x1200) {
    std::vector<uint8_t> v(end,0);
    ReadBytes(base,v.data(),v.size());
    return v;
}

uintptr_t FindChangedU32Offset(const std::vector<uint32_t>& a,const std::vector<uint32_t>& b,bool coord) {
    std::vector<uintptr_t> hits;
    size_t n=std::min(a.size(),b.size());
    for (size_t i=0;i<n;++i) {
        if (a[i]==b[i]) continue;
        if (coord) {
            if (a[i]<1000||a[i]>100000||b[i]<1000||b[i]>100000) continue;
            uint32_t d=a[i]>b[i]?a[i]-b[i]:b[i]-a[i];
            if (d!=1) continue;
        } else {
            if (a[i]>7||b[i]>7) continue;
        }
        hits.push_back(i*4);
    }
    if (hits.size()==1) return hits[0];
    if (!hits.empty()) {
        std::cout << "[AMBIGUOUS] offsets:";
        for (auto h:hits) std::cout << " +" << Hex(h);
        std::cout << "\n";
    }
    return 0;
}

PosFields CalibratePosition(uintptr_t base) {
    PosFields p{};
    std::cout << "\n============================================================\n"
              << "             POSITION / DIRECTION EXTERNAL\n"
              << "============================================================\n";
    auto a=SnapshotU32(base);
    WaitEnter("Mova EXATAMENTE 1 sqm para direita/esquerda e pressione ENTER...");
    auto b=SnapshotU32(base);
    p.x=FindChangedU32Offset(a,b,true);
    std::cout << "X offset: " << (p.x?Hex(p.x):"pending") << "\n";

    a=SnapshotU32(base);
    WaitEnter("Mova EXATAMENTE 1 sqm para cima/baixo e pressione ENTER...");
    b=SnapshotU32(base);
    p.y=FindChangedU32Offset(a,b,true);
    std::cout << "Y offset: " << (p.y?Hex(p.y):"pending") << "\n";

    if (AskYesNo("Consegue subir/descer 1 andar agora? [S/N]: ")) {
        auto zA=SnapshotU8(base,0x120);
        WaitEnter("Suba/desca exatamente 1 andar e pressione ENTER...");
        auto zB=SnapshotU8(base,0x120);
        std::vector<uintptr_t> hits;
        for (size_t i=0;i<zA.size();++i) {
            if (zA[i]<=31 && zB[i]<=31 && zA[i]!=zB[i]) {
                int d=std::abs(int(zA[i])-int(zB[i]));
                if (d==1) hits.push_back(i);
            }
        }
        if (p.y && std::find(hits.begin(),hits.end(),p.y+4)!=hits.end()) p.z=p.y+4;
        else if (hits.size()==1) p.z=hits[0];
        std::cout << "Z offset: " << (p.z?Hex(p.z):"pending") << "\n";
    } else if (p.y) {
        uint8_t z=ReadU8(base+p.y+4);
        if (z<=31) p.z=p.y+4;
    }

    if (AskYesNo("Consegue virar SEM andar? [S/N]: ")) {
        auto dA=SnapshotU32(base);
        WaitEnter("Vire para outro lado sem andar e pressione ENTER...");
        auto dB=SnapshotU32(base);
        WaitEnter("Vire novamente sem andar e pressione ENTER...");
        auto dC=SnapshotU32(base);
        std::vector<uintptr_t> hits;
        size_t n=std::min({dA.size(),dB.size(),dC.size()});
        for (size_t i=0;i<n;++i) {
            if (dA[i]<=7&&dB[i]<=7&&dC[i]<=7 && dA[i]!=dB[i] && dB[i]!=dC[i]) hits.push_back(i*4);
        }
        if (hits.size()==1) p.direction=hits[0];
        else if (!hits.empty()) {
            std::cout << "[DIRECTION AMBIGUOUS]";
            for(auto h:hits) std::cout << " +" << Hex(h);
            std::cout << "\n";
        }
    }
    std::cout << "Direction offset: " << (p.direction?Hex(p.direction):"pending") << "\n";
    return p;
}

PointerPath FindPointerPaths(uintptr_t localPlayer) {
    PointerPath out{};
    auto sections=GetSections();
    const size_t chunk=1u<<20;
    std::vector<uint8_t> buf(chunk+8);

    // Build a map of pointer values stored in non-executable module sections.
    std::unordered_map<uintptr_t,std::vector<uintptr_t>> modulePointerRvas;
    for (const auto&s:sections) {
        if (!s.readable() || s.executable()) continue;
        uintptr_t cur=s.begin;
        while(cur<s.end) {
            size_t want=static_cast<size_t>(std::min<uintptr_t>(chunk,s.end-cur));
            SIZE_T got=0;
            if (!ReadProcessMemory(g_process,reinterpret_cast<LPCVOID>(cur),buf.data(),want,&got)||got<8) {cur+=want;continue;}
            uintptr_t aligned=(cur+7)&~static_cast<uintptr_t>(7);
            size_t off0=static_cast<size_t>(aligned-cur);
            for(size_t off=off0;off+8<=got;off+=8){
                uintptr_t v=0;std::memcpy(&v,buf.data()+off,8);
                uintptr_t rva=cur+off-g_moduleBase;
                if(v==localPlayer) out.allDirectRvas.push_back(rva);
                if(v>=0x10000ull) modulePointerRvas[v].push_back(rva);
            }
            cur+=static_cast<size_t>(got);
        }
    }
    std::sort(out.allDirectRvas.begin(),out.allDirectRvas.end());
    out.allDirectRvas.erase(std::unique(out.allDirectRvas.begin(),out.allDirectRvas.end()),out.allDirectRvas.end());
    if(!out.allDirectRvas.empty()) out.directGlobalRva=out.allDirectRvas[0];

    // Find heap/object fields that point at LocalPlayer, then see whether the module stores a pointer to that owner.
    auto regions=EnumeratePrivateRwRegions(2ull*1024*1024*1024);
    std::vector<uintptr_t> lpFieldAddresses;
    const size_t bigChunk=4u*1024u*1024u;
    std::vector<uint8_t> big(bigChunk+8);
    for(const auto&r:regions){
        uintptr_t cur=r.first,end=r.first+r.second;
        while(cur<end && lpFieldAddresses.size()<4096){
            size_t want=static_cast<size_t>(std::min<uintptr_t>(bigChunk,end-cur));
            SIZE_T got=0;
            if(!ReadProcessMemory(g_process,reinterpret_cast<LPCVOID>(cur),big.data(),want,&got)||got<8){cur+=want;continue;}
            uintptr_t aligned=(cur+7)&~static_cast<uintptr_t>(7);
            size_t off0=static_cast<size_t>(aligned-cur);
            for(size_t off=off0;off+8<=got;off+=8){
                uintptr_t v=0;std::memcpy(&v,big.data()+off,8);
                if(v==localPlayer) lpFieldAddresses.push_back(cur+off);
            }
            cur+=static_cast<size_t>(got);
        }
        if(lpFieldAddresses.size()>=4096) break;
    }

    std::set<std::tuple<uintptr_t,uintptr_t,uintptr_t>> seen;
    for(uintptr_t fieldAddr:lpFieldAddresses){
        for(uintptr_t off=0;off<=0x300;off+=8){
            if(fieldAddr<off) continue;
            uintptr_t owner=fieldAddr-off;
            auto it=modulePointerRvas.find(owner);
            if(it==modulePointerRvas.end()) continue;
            for(uintptr_t globalRva:it->second){
                auto key=std::make_tuple(globalRva,owner,off);
                if(!seen.insert(key).second) continue;
                IndirectPathCandidate c{};
                c.globalRva=globalRva;c.owner=owner;c.ownerOffset=off;
                c.score=100;
                if(off<=0x80)c.score+=50; else if(off<=0x200)c.score+=25;
                if(RemoteRangeReadable(owner,0x100))c.score+=10;
                out.indirectCandidates.push_back(c);
            }
        }
    }
    std::sort(out.indirectCandidates.begin(),out.indirectCandidates.end(),[](const auto&a,const auto&b){
        if(a.score!=b.score)return a.score>b.score;
        if(a.ownerOffset!=b.ownerOffset)return a.ownerOffset<b.ownerOffset;
        return a.globalRva<b.globalRva;
    });
    if(!out.indirectCandidates.empty()){
        const auto&b=out.indirectCandidates.front();
        out.indirectGlobalRva=b.globalRva;out.indirectOwner=b.owner;out.ownerOffset=b.ownerOffset;
    }
    return out;
}

std::vector<uintptr_t> SnapshotQwords(uintptr_t base, uintptr_t bytes=0x120){
    std::vector<uintptr_t> v(bytes/8,0);
    for(uintptr_t off=0;off+8<=bytes;off+=8) ReadT(base+off,v[off/8]);
    return v;
}

bool PlausibleObjectPtr(uintptr_t p){
    if(!RemoteRangeReadable(p,sizeof(uintptr_t)))return false;
    uintptr_t vt=ReadPtr(p);
    auto sections=GetSections();
    return PlausibleVtable(vt,sections);
}

uintptr_t DiscoverZeroObjectZeroOffset(uintptr_t gameBase,const std::string&label,const std::set<uintptr_t>&exclude){
    std::cout << "\n============================================================\n"
              << "          EXTERNAL "<<label<<" DISCOVERY\n"
              << "============================================================\n";
    WaitEnter("PASSO A - deixe SEM alvo/follow e pressione ENTER...");
    auto a=SnapshotQwords(gameBase);
    if(label=="ATTACKTARGET") WaitEnter("PASSO B - selecione/ataque UM monstro e pressione ENTER...");
    else WaitEnter("PASSO B - use FOLLOW em uma criatura sem atacar e pressione ENTER...");
    auto b=SnapshotQwords(gameBase);
    WaitEnter("PASSO C - cancele e volte a ficar SEM alvo/follow; pressione ENTER...");
    auto c=SnapshotQwords(gameBase);
    struct C{uintptr_t off=0,ptr=0;int score=0;};std::vector<C> hits;
    for(size_t i=0;i<std::min({a.size(),b.size(),c.size()});++i){
        uintptr_t off=i*8;
        if(exclude.count(off))continue;
        if(a[i]!=0||b[i]==0||c[i]!=0)continue;
        if(!PlausibleObjectPtr(b[i]))continue;
        int sc=100;if(off<=0x40)sc+=50;else if(off<=0x80)sc+=25;
        hits.push_back({off,b[i],sc});
    }
    std::sort(hits.begin(),hits.end(),[](const C&a,const C&b){return a.score>b.score;});
    for(size_t i=0;i<hits.size()&&i<12;++i)
        std::cout<<"  #"<<i<<" off=+"<<Hex(hits[i].off)<<" ptr="<<Hex(hits[i].ptr)<<" score="<<hits[i].score<<"\n";
    if(hits.size()==1 || (!hits.empty() && (hits.size()==1 || hits[0].score>hits[1].score)))return hits[0].off;
    if(!hits.empty()){
        std::cout<<"[AMBIGUOUS] Escolha indice [ENTER=0]: ";std::string s;std::getline(std::cin,s);size_t idx=0;
        if(!s.empty()){try{idx=static_cast<size_t>(std::stoul(s));}catch(...){idx=0;}}
        if(idx<hits.size())return hits[idx].off;
    }
    return 0;
}

GameDiscovery DiscoverGameExternal(uintptr_t,const PointerPath&pp){
    GameDiscovery g{};
    if(pp.directGlobalRva){
        g.available=true;g.runtimeBase=g_moduleBase+pp.directGlobalRva;g.globalRva=pp.directGlobalRva;g.localPlayerOffset=0;g.source="module_direct_field";
    } else if(pp.indirectGlobalRva && pp.indirectOwner){
        g.available=true;g.runtimeBase=pp.indirectOwner;g.globalRva=pp.indirectGlobalRva;g.localPlayerOffset=pp.ownerOffset;g.source="module_pointer_to_owner";
    }
    if(!g.available){
        std::cout<<"[GAME] Nenhum owner externo forte encontrado; Attack/Follow ficam pending.\n";return g;
    }
    std::cout<<"\n[GAME OWNER] runtime="<<Hex(g.runtimeBase)<<" globalRva="<<Hex(g.globalRva)<<" lpOff=+"<<Hex(g.localPlayerOffset)<<" source="<<g.source<<"\n";
    std::set<uintptr_t> ex{g.localPlayerOffset};
    if(AskYesNo("Executar discovery EXTERNO de AttackTarget? [S/N]: ")){
        g.attackTargetOffset=DiscoverZeroObjectZeroOffset(g.runtimeBase,"ATTACKTARGET",ex);if(g.attackTargetOffset)ex.insert(g.attackTargetOffset);
    }
    if(AskYesNo("Executar discovery EXTERNO de FollowTarget? [S/N]: ")){
        g.followTargetOffset=DiscoverZeroObjectZeroOffset(g.runtimeBase,"FOLLOWTARGET",ex);
    }
    return g;
}

std::vector<uintptr_t> FindModulePositionTriples(uint32_t x,uint32_t y,uint8_t z){
    std::vector<uintptr_t> out;auto sections=GetSections();
    const size_t chunk=1u<<20;std::vector<uint8_t> buf(chunk+16);
    for(const auto&s:sections){
        if(!s.readable()||s.executable())continue;
        uintptr_t cur=s.begin;
        while(cur<s.end){
            size_t want=static_cast<size_t>(std::min<uintptr_t>(chunk,s.end-cur));SIZE_T got=0;
            if(!ReadProcessMemory(g_process,reinterpret_cast<LPCVOID>(cur),buf.data(),want,&got)||got<9){cur+=want;continue;}
            uintptr_t aligned=(cur+3)&~uintptr_t(3);size_t start=static_cast<size_t>(aligned-cur);
            for(size_t off=start;off+9<=got;off+=4){uint32_t rx=0,ry=0;std::memcpy(&rx,buf.data()+off,4);std::memcpy(&ry,buf.data()+off+4,4);uint8_t rz=buf[off+8];if(rx==x&&ry==y&&rz==z)out.push_back(cur+off);}
            cur+=static_cast<size_t>(got);
        }
    }
    return out;
}

MapCenterDiscovery DiscoverMapCenterExternal(uintptr_t localPlayer,const PosFields&pos){
    MapCenterDiscovery m{};if(!pos.x||!pos.y||!pos.z)return m;
    if(!AskYesNo("Executar discovery EXTERNO do Map Center? [S/N]: "))return m;
    auto readPos=[&](){return std::array<uint32_t,3>{ReadU32(localPlayer+pos.x),ReadU32(localPlayer+pos.y),ReadU8(localPlayer+pos.z)};};
    auto p0=readPos();auto cands=FindModulePositionTriples(p0[0],p0[1],static_cast<uint8_t>(p0[2]));
    std::cout<<"[MAP] triples iniciais no modulo: "<<cands.size()<<"\n";
    WaitEnter("Mova para outro SQM no MESMO andar e pressione ENTER...");auto p1=readPos();
    cands.erase(std::remove_if(cands.begin(),cands.end(),[&](uintptr_t a){return ReadU32(a)!=p1[0]||ReadU32(a+4)!=p1[1]||ReadU8(a+8)!=p1[2];}),cands.end());
    std::cout<<"[MAP] candidatos apos movimento 1: "<<cands.size()<<"\n";
    if(cands.size()!=1){WaitEnter("Mova novamente para OUTRO SQM no mesmo andar e pressione ENTER...");auto p2=readPos();
        cands.erase(std::remove_if(cands.begin(),cands.end(),[&](uintptr_t a){return ReadU32(a)!=p2[0]||ReadU32(a+4)!=p2[1]||ReadU8(a+8)!=p2[2];}),cands.end());}
    if(cands.size()==1){m.confirmed=true;m.centerRva=cands[0]-g_moduleBase;if(m.centerRva>=0x340)m.candidateMapRva=m.centerRva-0x340;
        std::cout<<"[MAP][PASS] centerRva="<<Hex(m.centerRva)<<" candidateMapRva="<<Hex(m.candidateMapRva)<<" (offset +0x340 family hint)\n";}
    else std::cout<<"[MAP][PENDING] candidatos finais="<<cands.size()<<"\n";
    return m;
}

bool ReadWholeText(std::vector<uint8_t>&out){
    if(g_textEnd<=g_textBegin)return false;
    size_t sz=static_cast<size_t>(g_textEnd-g_textBegin);out.resize(sz);
    size_t done=0;const size_t chunk=4u*1024u*1024u;
    while(done<sz){size_t want=std::min(chunk,sz-done);SIZE_T got=0;if(!ReadProcessMemory(g_process,reinterpret_cast<LPCVOID>(g_textBegin+done),out.data()+done,want,&got)||got==0)return false;done+=got;if(got<want)return false;}
    return true;
}

std::string ReadFileText(const std::string&path){std::ifstream f(std::filesystem::u8path(path),std::ios::binary);if(!f)return{};return std::string((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());}
std::vector<std::string> LoadReferenceProfiles() {
    auto root=std::filesystem::u8path(ExeDirectory());auto dir=root/"profiles"/"reference";
    if(!std::filesystem::exists(dir))dir=root/".."/"profiles"/"reference";
    std::vector<std::string> refs;
    if(std::filesystem::exists(dir))for(const auto& file:std::filesystem::directory_iterator(dir)) {
        if(file.is_regular_file() && file.path().extension()==".json") {auto raw=ReadFileText(file.path().u8string());if(!raw.empty())refs.push_back(std::move(raw));}
    }
    return refs;
}
std::string ExtractJsonString(const std::string&s,const std::string&key){
    std::string needle="\""+key+"\"";size_t p=s.find(needle);if(p==std::string::npos)return{};p=s.find(':',p+needle.size());if(p==std::string::npos)return{};p=s.find('"',p+1);if(p==std::string::npos)return{};++p;std::string out;bool esc=false;
    for(;p<s.size();++p){char c=s[p];if(esc){out+=c;esc=false;continue;}if(c=='\\'){esc=true;continue;}if(c=='"')break;out+=c;}return out;
}

struct PatternByte{uint8_t v=0;bool wild=false;};
std::vector<PatternByte> ParsePattern(const std::string&s){std::vector<PatternByte>p;std::istringstream is(s);std::string t;while(is>>t){PatternByte b{};if(t=="??"||t=="?")b.wild=true;else{try{b.v=static_cast<uint8_t>(std::stoul(t,nullptr,16));}catch(...){return{};}}p.push_back(b);}return p;}
std::vector<uintptr_t> ScanPattern(const std::vector<uint8_t>&text,const std::vector<PatternByte>&p){std::vector<uintptr_t>r;if(p.empty()||p.size()>text.size())return r;for(size_t i=0;i+p.size()<=text.size();++i){bool ok=true;for(size_t j=0;j<p.size();++j)if(!p[j].wild&&text[i+j]!=p[j].v){ok=false;break;}if(ok)r.push_back(static_cast<uintptr_t>(i));}return r;}

std::vector<FunctionResolution> ResolveFunctionsExternal(){
    std::vector<FunctionResolution> specs={
        {"Game::autoWalk","sigGameAutoWalk"},{"LocalPlayer::autoWalk","sigLocalPlayerAutoWalk"},{"Game::attack","sigGameAttack"},{"Game::follow","sigGameFollow"},{"Game::look","sigGameLook"},{"Game::detailedLook","sigGameDetailedLook"},{"Game::move","sigGameMove"},{"Game::use","sigGameUse"},{"Game::useWith","sigGameUseWith"},{"Game::talk","sigGameTalk"},{"Map::getTile","sigMapGetTile"},{"Map::getCreatureById","sigMapGetCreatureById"},{"Map::getCreatureByName","sigMapGetCreatureByName"},{"Container::getItem","sigContainerGetItem"}
    };
    std::vector<uint8_t> text;if(!ReadWholeText(text)){std::cout<<"[FUNCTIONS] Falha lendo .text remoto.\n";return specs;}
    auto refs=LoadReferenceProfiles();
    std::cout<<"\n============================================================\n          EXTERNAL STATIC FUNCTION SIGNATURES\n============================================================\n";
    std::cout<<"Reference profiles loaded: "<<refs.size()<<"\n";
    for(auto&sp:specs){std::set<uintptr_t> uniqueRvas;int totalMatches=0;
        for(const auto&raw:refs){std::string sig=ExtractJsonString(raw,sp.signatureKey);if(sig.empty())continue;auto pat=ParsePattern(sig);if(pat.size()<12)continue;auto ms=ScanPattern(text,pat);totalMatches+=static_cast<int>(ms.size());for(auto off:ms)uniqueRvas.insert(static_cast<uintptr_t>((g_textBegin-g_moduleBase)+off));}
        sp.matches=static_cast<int>(uniqueRvas.size());if(uniqueRvas.size()==1){sp.rva=*uniqueRvas.begin();sp.status="unique_reference_signature";}else if(uniqueRvas.size()>1)sp.status="ambiguous_reference_signature";
        std::cout<<std::left<<std::setw(24)<<sp.name<<" : "<<(sp.rva?Hex(sp.rva):"pending")<<" ["<<sp.status<<"] candidates="<<sp.matches<<" rawMatches="<<totalMatches<<"\n";
    }
    return specs;
}

std::vector<LightCandidate> CalibrateLightReadOnly(uintptr_t base) {
    std::vector<LightCandidate> out;
    if (!AskYesNo("Executar calibracao EXTERNA READ-ONLY de Light? [S/N]: ")) return out;
    std::cout << "\n[LIGHT] Deixe SEM TOCHA e SEM magia de luz.\n";
    WaitEnter("Pressione ENTER para capturar BASE...");
    auto a=SnapshotU8(base);
    WaitEnter("Ligue/equipe uma TOCHA e pressione ENTER...");
    auto b=SnapshotU8(base);
    WaitEnter("Apague/remova a TOCHA e pressione ENTER...");
    auto c=SnapshotU8(base);
    for(size_t i=0;i<a.size();++i){
        if(a[i]==c[i] && b[i]!=a[i] && a[i]<=31 && b[i]<=31){
            LightCandidate lc{};lc.offset=i;lc.base=a[i];lc.on=b[i];lc.back=c[i];lc.score=100;
            if(i<0x400) lc.score+=5;
            if(b[i]>a[i]) lc.score+=5;
            out.push_back(lc);
        }
    }
    std::cout << "[LIGHT] candidatos BASE->TORCH->BASE: " << out.size() << "\n";
    for(size_t i=0;i<out.size()&&i<20;++i)
        std::cout << "  #"<<i<<" +"<<Hex(out[i].offset)<<" "<<unsigned(out[i].base)<<" -> "<<unsigned(out[i].on)<<" -> "<<unsigned(out[i].back)<<"\n";

    if (AskYesNo("Consegue usar utevo lux agora? [S/N]: ")) {
        WaitEnter("Deixe a luz BASE/normal e pressione ENTER...");
        auto l0=SnapshotU8(base);
        WaitEnter("Use utevo lux e pressione ENTER enquanto a luz estiver ativa...");
        auto l1=SnapshotU8(base);
        WaitEnter("Quando a luz terminar/voltar ao normal, pressione ENTER...");
        auto l2=SnapshotU8(base);
        for(auto&lc:out){
            size_t i=static_cast<size_t>(lc.offset);
            if(i<l0.size() && l0[i]==l2[i] && l1[i]!=l0[i] && l0[i]<=31 && l1[i]<=31){lc.alsoLux=true;lc.lux=l1[i];lc.score+=100;}
        }
        for(size_t i=0;i<l0.size();++i){
            if(l0[i]==l2[i] && l1[i]!=l0[i] && l0[i]<=31 && l1[i]<=31){
                auto it=std::find_if(out.begin(),out.end(),[&](const LightCandidate&x){return x.offset==i;});
                if(it==out.end()){LightCandidate lc{};lc.offset=i;lc.base=l0[i];lc.on=l1[i];lc.back=l2[i];lc.alsoLux=true;lc.lux=l1[i];lc.score=150;out.push_back(lc);}
            }
        }
    }
    std::sort(out.begin(),out.end(),[](const LightCandidate&a,const LightCandidate&b){return a.score>b.score;});
    if(!out.empty()){
        std::cout << "[LIGHT BEST] +"<<Hex(out[0].offset)<<" base="<<unsigned(out[0].base)<<" torch/lux="<<unsigned(out[0].on)
                  <<" alsoLux="<<(out[0].alsoLux?"YES":"NO")<<"\n";
    }
    return out;
}

std::string JsonEscape(const std::string&s){std::string o;for(char c:s){if(c=='\\'||c=='\"')o+='\\';o+=c;}return o;}

void WriteOffsetsJson(std::ofstream& f,const char*name,const std::vector<uintptr_t>&v,bool comma=true){
    f << "      \""<<name<<"\": {\"status\": \""<<(v.size()==1?"strong_candidate":(v.empty()?"pending":"ambiguous"))<<"\", \"offsets\": [";
    for(size_t i=0;i<v.size();++i){if(i)f<<", ";f<<"\""<<Hex(v[i])<<"\"";}
    f << "]}" << (comma?",":"") << "\n";
}

std::string SaveProfile(const BuildInfo& b,const LocalCandidate& lc,const PosFields& pos,const PointerPath& path,const GameDiscovery& game,const MapCenterDiscovery& mapCenter,const std::vector<FunctionResolution>&functions,const std::vector<LightCandidate>&lights){
    std::string dir=ExeDirectory()+"\\Build";
    std::filesystem::create_directories(std::filesystem::u8path(dir));
    std::string file=dir+"\\OTCLab_EXTERNAL_Profile_"+Hex(b.timestamp).substr(2)+"_"+Hex(b.textHash).substr(2)+".json";
    const auto target=std::filesystem::u8path(file), temp=std::filesystem::u8path(file+".tmp");
    std::ofstream f(temp,std::ios::binary);
    if(!f)throw std::runtime_error("Nao consegui abrir o arquivo de perfil para gravacao.");
    f << "{\n";
    f << "  \"schema\": \"otclab-external-v27-port-v1\",\n";
    f << "  \"mode\": \"external_read_only\",\n";
    f << "  \"pid\": "<<g_pid<<",\n";
    f << "  \"process\": \""<<JsonEscape(g_processPath)<<"\",\n";
    f << "  \"build\": {\"timestamp\": \""<<Hex(b.timestamp)<<"\", \"sizeOfImage\": \""<<Hex(b.sizeOfImage)<<"\", \"entryPointRva\": \""<<Hex(b.entryPointRva)<<"\", \"textRva\": \""<<Hex(b.textRva)<<"\", \"textSize\": \""<<Hex(b.textSize)<<"\", \"textHashFNV1A64\": \""<<Hex(b.textHash)<<"\"},\n";
    f << "  \"localPlayer\": {\n";
    f << "    \"runtimeAddressDiagnostic\": \""<<Hex(lc.base)<<"\",\n";
    f << "    \"vtableRva\": \""<<Hex(lc.vtable-g_moduleBase)<<"\",\n";
    f << "    \"directGlobalRva\": \""<<Hex(path.directGlobalRva)<<"\",\n";
    f << "    \"indirectGlobalRva\": \""<<Hex(path.indirectGlobalRva)<<"\",\n";
    f << "    \"indirectOwnerOffset\": \""<<Hex(path.ownerOffset)<<"\",\n";
    f << "    \"position\": {\"x\": \""<<Hex(pos.x)<<"\", \"y\": \""<<Hex(pos.y)<<"\", \"z\": \""<<Hex(pos.z)<<"\", \"direction\": \""<<Hex(pos.direction)<<"\"},\n";
    f << "    \"fields\": {\n";
    const char* names[]={"health","maxHealth","mana","maxMana","experience","level","capacity"};
    for(size_t i=0;i<7;++i){auto it=lc.fieldOffsets.find(names[i]);std::vector<uintptr_t> empty;WriteOffsetsJson(f,names[i],it==lc.fieldOffsets.end()?empty:it->second,i!=6);}
    f << "    }\n  },\n";
    f << "  \"guidedFields\": {\n";
    size_t written=0;
    for(const auto& kv:g_guided) {
        const auto& e=kv.second;
        f<<"    \""<<kv.first<<"\": {\"status\": \""<<e.status()<<"\", \"reason\": \""<<e.reason<<"\", \"transitions\": "<<e.transitions<<", \"observations\": [";
        for(size_t i=0;i<e.observations.size();++i){if(i)f<<", ";f<<std::setprecision(17)<<e.observations[i];}
        f<<"], \"candidates\": [";
        for(size_t i=0;i<e.candidates.size();++i){if(i)f<<", ";f<<"{\"offset\": \""<<Hex(e.candidates[i].offset)<<"\", \"type\": \""<<e.candidates[i].type<<"\"}";}
        f<<"]}"<<(++written<g_guided.size()?",":"")<<"\n";
    }
    f << "  },\n";
    f << "  \"game\": {\"status\": \""<<(game.available?"external_owner_resolved":"pending")<<"\", \"globalRva\": \""<<Hex(game.globalRva)<<"\", \"localPlayerOffset\": \""<<Hex(game.localPlayerOffset)<<"\", \"attackTargetOffset\": \""<<Hex(game.attackTargetOffset)<<"\", \"followTargetOffset\": \""<<Hex(game.followTargetOffset)<<"\", \"source\": \""<<game.source<<"\"},\n";
    f << "  \"map\": {\"status\": \""<<(mapCenter.confirmed?"confirmed_external_center_transition":"pending")<<"\", \"centerRva\": \""<<Hex(mapCenter.centerRva)<<"\", \"candidateMapRva\": \""<<Hex(mapCenter.candidateMapRva)<<"\"},\n";
    f << "  \"functions\": {\n";
    for(size_t i=0;i<functions.size();++i){const auto&fn=functions[i];f<<"    \""<<JsonEscape(fn.name)<<"\": {\"rva\": \""<<Hex(fn.rva)<<"\", \"status\": \""<<fn.status<<"\", \"candidates\": "<<fn.matches<<"}"<<(i+1<functions.size()?",":"")<<"\n";}
    f << "  },\n";
    f << "  \"light\": {\"status\": \""<<(lights.empty()?"pending":"confirmed_read_transition")<<"\", \"writeValidated\": false";
    if(!lights.empty()) f << ", \"bestOffset\": \""<<Hex(lights[0].offset)<<"\", \"baseline\": "<<unsigned(lights[0].base)<<", \"active\": "<<unsigned(lights[0].on)<<", \"alsoLux\": "<<(lights[0].alsoLux?"true":"false");
    f << "},\n";
    uintptr_t lookRva=0;std::string lookFn="pending";for(const auto&fn:functions){if(fn.name=="Game::detailedLook"&&fn.rva){lookRva=fn.rva;lookFn="gameDetailedLook";break;}}if(!lookRva){for(const auto&fn:functions){if(fn.name=="Game::look"&&fn.rva){lookRva=fn.rva;lookFn="gameLook";break;}}}
    f << "  \"look\": {\"status\": \""<<(lookRva?"candidate_static_external":"pending")<<"\", \"hookFunction\": \""<<lookFn<<"\", \"hookRva\": \""<<Hex(lookRva)<<"\", \"runtimeAbi\": \"pending_external_read_only\", \"referenceItemDefaults\": [3031,3552], \"note\": \"No injection/hook: ABI remains pending; static candidate comes only from remote .text signature evidence.\"}\n";
    f << "}\n";
    f.flush();if(!f)throw std::runtime_error("Falha ao gravar perfil; arquivo anterior preservado.");f.close();
    if(!MoveFileExW(temp.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Falha ao finalizar perfil; arquivo anterior preservado.");
    return file;
}

void RuntimeWatch(uintptr_t base,const LocalCandidate&lc,const PosFields&pos){
    if(!AskYesNo("Abrir Runtime Watch externo por 20 segundos? [S/N]: ")) return;
    auto one=[&](const char*n)->uintptr_t{auto it=lc.fieldOffsets.find(n);return(it!=lc.fieldOffsets.end()&&it->second.size()==1)?it->second[0]:0;};
    uintptr_t hp=one("health"), mhp=one("maxHealth"), mana=one("mana"), mmana=one("maxMana");
    for(int i=0;i<40;++i){
        std::cout << "\r[WATCH] ";
        if(pos.x&&pos.y){std::cout<<ReadU32(base+pos.x)<<","<<ReadU32(base+pos.y);if(pos.z)std::cout<<","<<unsigned(ReadU8(base+pos.z));std::cout<<" ";}
        double v=0;
        if(hp&&ReadT(base+hp,v))std::cout<<"HP="<<std::fixed<<std::setprecision(0)<<v;
        if(mhp&&ReadT(base+mhp,v))std::cout<<"/"<<v<<" ";
        if(mana&&ReadT(base+mana,v))std::cout<<"MP="<<v;
        if(mmana&&ReadT(base+mmana,v))std::cout<<"/"<<v;
        std::cout<<"      "<<std::flush;
        Sleep(500);
    }
    std::cout<<"\n";
}

} // namespace

int main() try {
    SetConsoleTitleA("OTCLab External Builder V2.8 - Descoberta Guiada");
    std::cout << "============================================================\n"
              << "        OTCLab External Builder V2.8 - GUIADO\n"
              << "============================================================\n"
              << "PORT EXTERNO DA V2.7 / SEM DLL / SEM INJECAO / SEM HOOK / SEM WRITE\n"
              << "Usa apenas OpenProcess + ReadProcessMemory.\n"
              << "Se o cliente bloquear leitura externa, o programa para sem bypass.\n"
              << "============================================================\n";

    PrintProcesses();
    DWORD pid=AskPid();
    if(!OpenTarget(pid)){WaitEnter("ENTER para sair...");return 1;}

    BuildInfo build{};
    if(!ResolveBuild(build)){std::cout<<"[FAIL] Nao consegui ler PE x64 do alvo.\n";WaitEnter("ENTER para sair...");return 2;}

    std::cout << "\n[CONNECTED READ-ONLY]\n"
              << "PID        : "<<g_pid<<"\n"
              << "Executable : "<<g_processPath<<"\n"
              << "ModuleBase : "<<Hex(g_moduleBase)<<"\n"
              << "Timestamp  : "<<Hex(build.timestamp)<<"\n"
              << "SizeImage  : "<<Hex(build.sizeOfImage)<<"\n"
              << "TextHash   : "<<Hex(build.textHash)<<"\n";

    StatsInput in{};
    std::cout << "\nDigite os valores atuais mostrados no personagem.\n";
    in.health=AskDouble("Health atual     : ");
    in.maxHealth=AskDouble("Health maximo    : ");
    in.mana=AskDouble("Mana atual        : ");
    in.maxMana=AskDouble("Mana maxima       : ");
    in.experience=AskDouble("Experience        : ");
    in.level=AskDouble("Level             : ");
    in.capacity=AskDouble("Capacity          : ");

    if(SameDouble(in.health,in.maxHealth) && AskYesNo("Health == MaxHealth. Consegue tomar um pouco de dano antes do scan? [S/N]: ")){
        WaitEnter("Tome dano agora e pressione ENTER...");
        in.health=AskDouble("Health novo       : ");
    }

    auto candidates=DiscoverCandidates(in);
    if(candidates.empty()){
        std::cout<<"[STOP] Nenhum LocalPlayer forte encontrado por leitura externa.\n";
        WaitEnter("ENTER para sair...");
        return 3;
    }
    std::cout << "\n[LOCALPLAYER CANDIDATES]\n";
    for(size_t i=0;i<candidates.size();++i) PrintCandidate(candidates[i],i);
    size_t idx=AskCandidateIndex(candidates.size());
    LocalCandidate chosen=candidates[idx];
    std::cout<<"[SELECTED] LocalPlayer="<<Hex(chosen.base)<<" VTableRVA="<<Hex(chosen.vtable-g_moduleBase)<<"\n";

    PointerPath pp=FindPointerPaths(chosen.base);
    g_guidedDirect=pp.directGlobalRva;g_guidedIndirect=pp.indirectGlobalRva;g_guidedOwnerOffset=pp.ownerOffset;
    GuideStats(chosen);

    PosFields pos=CalibratePosition(chosen.base);
    std::cout << "\n[POINTER PATH]\nDirect refs no modulo: "<<pp.allDirectRvas.size()<<"\n";
    for(auto r:pp.allDirectRvas)std::cout<<"  DIRECT RVA "<<Hex(r)<<"\n";
    if(pp.indirectGlobalRva)std::cout<<"Selected INDIRECT globalRva="<<Hex(pp.indirectGlobalRva)<<" owner="<<Hex(pp.indirectOwner)<<" LPoff=+"<<Hex(pp.ownerOffset)<<"\n";
    if(!pp.indirectCandidates.empty()){std::cout<<"Indirect candidates: "<<pp.indirectCandidates.size()<<"\n";for(size_t i=0;i<pp.indirectCandidates.size()&&i<10;++i){const auto&c=pp.indirectCandidates[i];std::cout<<"  #"<<i<<" global="<<Hex(c.globalRva)<<" owner="<<Hex(c.owner)<<" off=+"<<Hex(c.ownerOffset)<<" score="<<c.score<<"\n";}}

    GameDiscovery game=DiscoverGameExternal(chosen.base,pp);
    MapCenterDiscovery mapCenter=DiscoverMapCenterExternal(chosen.base,pos);
    auto functions=ResolveFunctionsExternal();
    auto lights=CalibrateLightReadOnly(chosen.base);
    PrintGuidedSummary();
    std::string saved=SaveProfile(build,chosen,pos,pp,game,mapCenter,functions,lights);
    std::cout << "\n[SAVE] "<<saved<<"\n";

    RuntimeWatch(chosen.base,chosen,pos);

    std::cout << "\n============================================================\n"
              << "EXTERNAL TEST FINISHED\n"
              << "============================================================\n"
              << "Look: candidato estatico pode ser descoberto; ABI runtime continua pending sem hook/injecao.\n"
              << "Nenhuma memoria do cliente foi escrita. Nenhum hook foi instalado.\n";

    if(g_process) CloseHandle(g_process);
    WaitEnter("ENTER para sair...");
    return 0;
} catch(const std::exception& e) {
    std::cout<<"\n[INTERROMPIDO] "<<e.what()<<"\n";
    if(g_process)CloseHandle(g_process);
    WaitEnter("ENTER para sair...");return 4;
}
