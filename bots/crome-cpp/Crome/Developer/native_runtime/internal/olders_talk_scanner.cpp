#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <set>
#include <string>
#include <tuple>
#include <vector>

// ============================================================
// CROME SDK V17.1 - FAST TALK / SAY XREF SCANNER
//
// READ-ONLY. No game function is called. No memory is written.
//
// Goal:
//   OTClient/Rifbot model:
//     Game::talk(message)
//       -> Game::talkChannel(MessageSay=1, channel=0, message)
//       -> ProtocolGame::sendTalk(...)
//       -> ClientTalk opcode 0x96
//
// We already know these Olders anchors:
//   Game::attack              0x165410
//   Game::follow              0x1656E0
//   ProtocolGame move helper  0x271E90 (opcode 0x78)
//   ProtocolGame use helper   0x272840 (opcode 0x82)
//   ProtocolGame use creature 0x272C10 (opcode 0x84)
//
// The scanner searches for the same immediate-opcode pattern using 0x96,
// then walks direct CALL xrefs back toward the Game action cluster.
// ============================================================

constexpr uintptr_t RVA_GAME_ATTACK = 0x00165410;
constexpr uintptr_t RVA_GAME_FOLLOW = 0x001656E0;
constexpr uintptr_t RVA_PROTOCOL_MOVE = 0x00271E90;
constexpr uintptr_t RVA_PROTOCOL_USE = 0x00272840;
constexpr uintptr_t RVA_PROTOCOL_USE_CREATURE = 0x00272C10;

constexpr uintptr_t GAME_SCAN_BEGIN = 0x00165000;
constexpr uintptr_t GAME_SCAN_END   = 0x00168000;
constexpr uintptr_t PROTO_SCAN_BEGIN = 0x00270000;
constexpr uintptr_t PROTO_SCAN_END   = 0x00278000;

HMODULE g_dll = nullptr;
uintptr_t g_base = 0;
uintptr_t g_imageBegin = 0;
uintptr_t g_imageEnd = 0;
uintptr_t g_textBegin = 0;
uintptr_t g_textEnd = 0;
uintptr_t g_exceptionBegin = 0;
uintptr_t g_exceptionEnd = 0;
DWORD g_exceptionRva = 0;
DWORD g_exceptionSize = 0;
volatile bool g_running = true;

#pragma pack(push, 1)
struct RuntimeFunctionEntry {
    DWORD BeginAddress;
    DWORD EndAddress;
    DWORD UnwindInfoAddress;
};
#pragma pack(pop)

struct FunctionInfo {
    bool found = false;
    uint32_t beginRva = 0;
    uint32_t endRva = 0;
    uint32_t unwindRva = 0;
    size_t tableIndex = 0;
};

struct DirectCall {
    uintptr_t instruction = 0;
    uintptr_t target = 0;
};

struct DirectRef {
    uintptr_t instruction = 0;
    uintptr_t target = 0;
    bool isCall = true;
};

struct RipDecoded {
    bool valid = false;
    uintptr_t instruction = 0;
    uintptr_t target = 0;
    int length = 0;
    uint8_t opcode = 0;
};

struct OpcodeSite {
    uintptr_t address = 0;
    std::string kind;
    int strength = 0;
};

struct TalkCandidate {
    FunctionInfo function;
    std::vector<OpcodeSite> opcodeSites;
    std::vector<DirectRef> refs;
    int score = 0;
};

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

template<typename T>
bool SafeRead(uintptr_t address, T& out) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || !IsReadableProtection(mbi.Protect)) return false;
    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (address + sizeof(T) > regionEnd) return false;
    std::memcpy(&out, reinterpret_cast<void*>(address), sizeof(T));
    return true;
}

bool IsReadableRange(uintptr_t address, size_t size) {
    if (!address || !size) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || !IsReadableProtection(mbi.Protect)) return false;
    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return address + size <= regionEnd;
}

void Hex() { std::cout << std::right << std::hex << std::uppercase << std::setfill('0'); }
void Dec() { std::cout << std::right << std::dec << std::setfill(' '); }
void Section(const char* title) {
    std::cout << "\n============================================================\n"
              << title
              << "\n============================================================\n";
}

void PrintRva(uintptr_t rva) {
    Hex(); std::cout << "0x" << std::setw(8) << rva; Dec();
}

bool ResolvePE() {
    if (!IsReadableRange(g_base, sizeof(IMAGE_DOS_HEADER))) return false;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const uintptr_t ntAddress = g_base + static_cast<uintptr_t>(dos->e_lfanew);
    if (!IsReadableRange(ntAddress, sizeof(IMAGE_NT_HEADERS64))) return false;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(ntAddress);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;

    g_imageBegin = g_base;
    g_imageEnd = g_base + nt->OptionalHeader.SizeOfImage;
    auto section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char name[9]{};
        std::memcpy(name, section[i].Name, 8);
        if (std::strcmp(name, ".text") == 0) {
            g_textBegin = g_base + section[i].VirtualAddress;
            const size_t size = std::max<size_t>(section[i].Misc.VirtualSize, section[i].SizeOfRawData);
            g_textEnd = g_textBegin + size;
        }
    }

    const auto& exceptionDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    g_exceptionRva = exceptionDir.VirtualAddress;
    g_exceptionSize = exceptionDir.Size;
    if (g_exceptionRva && g_exceptionSize) {
        g_exceptionBegin = g_base + g_exceptionRva;
        g_exceptionEnd = g_exceptionBegin + g_exceptionSize;
    }
    return g_textBegin && g_textEnd > g_textBegin && g_exceptionBegin && g_exceptionEnd > g_exceptionBegin;
}

size_t RuntimeFunctionCount() {
    return g_exceptionSize / sizeof(RuntimeFunctionEntry);
}

bool GetRuntimeFunction(size_t index, RuntimeFunctionEntry& out) {
    if (index >= RuntimeFunctionCount()) return false;
    return SafeRead(g_exceptionBegin + index * sizeof(RuntimeFunctionEntry), out);
}

FunctionInfo FindRuntimeFunction(uintptr_t rva) {
    FunctionInfo result{};
    size_t lo = 0, hi = RuntimeFunctionCount();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        RuntimeFunctionEntry entry{};
        if (!GetRuntimeFunction(mid, entry)) break;
        if (rva < entry.BeginAddress) hi = mid;
        else if (rva >= entry.EndAddress) lo = mid + 1;
        else {
            result.found = true;
            result.beginRva = entry.BeginAddress;
            result.endRva = entry.EndAddress;
            result.unwindRva = entry.UnwindInfoAddress;
            result.tableIndex = mid;
            return result;
        }
    }
    return result;
}

void PrintFunction(const FunctionInfo& f) {
    if (!f.found) { std::cout << "no-pdata"; return; }
    PrintRva(f.beginRva); std::cout << ".."; PrintRva(f.endRva);
    std::cout << " size=" << (f.endRva - f.beginRva);
}

bool DecodeDirectCall(uintptr_t instruction, DirectCall& out) {
    uint8_t opcode = 0;
    if (!SafeRead(instruction, opcode) || opcode != 0xE8) return false;
    int32_t rel = 0;
    if (!SafeRead(instruction + 1, rel)) return false;
    out.instruction = instruction;
    out.target = static_cast<uintptr_t>(static_cast<int64_t>(instruction + 5) + static_cast<int64_t>(rel));
    return true;
}

std::vector<uintptr_t> FindDirectCallers(uintptr_t target) {
    std::vector<uintptr_t> result;
    if (!g_textBegin || !g_textEnd) return result;
    const auto* text = reinterpret_cast<const uint8_t*>(g_textBegin);
    const size_t size = static_cast<size_t>(g_textEnd - g_textBegin);
    for (size_t i = 0; i + 5 <= size; ++i) {
        if (text[i] != 0xE8) continue;
        int32_t rel = 0;
        std::memcpy(&rel, text + i + 1, sizeof(rel));
        const uintptr_t instruction = g_textBegin + i;
        const uintptr_t resolved = static_cast<uintptr_t>(static_cast<int64_t>(instruction + 5) + rel);
        if (resolved == target) result.push_back(instruction);
    }
    return result;
}

std::vector<DirectRef> FindDirectRefs(uintptr_t target) {
    std::vector<DirectRef> result;
    if (!g_textBegin || !g_textEnd) return result;
    const auto* text = reinterpret_cast<const uint8_t*>(g_textBegin);
    const size_t size = static_cast<size_t>(g_textEnd - g_textBegin);
    for (size_t i = 0; i + 5 <= size; ++i) {
        if (text[i] != 0xE8 && text[i] != 0xE9) continue;
        int32_t rel = 0;
        std::memcpy(&rel, text + i + 1, sizeof(rel));
        const uintptr_t instruction = g_textBegin + i;
        const uintptr_t resolved = static_cast<uintptr_t>(static_cast<int64_t>(instruction + 5) + rel);
        if (resolved == target) result.push_back(DirectRef{instruction, resolved, text[i] == 0xE8});
    }
    return result;
}

RipDecoded DecodeRip(uintptr_t address) {
    RipDecoded out{};
    uint8_t bytes[8]{};
    if (!IsReadableRange(address, sizeof(bytes))) return out;
    std::memcpy(bytes, reinterpret_cast<void*>(address), sizeof(bytes));
    size_t i = 0;
    if ((bytes[i] & 0xF0) == 0x40) ++i;
    if (bytes[i] != 0x8D && bytes[i] != 0x8B) return out;
    const uint8_t opcode = bytes[i++];
    const uint8_t modrm = bytes[i++];
    if ((modrm & 0xC7) != 0x05) return out;
    int32_t disp = 0;
    std::memcpy(&disp, bytes + i, sizeof(disp));
    i += 4;
    out.valid = true;
    out.instruction = address;
    out.target = static_cast<uintptr_t>(static_cast<int64_t>(address + i) + disp);
    out.length = static_cast<int>(i);
    out.opcode = opcode;
    return out;
}

std::vector<uintptr_t> FindRipRefs(uintptr_t target) {
    std::vector<uintptr_t> refs;
    if (!g_textBegin || !g_textEnd) return refs;

    // FAST path: .text is already resolved/readable. Do not VirtualQuery per byte.
    const auto* b = reinterpret_cast<const uint8_t*>(g_textBegin);
    const size_t n = static_cast<size_t>(g_textEnd - g_textBegin);

    for (size_t off = 0; off + 7 <= n; ++off) {
        size_t i = off;
        size_t len = 0;

        // optional REX prefix
        if ((b[i] & 0xF0) == 0x40) {
            ++i;
            ++len;
            if (i + 6 > n) break;
        }

        if (b[i] != 0x8D && b[i] != 0x8B) continue;
        ++i; ++len;

        const uint8_t modrm = b[i++];
        ++len;
        if ((modrm & 0xC7) != 0x05) continue;

        int32_t disp = 0;
        std::memcpy(&disp, b + i, sizeof(disp));
        len += 4;

        const uintptr_t instruction = g_textBegin + off;
        const uintptr_t resolved = static_cast<uintptr_t>(
            static_cast<int64_t>(instruction + len) + static_cast<int64_t>(disp));
        if (resolved == target) refs.push_back(instruction);
    }
    return refs;
}


std::vector<std::pair<uintptr_t, uintptr_t>> FindNearbyRipCodeRefsFast(uintptr_t center, size_t radius = 0x120) {
    std::vector<std::pair<uintptr_t, uintptr_t>> out;
    if (!g_textBegin || !g_textEnd) return out;
    const uintptr_t lo = std::max(g_textBegin, center > radius ? center - radius : g_textBegin);
    const uintptr_t hi = std::min(g_textEnd, center + radius);
    const auto* text = reinterpret_cast<const uint8_t*>(g_textBegin);

    for (uintptr_t addr = lo; addr + 7 <= hi; ++addr) {
        size_t off = static_cast<size_t>(addr - g_textBegin);
        size_t i = off;
        size_t len = 0;
        if ((text[i] & 0xF0) == 0x40) { ++i; ++len; }
        if (i + 6 > static_cast<size_t>(g_textEnd - g_textBegin)) break;
        if (text[i] != 0x8D && text[i] != 0x8B) continue;
        ++i; ++len;
        const uint8_t modrm = text[i++]; ++len;
        if ((modrm & 0xC7) != 0x05) continue;
        int32_t disp = 0;
        std::memcpy(&disp, text + i, sizeof(disp));
        len += 4;
        const uintptr_t target = static_cast<uintptr_t>(static_cast<int64_t>(addr + len) + disp);
        if (target >= g_textBegin && target < g_textEnd) out.emplace_back(addr, target);
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<uintptr_t> FindAsciiExact(const std::string& needle) {
    std::vector<uintptr_t> out;
    if (needle.empty()) return out;
    uintptr_t p = g_imageBegin;
    while (p < g_imageEnd) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) break;
        const uintptr_t regionBegin = std::max(p, reinterpret_cast<uintptr_t>(mbi.BaseAddress));
        const uintptr_t regionEnd = std::min(g_imageEnd, reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize);
        if (mbi.State == MEM_COMMIT && IsReadableProtection(mbi.Protect) && regionEnd > regionBegin) {
            const size_t n = static_cast<size_t>(regionEnd - regionBegin);
            const auto* b = reinterpret_cast<const uint8_t*>(regionBegin);
            for (size_t i = 0; i + needle.size() + 1 <= n; ++i) {
                if (std::memcmp(b + i, needle.data(), needle.size()) == 0 && b[i + needle.size()] == 0)
                    out.push_back(regionBegin + i);
            }
        }
        if (regionEnd <= p) break;
        p = regionEnd;
    }
    return out;
}

std::vector<DirectCall> CallsInside(const FunctionInfo& f) {
    std::vector<DirectCall> out;
    if (!f.found) return out;
    const uintptr_t begin = g_base + f.beginRva;
    const uintptr_t end = g_base + f.endRva;
    if (!IsReadableRange(begin, static_cast<size_t>(end - begin))) return out;
    for (uintptr_t p = begin; p + 5 <= end; ++p) {
        if (*reinterpret_cast<const uint8_t*>(p) != 0xE8) continue;
        DirectCall c{};
        if (DecodeDirectCall(p, c)) out.push_back(c);
    }
    return out;
}

void DumpHex(uintptr_t center, size_t before = 24, size_t after = 40) {
    uintptr_t begin = center > before ? center - before : g_imageBegin;
    uintptr_t end = std::min(center + after, g_imageEnd);
    begin = std::max(begin, g_imageBegin);
    for (uintptr_t row = begin & ~uintptr_t(0xF); row < end; row += 16) {
        if (!IsReadableRange(row, 16)) continue;
        Hex(); std::cout << "RVA 0x" << std::setw(8) << (row - g_base) << " : ";
        for (int i = 0; i < 16; ++i) {
            std::cout << std::setw(2) << static_cast<unsigned>(*reinterpret_cast<const uint8_t*>(row + i)) << ' ';
        }
        Dec(); std::cout << "\n";
    }
}

std::vector<OpcodeSite> FindOpcodeSites(const FunctionInfo& f, uint8_t opcode) {
    std::vector<OpcodeSite> sites;
    if (!f.found) return sites;
    const uintptr_t begin = g_base + f.beginRva;
    const uintptr_t end = g_base + f.endRva;
    if (!IsReadableRange(begin, static_cast<size_t>(end - begin))) return sites;
    const auto* b = reinterpret_cast<const uint8_t*>(begin);
    const size_t n = static_cast<size_t>(end - begin);

    auto add = [&](size_t off, const char* kind, int strength) {
        sites.push_back(OpcodeSite{begin + off, kind, strength});
    };

    for (size_t i = 0; i < n; ++i) {
        // mov edx, imm32 -- ideal for OutputMessage::addU8 second parameter
        if (i + 5 <= n && b[i] == 0xBA && b[i+1] == opcode && b[i+2] == 0 && b[i+3] == 0 && b[i+4] == 0)
            add(i + 1, "mov edx, imm32", 100);
        // mov ecx/eax/esi/etc, imm32
        if (i + 5 <= n && b[i] >= 0xB8 && b[i] <= 0xBF && b[i+1] == opcode && b[i+2] == 0 && b[i+3] == 0 && b[i+4] == 0)
            add(i + 1, "mov r32, imm32", 80);
        // REX + mov r8d/r9d/... imm32
        if (i + 6 <= n && (b[i] & 0xF0) == 0x40 && b[i+1] >= 0xB8 && b[i+1] <= 0xBF && b[i+2] == opcode && b[i+3] == 0 && b[i+4] == 0 && b[i+5] == 0)
            add(i + 2, "rex mov r32, imm32", 80);
        // mov dl/cl/al/etc, imm8
        if (i + 2 <= n && b[i] >= 0xB0 && b[i] <= 0xB7 && b[i+1] == opcode)
            add(i + 1, "mov r8, imm8", 70);
        // push imm32 (less likely on x64 but useful diagnostic)
        if (i + 5 <= n && b[i] == 0x68 && b[i+1] == opcode && b[i+2] == 0 && b[i+3] == 0 && b[i+4] == 0)
            add(i + 1, "push imm32", 50);
    }

    // Deduplicate identical immediate addresses caused by overlapping patterns.
    std::sort(sites.begin(), sites.end(), [](const auto& a, const auto& b){
        if (a.address != b.address) return a.address < b.address;
        return a.strength > b.strength;
    });
    std::vector<OpcodeSite> unique;
    for (const auto& s : sites) {
        auto it = std::find_if(unique.begin(), unique.end(), [&](const auto& q){ return q.address == s.address; });
        if (it == unique.end()) unique.push_back(s);
        else if (s.strength > it->strength) *it = s;
    }
    return unique;
}

void PrintKnownProtocolAnchor(const char* name, uintptr_t rva, uint8_t opcode) {
    const FunctionInfo f = FindRuntimeFunction(rva);
    std::cout << "\n" << name << " @ "; PrintRva(rva); std::cout << " owner="; PrintFunction(f); std::cout << "\n";
    const auto sites = FindOpcodeSites(f, opcode);
    std::cout << "  expected opcode 0x"; Hex(); std::cout << std::setw(2) << static_cast<unsigned>(opcode); Dec();
    std::cout << " instruction-pattern sites=" << sites.size() << "\n";
    for (size_t i = 0; i < sites.size() && i < 8; ++i) {
        std::cout << "    site #" << (i+1) << " "; PrintRva(sites[i].address - g_base);
        std::cout << " kind=" << sites[i].kind << "\n";
        DumpHex(sites[i].address, 16, 24);
    }
}

std::vector<TalkCandidate> FindProtocolTalkCandidates() {
    std::vector<TalkCandidate> out;
    const size_t count = RuntimeFunctionCount();
    for (size_t i = 0; i < count; ++i) {
        RuntimeFunctionEntry e{};
        if (!GetRuntimeFunction(i, e)) continue;
        if (e.EndAddress <= PROTO_SCAN_BEGIN || e.BeginAddress >= PROTO_SCAN_END) continue;
        if (e.EndAddress <= e.BeginAddress || e.EndAddress - e.BeginAddress > 0x3000) continue;

        FunctionInfo f{true, e.BeginAddress, e.EndAddress, e.UnwindInfoAddress, i};
        auto sites = FindOpcodeSites(f, 0x96);
        if (sites.empty()) continue;

        TalkCandidate c{};
        c.function = f;
        c.opcodeSites = std::move(sites);
        c.refs = FindDirectRefs(g_base + f.beginRva);
        for (const auto& s : c.opcodeSites) c.score = std::max(c.score, s.strength);
        if (f.beginRva > RVA_PROTOCOL_USE_CREATURE) c.score += 20;
        for (const auto& ref : c.refs) {
            const uintptr_t rva = ref.instruction - g_base;
            const auto owner = FindRuntimeFunction(rva);
            if (owner.found && owner.beginRva >= GAME_SCAN_BEGIN && owner.beginRva < GAME_SCAN_END) c.score += 200;
            else if (rva >= 0x00100000 && rva < 0x00300000) c.score += 10;
        }
        out.push_back(std::move(c));
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b){
        if (a.score != b.score) return a.score > b.score;
        return a.function.beginRva < b.function.beginRva;
    });
    return out;
}

void PrintCallerChain(const TalkCandidate& c) {
    std::cout << "Candidate helper       : "; PrintFunction(c.function); std::cout << " score=" << c.score << "\n";
    for (const auto& s : c.opcodeSites) {
        std::cout << "  opcode 0x96 site     : "; PrintRva(s.address - g_base); std::cout << "  " << s.kind << "\n";
    }
    std::cout << "  direct CALL/JMP refs : " << c.refs.size() << "\n";
    for (size_t i = 0; i < c.refs.size() && i < 24; ++i) {
        const auto& ref = c.refs[i];
        const uintptr_t call = ref.instruction;
        const auto owner = FindRuntimeFunction(call - g_base);
        std::cout << "    " << (ref.isCall ? "CALL " : "JMP  "); PrintRva(call - g_base); std::cout << " owner="; PrintFunction(owner);
        const bool game = owner.found && owner.beginRva >= GAME_SCAN_BEGIN && owner.beginRva < GAME_SCAN_END;
        if (game) std::cout << "  <GAME ACTION CLUSTER>";
        std::cout << "\n";

        if (game) {
            const auto wrappers = FindDirectRefs(g_base + owner.beginRva);
            std::cout << "      callers of owner : " << wrappers.size() << "\n";
            for (size_t j = 0; j < wrappers.size() && j < 12; ++j) {
                const auto wowner = FindRuntimeFunction(wrappers[j].instruction - g_base);
                std::cout << "        " << (wrappers[j].isCall ? "CALL " : "JMP  "); PrintRva(wrappers[j].instruction - g_base); std::cout << " owner="; PrintFunction(wowner);
                if (wowner.found && wowner.beginRva >= GAME_SCAN_BEGIN && wowner.beginRva < GAME_SCAN_END)
                    std::cout << "  <POSSIBLE Game::talk WRAPPER>";
                std::cout << "\n";
            }
        }
    }
}

void PrintTalkStringBindings() {
    Section("               TALK STRING / RIP-XREF BINDINGS");
    const std::vector<std::string> names = {"talk", "talkChannel", "talkPrivate"};
    for (const auto& name : names) {
        const auto strings = FindAsciiExact(name);
        std::cout << "\n[STRING] \"" << name << "\" exact-null matches=" << strings.size() << "\n";
        for (size_t i = 0; i < strings.size() && i < 24; ++i) {
            const uintptr_t str = strings[i];
            std::cout << "  str #" << (i+1) << " "; PrintRva(str - g_base); std::cout << "\n";
            const auto refs = FindRipRefs(str);
            std::cout << "    RIP refs=" << refs.size() << "\n";
            for (size_t j = 0; j < refs.size() && j < 16; ++j) {
                const auto owner = FindRuntimeFunction(refs[j] - g_base);
                std::cout << "      ref "; PrintRva(refs[j] - g_base); std::cout << " owner="; PrintFunction(owner); std::cout << "\n";

                // Registration-style bindings often place the function pointer
                // very close to the method-name string xref (autoWalk did this).
                const auto nearbyCode = FindNearbyRipCodeRefsFast(refs[j], 0x120);
                std::cout << "        nearby RIP->CODE refs=" << nearbyCode.size() << "\n";
                for (size_t k = 0; k < nearbyCode.size() && k < 24; ++k) {
                    const uintptr_t trva = nearbyCode[k].second - g_base;
                    std::cout << "          "; PrintRva(nearbyCode[k].first - g_base);
                    std::cout << " -> "; PrintRva(trva);
                    const auto targetOwner = FindRuntimeFunction(trva);
                    std::cout << " owner="; PrintFunction(targetOwner);
                    if (trva >= GAME_SCAN_BEGIN && trva < GAME_SCAN_END) std::cout << " <GAME CLUSTER>";
                    std::cout << "\n";
                }

                if (owner.found && (owner.endRva - owner.beginRva) <= 0x4000) {
                    const auto calls = CallsInside(owner);
                    size_t shown = 0;
                    for (const auto& c : calls) {
                        if (c.target < g_base || c.target >= g_imageEnd) continue;
                        const uintptr_t trva = c.target - g_base;
                        if ((trva >= GAME_SCAN_BEGIN && trva < GAME_SCAN_END) ||
                            (trva >= PROTO_SCAN_BEGIN && trva < PROTO_SCAN_END)) {
                            std::cout << "        owner CALL "; PrintRva(c.instruction - g_base);
                            std::cout << " -> "; PrintRva(trva);
                            const auto targetOwner = FindRuntimeFunction(trva);
                            std::cout << " owner="; PrintFunction(targetOwner);
                            if (trva >= GAME_SCAN_BEGIN && trva < GAME_SCAN_END) std::cout << " <GAME CLUSTER>";
                            if (trva >= PROTO_SCAN_BEGIN && trva < PROTO_SCAN_END) std::cout << " <PROTOCOL CLUSTER>";
                            std::cout << "\n";
                            if (++shown >= 20) break;
                        }
                    }
                }
            }
        }
    }
}

void PrintGameActionSequence() {
    Section("          GAME ACTION FUNCTION SEQUENCE AROUND ATTACK/FOLLOW/TALK");
    const FunctionInfo attack = FindRuntimeFunction(RVA_GAME_ATTACK);
    const FunctionInfo follow = FindRuntimeFunction(RVA_GAME_FOLLOW);
    std::cout << "Known Game::attack     : "; PrintFunction(attack); std::cout << "\n";
    std::cout << "Known Game::follow     : "; PrintFunction(follow); std::cout << "\n\n";

    if (!attack.found) return;
    const size_t count = RuntimeFunctionCount();
    const size_t start = attack.tableIndex > 2 ? attack.tableIndex - 2 : 0;
    const size_t finish = std::min(count, attack.tableIndex + 20);
    for (size_t i = start; i < finish; ++i) {
        RuntimeFunctionEntry e{};
        if (!GetRuntimeFunction(i, e)) continue;
        if (e.BeginAddress < GAME_SCAN_BEGIN || e.BeginAddress >= GAME_SCAN_END) continue;
        FunctionInfo f{true, e.BeginAddress, e.EndAddress, e.UnwindInfoAddress, i};
        std::cout << "[#" << i << "] "; PrintFunction(f);
        if (f.beginRva == attack.beginRva) std::cout << "  <Game::attack>";
        if (f.beginRva == follow.beginRva) std::cout << "  <Game::follow>";
        std::cout << "\n";

        const auto calls = CallsInside(f);
        int shown = 0;
        for (const auto& c : calls) {
            if (c.target < g_base || c.target >= g_imageEnd) continue;
            const uintptr_t trva = c.target - g_base;
            if ((trva >= PROTO_SCAN_BEGIN && trva < PROTO_SCAN_END) ||
                (trva >= GAME_SCAN_BEGIN && trva < GAME_SCAN_END)) {
                std::cout << "      CALL "; PrintRva(c.instruction - g_base); std::cout << " -> "; PrintRva(trva);
                const auto targetOwner = FindRuntimeFunction(trva);
                if (targetOwner.found) { std::cout << " owner="; PrintFunction(targetOwner); }
                std::cout << "\n";
                if (++shown >= 16) break;
            }
        }

        const auto modeSites = FindOpcodeSites(f, 0x01);
        for (size_t k = 0; k < modeSites.size() && k < 3; ++k) {
            if (modeSites[k].strength >= 70) {
                std::cout << "      MessageSay=1 imm : "; PrintRva(modeSites[k].address - g_base);
                std::cout << " " << modeSites[k].kind << "\n";
            }
        }
    }
}

void RunScan() {
    Section("              CROME SDK V17.1 - FAST TALK/SAY OFFLINE SCAN");
    std::cout << "[MODE] READ-ONLY. No internal call / write / hook / breakpoint.\n";
    std::cout << "ModuleBase            : 0x"; Hex(); std::cout << g_base; Dec(); std::cout << "\n";
    std::cout << ".text RVA            : "; PrintRva(g_textBegin - g_base); std::cout << " - "; PrintRva(g_textEnd - g_base); std::cout << "\n";
    std::cout << ".pdata RVA           : "; PrintRva(g_exceptionRva); std::cout << " size=0x"; Hex(); std::cout << g_exceptionSize; Dec(); std::cout << "\n";
    std::cout << "Runtime functions     : " << RuntimeFunctionCount() << "\n";

    // Print the known Game neighborhood FIRST. This is fast and gives useful
    // output even if later heuristic scans find nothing.
    PrintGameActionSequence();

    Section("               KNOWN PROTOCOL OPCODE ANCHORS");
    PrintKnownProtocolAnchor("MOVE helper", RVA_PROTOCOL_MOVE, 0x78);
    PrintKnownProtocolAnchor("USE helper", RVA_PROTOCOL_USE, 0x82);
    PrintKnownProtocolAnchor("USE-ON-CREATURE helper", RVA_PROTOCOL_USE_CREATURE, 0x84);

    PrintTalkStringBindings();

    Section("             PROTOCOL ClientTalk=0x96 CANDIDATES");
    const auto candidates = FindProtocolTalkCandidates();
    std::cout << "Candidates found       : " << candidates.size() << "\n";
    const size_t limit = std::min<size_t>(candidates.size(), 20);
    for (size_t i = 0; i < limit; ++i) {
        std::cout << "\n---------------- TALK CANDIDATE #" << (i+1) << " ----------------\n";
        PrintCallerChain(candidates[i]);
        if (!candidates[i].opcodeSites.empty()) DumpHex(candidates[i].opcodeSites.front().address, 24, 40);
    }

    Section("                         SCAN COMPLETE");
    std::cout << "Send the full console output. The useful chain should look like:\n"
              << "  protocol helper containing 0x96\n"
              << "    <- caller in Game cluster after follow\n"
              << "       <- small wrapper in same cluster with MessageSay=1/channel=0\n";
}

DWORD WINAPI MainThread(LPVOID) {
    AllocConsole();
    FILE* dummy = nullptr;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);
    SetConsoleTitleA("CROME SDK V17.1 - FAST TALK/SAY XREF SCANNER");

    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    const bool peOk = ResolvePE();

    std::cout << "============================================================\n"
              << "       CROME SDK V17.1 - FAST TALK / SAY XREF SCANNER\n"
              << "============================================================\n";
    std::cout << "ModuleBase           : 0x"; Hex(); std::cout << g_base; Dec(); std::cout << "\n";
    std::cout << "PE/.text/.pdata      : " << (peOk ? "OK" : "FAIL") << "\n";
    std::cout << "Known Game::attack   : RVA 0x165410\n"
              << "Known Game::follow   : RVA 0x1656E0\n"
              << "Target ClientTalk    : opcode 0x96\n\n";
    std::cout << "F7  = executar scan TALK/SAY novamente\n"
              << "END = descarregar DLL\n"
              << "============================================================\n";

    if (peOk) RunScan();

    bool f7Down = false, endDown = false;
    while (g_running) {
        const bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        const bool end = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
        if (f7 && !f7Down && peOk) RunScan();
        if (end && !endDown) g_running = false;
        f7Down = f7;
        endDown = end;
        Sleep(30);
    }

    FreeConsole();
    FreeLibraryAndExitThread(g_dll, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_dll = hModule;
        HANDLE thread = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
