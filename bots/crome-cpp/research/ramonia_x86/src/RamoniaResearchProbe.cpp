#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>
#include "ProbeConsole.hpp"
#include "RamoniaPipeBaseline.hpp"
#include "GuidedDiscovery.hpp"
#include "../../../Crome/Developer/native_runtime/profiles/MiniJson.hpp"

// ============================================================================
// OTCLab Ramonia x86 Research Probe V39
// DISCOVERY READ-ONLY + ONE-SHOT RUNTIME ABI CAPTURE
// No internal game action is called by the probe. V33 temporarily replaces one
// selected action entry byte with INT3, captures the natural game call made by
// the user, restores the byte immediately, and then analyzes the captured ABI.
//
// Purpose:
//   Validate a new x86 discovery strategy before it is merged back into the
//   generic BuildMigrator:
//     PE -> RTTI -> semantic Lua/OTC binds -> Game/Map owners -> static Game
//     layout inference -> runtime validation -> persistent Creature/Light discovery.
//
// Important:
//   This source intentionally does NOT contain Ramonia RVAs from another build.
//   It rediscovers addresses from the loaded executable every run.
// ============================================================================

static_assert(sizeof(void*) == 4,
              "OTCLab Ramonia x86 probe must be compiled with a 32-bit compiler.");

namespace {

HMODULE g_self = nullptr;

std::string HexU64(uint64_t v, int width = 0) {
    std::ostringstream os;
    os << "0x" << std::uppercase << std::hex;
    if (width > 0) os << std::setw(width) << std::setfill('0');
    os << v;
    return os.str();
}

std::string HexU32(uint32_t v) { return HexU64(v); }

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string JsonEscape(const std::string& s) {
    std::ostringstream os;
    for (unsigned char c : s) {
        switch (c) {
            case '\\': os << "\\\\"; break;
            case '"':  os << "\\\""; break;
            case '\n': os << "\\n"; break;
            case '\r': os << "\\r"; break;
            case '\t': os << "\\t"; break;
            default:
                if (c < 0x20) {
                    os << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<int>(c) << std::dec;
                } else {
                    os << static_cast<char>(c);
                }
        }
    }
    return os.str();
}

bool IsReadableProtection(DWORD protect) {
    if (protect & PAGE_GUARD) return false;
    if (protect & PAGE_NOACCESS) return false;
    const DWORD p = protect & 0xFF;
    return p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
           p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ ||
           p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

bool SafeCopy(uintptr_t address, void* out, size_t size) {
    if (!address || !out || size == 0) return false;
    auto* dst = static_cast<uint8_t*>(out);
    size_t done = 0;
    while (done < size) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(address + done), &mbi, sizeof(mbi))) {
            return false;
        }
        if (mbi.State != MEM_COMMIT || !IsReadableProtection(mbi.Protect)) return false;
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= address + done) return false;
        const size_t chunk = std::min<size_t>(size - done, regionEnd - (address + done));
        std::memcpy(dst + done, reinterpret_cast<const void*>(address + done), chunk);
        done += chunk;
    }
    return true;
}

template <typename T>
bool SafeRead(uintptr_t address, T& out) {
    return SafeCopy(address, &out, sizeof(T));
}

bool SafeReadCString(uintptr_t address, std::string& out, size_t maxLen = 512) {
    out.clear();
    for (size_t i = 0; i < maxLen; ++i) {
        char c = 0;
        if (!SafeRead(address + i, c)) return false;
        if (!c) return true;
        out.push_back(c);
    }
    return false;
}

uint64_t Fnv1a64(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

struct SectionInfo {
    std::string name;
    uint32_t rva = 0;
    uint32_t virtualSize = 0;
    uint32_t rawSize = 0;
    uint32_t characteristics = 0;
    uintptr_t start = 0;
    uintptr_t end = 0;

    bool writable() const { return (characteristics & IMAGE_SCN_MEM_WRITE) != 0; }
    bool executable() const { return (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0; }
    bool readable() const { return (characteristics & IMAGE_SCN_MEM_READ) != 0; }
};

struct ModuleView {
    uintptr_t base = 0;
    uint32_t timestamp = 0;
    uint32_t sizeOfImage = 0;
    uint32_t entryRva = 0;
    std::string path;
    std::vector<SectionInfo> sections;
    std::map<std::string,std::vector<uintptr_t>> semanticStringIndex;
    IMAGE_DATA_DIRECTORY importDirectory{};

    const SectionInfo* text() const {
        for (const auto& s : sections) if (s.name == ".text") return &s;
        return nullptr;
    }

    const SectionInfo* sectionFor(uintptr_t address) const {
        for (const auto& s : sections) {
            if (address >= s.start && address < s.end) return &s;
        }
        return nullptr;
    }

    bool inModule(uintptr_t address) const {
        return address >= base && address < base + sizeOfImage;
    }

    bool inText(uintptr_t address) const {
        const auto* t = text();
        return t && address >= t->start && address < t->end;
    }

    uint32_t rva(uintptr_t address) const {
        return inModule(address) ? static_cast<uint32_t>(address - base) : 0;
    }
};

bool BuildModuleView(ModuleView& m) {
    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe) return false;
    m.base = reinterpret_cast<uintptr_t>(exe);

    IMAGE_DOS_HEADER dos{};
    if (!SafeRead(m.base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) return false;

    const uintptr_t ntAddr = m.base + static_cast<uint32_t>(dos.e_lfanew);
    DWORD signature = 0;
    if (!SafeRead(ntAddr, signature) || signature != IMAGE_NT_SIGNATURE) return false;

    IMAGE_FILE_HEADER fh{};
    if (!SafeRead(ntAddr + 4, fh)) return false;
    if (fh.Machine != IMAGE_FILE_MACHINE_I386) return false;

    IMAGE_OPTIONAL_HEADER32 oh{};
    if (!SafeRead(ntAddr + 4 + sizeof(IMAGE_FILE_HEADER), oh)) return false;
    if (oh.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return false;

    m.timestamp = fh.TimeDateStamp;
    m.sizeOfImage = oh.SizeOfImage;
    m.entryRva = oh.AddressOfEntryPoint;
    m.importDirectory = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

    char path[MAX_PATH * 4]{};
    DWORD n = GetModuleFileNameA(exe, path, static_cast<DWORD>(sizeof(path)));
    if (n) m.path.assign(path, path + n);

    const uintptr_t secBase = ntAddr + 4 + sizeof(IMAGE_FILE_HEADER) + fh.SizeOfOptionalHeader;
    for (unsigned i = 0; i < fh.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER sh{};
        if (!SafeRead(secBase + i * sizeof(sh), sh)) return false;
        char name[9]{};
        std::memcpy(name, sh.Name, 8);
        SectionInfo s;
        s.name = name;
        s.rva = sh.VirtualAddress;
        s.virtualSize = sh.Misc.VirtualSize;
        s.rawSize = sh.SizeOfRawData;
        s.characteristics = sh.Characteristics;
        s.start = m.base + sh.VirtualAddress;
        uint32_t mappedSize = std::max<uint32_t>(sh.Misc.VirtualSize, sh.SizeOfRawData);
        if (sh.VirtualAddress + mappedSize > m.sizeOfImage) {
            mappedSize = (sh.VirtualAddress < m.sizeOfImage) ? (m.sizeOfImage - sh.VirtualAddress) : 0;
        }
        s.end = s.start + mappedSize;
        m.sections.push_back(s);
    }
    return true;
}

struct DiskFingerprint {
    uint64_t textHash = 0;
    uint32_t textRawSize = 0;
    bool ok = false;
};

DiskFingerprint FingerprintTextFromDisk(const std::string& path) {
    DiskFingerprint fp;
    std::ifstream f(path, std::ios::binary);
    if (!f) return fp;
    f.seekg(0, std::ios::end);
    const auto len = f.tellg();
    if (len <= 0) return fp;
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(len));
    f.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (!f) return fp;
    if (bytes.size() < sizeof(IMAGE_DOS_HEADER)) return fp;

    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, bytes.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) return fp;
    const size_t nt = static_cast<size_t>(dos.e_lfanew);
    if (nt + 4 + sizeof(IMAGE_FILE_HEADER) > bytes.size()) return fp;
    DWORD sig = 0;
    std::memcpy(&sig, bytes.data() + nt, 4);
    if (sig != IMAGE_NT_SIGNATURE) return fp;
    IMAGE_FILE_HEADER fh{};
    std::memcpy(&fh, bytes.data() + nt + 4, sizeof(fh));
    const size_t secBase = nt + 4 + sizeof(IMAGE_FILE_HEADER) + fh.SizeOfOptionalHeader;
    for (unsigned i = 0; i < fh.NumberOfSections; ++i) {
        if (secBase + (i + 1) * sizeof(IMAGE_SECTION_HEADER) > bytes.size()) return fp;
        IMAGE_SECTION_HEADER sh{};
        std::memcpy(&sh, bytes.data() + secBase + i * sizeof(sh), sizeof(sh));
        char name[9]{};
        std::memcpy(name, sh.Name, 8);
        if (std::string(name) != ".text") continue;
        const size_t raw = sh.PointerToRawData;
        const size_t rawSize = sh.SizeOfRawData;
        if (raw + rawSize > bytes.size()) return fp;
        fp.textHash = Fnv1a64(bytes.data() + raw, rawSize);
        fp.textRawSize = static_cast<uint32_t>(rawSize);
        fp.ok = true;
        return fp;
    }
    return fp;
}

uint64_t FingerprintMappedText(const ModuleView& m) {
    const SectionInfo* t = m.text();
    if (!t || t->end <= t->start) return 0;
    const size_t n = static_cast<size_t>(t->end - t->start);
    return Fnv1a64(reinterpret_cast<const uint8_t*>(t->start), n);
}

std::vector<std::string> ReadImports(const ModuleView& m) {
    std::vector<std::string> out;
    if (!m.importDirectory.VirtualAddress || !m.importDirectory.Size) return out;
    uintptr_t p = m.base + m.importDirectory.VirtualAddress;
    for (size_t i = 0; i < 1024; ++i) {
        IMAGE_IMPORT_DESCRIPTOR d{};
        if (!SafeRead(p + i * sizeof(d), d)) break;
        if (!d.Name && !d.FirstThunk && !d.OriginalFirstThunk) break;
        if (!d.Name || d.Name >= m.sizeOfImage) continue;
        std::string name;
        if (SafeReadCString(m.base + d.Name, name, 256) && !name.empty()) out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string RendererHint(const std::vector<std::string>& imports) {
    bool gl = false, gles = false, d3d = false;
    for (auto s : imports) {
        s = Lower(s);
        if (s.find("opengl32") != std::string::npos) gl = true;
        if (s.find("libglesv2") != std::string::npos || s.find("libegl") != std::string::npos) gles = true;
        if (s.find("d3d9") != std::string::npos || s.find("d3d11") != std::string::npos ||
            s.find("dxgi") != std::string::npos) d3d = true;
    }
    if (gl) return "opengl";
    if (gles) return "angle_gles_egl";
    if (d3d) return "direct3d";
    return "unknown";
}

std::vector<uintptr_t> FindBytesInSection(const SectionInfo& s,
                                          const uint8_t* needle,
                                          size_t needleSize,
                                          size_t maxHits = 4096) {
    std::vector<uintptr_t> hits;
    if (!needle || !needleSize || s.end <= s.start) return hits;
    const size_t size = static_cast<size_t>(s.end - s.start);
    if (size < needleSize) return hits;

    MEMORY_BASIC_INFORMATION mbi{};
    uintptr_t cur = s.start;
    while (cur < s.end && hits.size() < maxHits) {
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi))) break;
        const uintptr_t regionStart = std::max<uintptr_t>(cur, reinterpret_cast<uintptr_t>(mbi.BaseAddress));
        const uintptr_t regionEnd = std::min<uintptr_t>(s.end,
            reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize);
        if (mbi.State == MEM_COMMIT && IsReadableProtection(mbi.Protect) && regionEnd > regionStart) {
            const auto* p = reinterpret_cast<const uint8_t*>(regionStart);
            const size_t n = static_cast<size_t>(regionEnd - regionStart);
            if (n >= needleSize) {
                for (size_t i = 0; i + needleSize <= n && hits.size() < maxHits; ++i) {
                    if (std::memcmp(p + i, needle, needleSize) == 0) hits.push_back(regionStart + i);
                }
            }
        }
        if (regionEnd <= cur) break;
        cur = regionEnd;
    }
    return hits;
}

std::vector<uintptr_t> FindU32Refs(const SectionInfo& s, uint32_t value, size_t maxHits = 4096) {
    const uint8_t needle[4] = {
        static_cast<uint8_t>(value & 0xFF),
        static_cast<uint8_t>((value >> 8) & 0xFF),
        static_cast<uint8_t>((value >> 16) & 0xFF),
        static_cast<uint8_t>((value >> 24) & 0xFF)
    };
    return FindBytesInSection(s, needle, 4, maxHits);
}

struct MemberRefDiag {
    uintptr_t address = 0;
    uint32_t rva = 0;
    std::string access = "unknown";
    std::string opcode = "raw-disp32";
    std::string context;
};

bool ModRmUsesDisp32(uint8_t modrm) {
    return ((modrm >> 6) & 0x3) == 2;
}

std::string HexBytesAround(uintptr_t ref, int before=6, int after=4) {
    std::ostringstream os;
    for (int i=-before; i<after; ++i) {
        uint8_t b=0;
        if (!SafeRead(ref+i,b)) {
            os << "??";
        } else {
            os << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<unsigned>(b);
        }
        if (i+1<after) os << ' ';
    }
    return os.str();
}

MemberRefDiag ClassifyMemberDispRefX86(const ModuleView& m, uintptr_t ref) {
    MemberRefDiag d;
    d.address=ref;
    d.rva=m.rva(ref);
    d.context=HexBytesAround(ref);

    auto rd=[&](int rel, uint8_t& v){ return SafeRead(ref+rel,v); };
    uint8_t m1=0,m2=0,o2=0,o3=0,o4=0,o5=0;
    rd(-1,m1); rd(-2,m2); rd(-3,o3); rd(-4,o4); rd(-5,o5); rd(-2,o2);

    // SSE2 scalar double: F2 0F 10 /r [base+disp32] = read,
    // F2 0F 11 /r [base+disp32] = write. Handle optional SIB.
    if (ModRmUsesDisp32(m1) && o4==0xF2 && o3==0x0F && (o2==0x10 || o2==0x11)) {
        d.access = (o2==0x10) ? "read" : "write";
        d.opcode = (o2==0x10) ? "movsd-load" : "movsd-store";
        return d;
    }
    if (ModRmUsesDisp32(m2) && o5==0xF2 && o4==0x0F && (o3==0x10 || o3==0x11)) {
        d.access = (o3==0x10) ? "read" : "write";
        d.opcode = (o3==0x10) ? "movsd-load-sib" : "movsd-store-sib";
        return d;
    }

    // Common integer/address forms with disp32.
    if (ModRmUsesDisp32(m1)) {
        if (m2==0x8B) { d.access="read"; d.opcode="mov-load"; return d; }
        if (m2==0x89) { d.access="write"; d.opcode="mov-store"; return d; }
        if (m2==0x8D) { d.access="address"; d.opcode="lea"; return d; }
        if (m2==0xDD || m2==0xD9) {
            const unsigned reg=(m1>>3)&7;
            if (reg==0) { d.access="read"; d.opcode=(m2==0xDD?"fld-qword":"fld-dword"); }
            else if (reg==2 || reg==3) { d.access="write"; d.opcode=(m2==0xDD?"fst/fstp-qword":"fst/fstp-dword"); }
            else { d.access="readwrite"; d.opcode="x87-memory"; }
            return d;
        }
    }
    if (ModRmUsesDisp32(m2)) {
        if (o3==0x8B) { d.access="read"; d.opcode="mov-load-sib"; return d; }
        if (o3==0x89) { d.access="write"; d.opcode="mov-store-sib"; return d; }
        if (o3==0x8D) { d.access="address"; d.opcode="lea-sib"; return d; }
        if (o3==0xDD || o3==0xD9) {
            const unsigned reg=(m2>>3)&7;
            if (reg==0) { d.access="read"; d.opcode=(o3==0xDD?"fld-qword-sib":"fld-dword-sib"); }
            else if (reg==2 || reg==3) { d.access="write"; d.opcode=(o3==0xDD?"fst/fstp-qword-sib":"fst/fstp-dword-sib"); }
            else { d.access="readwrite"; d.opcode="x87-memory-sib"; }
            return d;
        }
    }
    return d;
}

std::vector<MemberRefDiag> CollectMemberOffsetRefsV9(const ModuleView& m, int offset) {
    std::vector<MemberRefDiag> out;
    const auto* t=m.text();
    if(!t || offset<0) return out;
    auto refs=FindU32Refs(*t,static_cast<uint32_t>(offset),512);
    out.reserve(refs.size());
    for(uintptr_t ref:refs) out.push_back(ClassifyMemberDispRefX86(m,ref));
    return out;
}

void PrintMemberOffsetRefsV9(const ModuleView& m, int offset, const char* label) {
    auto refs=CollectMemberOffsetRefsV9(m,offset);
    size_t reads=0,writes=0,addresses=0,unknown=0;
    for(const auto& r:refs) {
        if(r.access=="read") ++reads;
        else if(r.access=="write") ++writes;
        else if(r.access=="address") ++addresses;
        else ++unknown;
    }
    std::cout << "[MEMBER OFFSET REF V9] " << label << " +" << HexU32(offset)
              << " refs=" << refs.size()
              << " read=" << reads << " write=" << writes
              << " address=" << addresses << " other=" << unknown
              << " (static diagnostic, no promotion by itself)\\n";
    for(size_t i=0;i<refs.size() && i<16;++i) {
        const auto& r=refs[i];
        std::cout << "  RVA=" << HexU32(r.rva)
                  << " access=" << r.access
                  << " op=" << r.opcode
                  << " bytes=[" << r.context << "]\\n";
    }
}

void PrintCapacityPairCodeUseV9(const ModuleView& m, int aOff, int bOff) {
    if(aOff<0 || bOff<0) return;
    auto a=CollectMemberOffsetRefsV9(m,aOff);
    auto b=CollectMemberOffsetRefsV9(m,bOff);
    struct Near { uint32_t a=0,b=0,delta=0; std::string aa,ba,ao,bo; };
    std::vector<Near> nearPairs;
    for(const auto& x:a) for(const auto& y:b) {
        const uint32_t d = x.rva>y.rva ? x.rva-y.rva : y.rva-x.rva;
        if(d<=0x80) nearPairs.push_back({x.rva,y.rva,d,x.access,y.access,x.opcode,y.opcode});
    }
    std::sort(nearPairs.begin(),nearPairs.end(),[](const Near& x,const Near& y){
        if(x.delta!=y.delta) return x.delta<y.delta;
        if(x.a!=y.a) return x.a<y.a;
        return x.b<y.b;
    });
    std::cout << "[CAPACITY CODE-USE V9] nearby A/B .text sites=" << nearPairs.size()
              << " (<=0x80 bytes; diagnostic only)\\n";
    for(size_t i=0;i<nearPairs.size() && i<16;++i) {
        const auto& n=nearPairs[i];
        std::cout << "  A@" << HexU32(n.a) << " " << n.aa << "/" << n.ao
                  << "  B@" << HexU32(n.b) << " " << n.ba << "/" << n.bo
                  << "  delta=" << HexU32(n.delta) << "\\n";
    }
}

std::vector<uintptr_t> FindStringAddresses(const ModuleView& m, const std::string& exact) {
    const auto indexed=m.semanticStringIndex.find(exact);
    if(indexed!=m.semanticStringIndex.end()) return indexed->second;
    std::vector<uintptr_t> out;
    std::vector<uint8_t> needle(exact.begin(), exact.end());
    needle.push_back(0);
    for (const auto& s : m.sections) {
        if (!s.readable() || s.executable()) continue;
        auto h = FindBytesInSection(s, needle.data(), needle.size(), 128);
        out.insert(out.end(), h.begin(), h.end());
    }
    return out;
}

void BuildSemanticStringIndex(ModuleView& module,const std::vector<std::string>& tokens) {
    module.semanticStringIndex.clear();
    for(const auto& token:tokens) module.semanticStringIndex[token]={};
    guided_discovery::TokenIndex index(tokens);
    std::array<uint8_t,4096> bytes{};
    std::cout << "[BUSCA COMPARTILHADA] " << tokens.size()
              << " nomes em uma passagem; nenhuma secao de recursos excluida.\n" << std::flush;
    for(const auto& section:module.sections) {
        if(!section.readable() || section.executable() || section.end<=section.start) continue;
        index.gap();
        std::cout << "[BUSCANDO STRINGS] " << section.name << " bytes=" << section.end-section.start << "\n" << std::flush;
        uintptr_t progress=section.start;
        size_t skipped=0;
        for(uintptr_t address=section.start;address<section.end;) {
            const size_t take=std::min<size_t>(section.end-address,4096u-(address&4095u));
            SIZE_T got=0;
            if(ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<LPCVOID>(address),bytes.data(),take,&got) && got==take)
                index.feed(bytes.data(),take,address,[&](size_t id,uintptr_t at) {
                    module.semanticStringIndex[tokens[id]].push_back(at);
                });
            else { index.gap(); skipped+=take; }
            address+=take;
            if(address-progress>=16u*1024u*1024u) {
                std::cout << "[PROGRESSO STRINGS] " << section.name << " "
                          << address-section.start << "/" << section.end-section.start << " bytes\n" << std::flush;
                progress=address;
            }
        }
        std::cout << "[SECAO CONCLUIDA] " << section.name << " bytes_nao_lidos=" << skipped << "\n" << std::flush;
    }
}

struct RttiResult {
    std::string className;
    std::vector<uint32_t> vtableRvas;
    uint32_t typeDescriptorRva = 0;
};

#pragma pack(push, 1)
struct CompleteObjectLocator32 {
    uint32_t signature;
    uint32_t offset;
    uint32_t cdOffset;
    uint32_t pTypeDescriptor;
    uint32_t pClassDescriptor;
};
#pragma pack(pop)

bool LooksLikeVtable(const ModuleView& m, uintptr_t vt) {
    int code = 0;
    for (int i = 0; i < 4; ++i) {
        uint32_t fn = 0;
        if (!SafeRead(vt + i * 4, fn)) return false;
        if (m.inText(fn)) ++code;
    }
    return code >= 2;
}

RttiResult ResolveRttiClass(const ModuleView& m, const std::string& cls) {
    RttiResult out;
    out.className = cls;
    const std::string decorated = ".?AV" + cls + "@@";
    auto names = FindStringAddresses(m, decorated);
    std::set<uint32_t> vts;
    for (uintptr_t nameAddr : names) {
        if (nameAddr < m.base + 8) continue;
        const uintptr_t td = nameAddr - 8;
        if (!out.typeDescriptorRva) out.typeDescriptorRva = m.rva(td);
        const uint32_t td32 = static_cast<uint32_t>(td);
        for (const auto& sec : m.sections) {
            if (!sec.readable()) continue;
            auto refs = FindU32Refs(sec, td32, 512);
            for (uintptr_t ref : refs) {
                if (ref < m.base + 12) continue;
                const uintptr_t colAddr = ref - 12;
                CompleteObjectLocator32 col{};
                if (!SafeRead(colAddr, col)) continue;
                if (col.pTypeDescriptor != td32) continue;
                if (col.signature > 1) continue;
                if (!m.inModule(col.pClassDescriptor)) continue;
                const uint32_t col32 = static_cast<uint32_t>(colAddr);
                for (const auto& sec2 : m.sections) {
                    if (!sec2.readable()) continue;
                    auto colRefs = FindU32Refs(sec2, col32, 512);
                    for (uintptr_t colRef : colRefs) {
                        const uintptr_t vt = colRef + 4;
                        if (LooksLikeVtable(m, vt)) vts.insert(m.rva(vt));
                    }
                }
            }
        }
    }
    out.vtableRvas.assign(vts.begin(), vts.end());
    return out;
}

std::vector<std::string> FindAsciiStringsContaining(const ModuleView& m, const std::string& needle) {
    guided_discovery::AsciiHints hints(needle);
    std::array<uint8_t,4096> bytes{};
    for (const auto& sec : m.sections) {
        if (!sec.readable() || sec.executable() || sec.end <= sec.start) continue;
        uintptr_t cur = sec.start;
        while (cur < sec.end) {
            // Align chunks to page boundaries so an unreadable page does not
            // discard readable bytes from the adjacent page.
            const size_t take=std::min<size_t>(sec.end-cur,4096u-(cur&4095u));
            SIZE_T got=0;
            if(ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<LPCVOID>(cur),bytes.data(),take,&got)
               && got==take) hints.feed(bytes.data(),take);
            else hints.gap();
            cur+=take;
        }
        hints.finish();
    }
    return hints.result();
}

struct BindCandidate {
    uint32_t tokenRva = 0;
    uint32_t xrefRva = 0;
    uint32_t pairRva = 0;
    uint32_t ownerRva = 0;
    uint32_t methodRva = 0;
    std::string ownerSection;
    int distance = 0;
    int score = 0;
};

struct BindResult {
    std::string name;
    std::vector<BindCandidate> candidates;
    BindCandidate selected{};
    bool selectedOk = false;
};

void PrintAllBindCandidates(const BindResult& bind) {
    std::cout << "[CANDIDATOS] " << bind.name << " total=" << bind.candidates.size() << "\n";
    for(size_t i=0;i<bind.candidates.size();++i) {
        const auto& candidate=bind.candidates[i];
        std::cout << "  #" << i+1 << " method=" << HexU32(candidate.methodRva)
                  << " owner=" << HexU32(candidate.ownerRva) << " [" << candidate.ownerSection
                  << "] score=" << candidate.score << " token=" << HexU32(candidate.tokenRva)
                  << " xref=" << HexU32(candidate.xrefRva) << "\n";
    }
    if(bind.candidates.empty()) std::cout << "  [PENDENTE] nenhum candidato; ajuda de busca sera oferecida no guia.\n";
    else if(guided_discovery::candidates(bind).size()>1)
        std::cout << "  [DUVIDA] varios enderecos: testar por acao natural; score nao confirma ABI.\n";
    std::cout << std::flush;
}

std::vector<BindCandidate> ResolveBindCandidates(const ModuleView& m, const std::string& token) {
    std::vector<BindCandidate> out;
    const SectionInfo* text = m.text();
    if (!text) return out;
    const auto tokenAddrs = FindStringAddresses(m, token);
    std::set<std::pair<uint32_t,uint32_t>> seen;

    for (uintptr_t tokenAddr : tokenAddrs) {
        const auto xrefs = FindU32Refs(*text, static_cast<uint32_t>(tokenAddr), 4096);
        for (uintptr_t xref : xrefs) {
            uintptr_t start = (xref > text->start + 16) ? xref - 16 : text->start;
            uintptr_t end = std::min<uintptr_t>(text->end, xref + 160);
            for (uintptr_t p = start; p + 10 <= end; ++p) {
                uint8_t op1 = 0, op2 = 0;
                if (!SafeRead(p, op1) || op1 != 0x68) continue;
                if (!SafeRead(p + 5, op2) || op2 != 0x68) continue;
                uint32_t owner = 0, method = 0;
                if (!SafeRead(p + 1, owner) || !SafeRead(p + 6, method)) continue;
                if (!m.inModule(owner) || !m.inText(method)) continue;
                const SectionInfo* ownerSec = m.sectionFor(owner);
                if (!ownerSec || ownerSec->executable()) continue;
                const uint32_t ownerRva = m.rva(owner);
                const uint32_t methodRva = m.rva(method);
                if (!seen.insert({ownerRva, methodRva}).second) continue;

                BindCandidate c;
                c.tokenRva = m.rva(tokenAddr);
                c.xrefRva = m.rva(xref);
                c.pairRva = m.rva(p);
                c.ownerRva = ownerRva;
                c.methodRva = methodRva;
                c.ownerSection = ownerSec->name;
                c.distance = static_cast<int>(p - xref);
                c.score = 0;
                if (ownerSec->writable()) c.score += 60;
                if (c.distance >= 0 && c.distance <= 96) c.score += 40;
                c.score -= std::min(30, std::abs(c.distance - 48) / 2);
                uint8_t first = 0;
                SafeRead(method, first);
                if (first == 0x55 || first == 0x56 || first == 0x57 || first == 0x83 || first == 0x8B) c.score += 10;
                out.push_back(c);
            }
        }
    }

    std::sort(out.begin(), out.end(), [](const BindCandidate& a, const BindCandidate& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.distance != b.distance) return std::abs(a.distance - 48) < std::abs(b.distance - 48);
        if (a.ownerRva != b.ownerRva) return a.ownerRva < b.ownerRva;
        return a.methodRva < b.methodRva;
    });
    return out;
}


// V13: class-instance Lua binds (for example UIMap methods) do not carry a
// singleton owner next to the method pointer. Resolve them as:
// exact token -> x86 immediate xref -> nearby PUSH <.text method>.
std::vector<BindCandidate> ResolveInstanceMethodCandidatesV13(const ModuleView& m,
                                                               const std::string& token) {
    std::vector<BindCandidate> out;
    const SectionInfo* text = m.text();
    if (!text) return out;
    const auto tokenAddrs = FindStringAddresses(m, token);
    std::set<uint32_t> seenMethods;

    for (uintptr_t tokenAddr : tokenAddrs) {
        const auto xrefs = FindU32Refs(*text, static_cast<uint32_t>(tokenAddr), 4096);
        for (uintptr_t xref : xrefs) {
            const uintptr_t begin = std::min<uintptr_t>(text->end, xref + 4);
            const uintptr_t finish = std::min<uintptr_t>(text->end, xref + 96);
            for (uintptr_t p = begin; p + 5 <= finish; ++p) {
                uint8_t op = 0;
                uint32_t imm = 0;
                if (!SafeRead(p, op) || op != 0x68 || !SafeRead(p + 1, imm)) continue;
                if (!m.inText(imm)) continue;
                const uint32_t methodRva = m.rva(imm);
                if (!seenMethods.insert(methodRva).second) continue;

                BindCandidate c;
                c.tokenRva = m.rva(tokenAddr);
                c.xrefRva = m.rva(xref);
                c.pairRva = m.rva(p);
                c.ownerRva = 0;
                c.methodRva = methodRva;
                c.ownerSection = "instance";
                c.distance = static_cast<int>(p - xref);
                c.score = 100 - std::min(80, std::abs(c.distance - 24));
                uint8_t first = 0;
                SafeRead(imm, first);
                if (first == 0x55 || first == 0x8B || first == 0x56 || first == 0x57) c.score += 20;
                if (c.distance >= 8 && c.distance <= 64) c.score += 30;
                out.push_back(c);
            }
        }
    }

    std::sort(out.begin(), out.end(), [](const BindCandidate& a, const BindCandidate& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.distance != b.distance) return a.distance < b.distance;
        return a.methodRva < b.methodRva;
    });
    return out;
}

uint32_t ChooseOwnerCluster(const std::map<std::string, BindResult>& binds,
                            const std::vector<std::string>& names,
                            int minUniqueTokens) {
    std::map<uint32_t, int> tokenCount;
    std::map<uint32_t, int> scoreSum;
    for (const auto& name : names) {
        auto it = binds.find(name);
        if (it == binds.end()) continue;
        std::set<uint32_t> ownersSeenForToken;
        for (const auto& c : it->second.candidates) {
            if (!ownersSeenForToken.insert(c.ownerRva).second) continue;
            tokenCount[c.ownerRva] += 1;
            scoreSum[c.ownerRva] += c.score;
        }
    }
    uint32_t best = 0;
    int bestCount = 0;
    int bestScore = std::numeric_limits<int>::min();
    for (const auto& kv : tokenCount) {
        const int cnt = kv.second;
        const int sc = scoreSum[kv.first];
        if (cnt > bestCount || (cnt == bestCount && sc > bestScore)) {
            best = kv.first;
            bestCount = cnt;
            bestScore = sc;
        }
    }
    if (bestCount < minUniqueTokens) return 0;
    return best;
}

void SelectBind(BindResult& r, uint32_t preferredOwner) {
    r.selectedOk = false;
    if (r.candidates.empty()) return;
    if (preferredOwner) {
        for (const auto& c : r.candidates) {
            if (c.ownerRva == preferredOwner) {
                r.selected = c;
                r.selectedOk = true;
                return;
            }
        }
    }
    r.selected = r.candidates.front();
    r.selectedOk = true;
}

int DetectThisRegister(uintptr_t fn) {
    std::array<uint8_t, 128> b{};
    if (!SafeCopy(fn, b.data(), b.size())) return 1; // ECX fallback
    for (size_t i = 0; i + 1 < b.size(); ++i) {
        if (b[i] == 0x8B && b[i+1] == 0xD9) return 3; // EBX <- ECX
        if (b[i] == 0x8B && b[i+1] == 0xF1) return 6; // ESI <- ECX
        if (b[i] == 0x8B && b[i+1] == 0xF9) return 7; // EDI <- ECX
    }
    return 1;
}

bool DecodeThisDisp(const uint8_t* p, int thisReg, int requiredReg, int& disp, size_t& len) {
    const uint8_t op = p[0];
    if (!(op == 0x8B || op == 0x89 || op == 0x3B || op == 0x39 ||
          op == 0x80 || op == 0x81 || op == 0x83 || op == 0xC6 ||
          op == 0xC7 || op == 0xFF)) return false;
    const uint8_t modrm = p[1];
    const int mod = (modrm >> 6) & 3;
    const int reg = (modrm >> 3) & 7;
    const int rm = modrm & 7;
    if (rm != thisReg) return false;
    if (requiredReg >= 0 && reg != requiredReg) return false;
    if (mod == 0) {
        if (rm == 5) return false;
        disp = 0;
        len = 2;
        return true;
    }
    if (mod == 1) {
        disp = static_cast<int8_t>(p[2]);
        len = 3;
        return true;
    }
    if (mod == 2) {
        int32_t d = 0;
        std::memcpy(&d, p + 2, 4);
        disp = d;
        len = 6;
        return true;
    }
    return false;
}

std::map<int,int> CollectThisOffsets(uintptr_t fn, int thisReg, size_t scan = 384) {
    std::vector<uint8_t> b(scan);
    if (!SafeCopy(fn, b.data(), b.size())) return {};
    std::map<int,int> counts;
    for (size_t i = 0; i + 6 < b.size(); ++i) {
        int disp = 0; size_t len = 0;
        if (DecodeThisDisp(b.data() + i, thisReg, -1, disp, len) && disp >= 0 && disp <= 0x200) {
            counts[disp]++;
        }
    }
    return counts;
}

int FindArgumentCompareOffset(uintptr_t fn, int thisReg) {
    std::vector<uint8_t> b(384);
    if (!SafeCopy(fn, b.data(), b.size())) return -1;
    for (size_t i = 0; i + 3 < b.size(); ++i) {
        if (!(b[i] == 0x8B && b[i+1] == 0x45 && b[i+2] == 0x08)) continue; // mov eax,[ebp+8]
        const size_t end = std::min(b.size(), i + 64);
        for (size_t j = i + 3; j + 6 < end; ++j) {
            if (b[j] != 0x3B) continue; // cmp r32,r/m32
            int disp = 0; size_t len = 0;
            if (DecodeThisDisp(b.data() + j, thisReg, 0, disp, len) && disp >= 0 && disp <= 0x40) {
                return disp;
            }
        }
    }
    return -1;
}

int FindProtocolCandidate(uintptr_t attack, int attackReg,
                          uintptr_t follow, int followReg,
                          int localOff, int attackOff, int followOff) {
    auto a = CollectThisOffsets(attack, attackReg);
    auto f = CollectThisOffsets(follow, followReg);
    int best = -1;
    int bestScore = -1;
    for (const auto& kv : a) {
        const int off = kv.first;
        if (off == localOff || off == attackOff || off == followOff || off > 0x40) continue;
        auto it = f.find(off);
        if (it == f.end()) continue;
        const int score = kv.second + it->second;
        if (score > bestScore) { best = off; bestScore = score; }
    }
    return best;
}

struct GameLayout {
    int localPlayer = -1;
    int attackTarget = -1;
    int followTarget = -1;
    int protocolCandidate = -1;
    int attackThisReg = -1;
    int followThisReg = -1;
    bool inferred = false;
};

GameLayout InferGameLayout(const ModuleView& m, uint32_t attackRva, uint32_t followRva) {
    GameLayout g;
    if (!attackRva || !followRva) return g;
    const uintptr_t attack = m.base + attackRva;
    const uintptr_t follow = m.base + followRva;
    g.attackThisReg = DetectThisRegister(attack);
    g.followThisReg = DetectThisRegister(follow);
    g.attackTarget = FindArgumentCompareOffset(attack, g.attackThisReg);
    g.followTarget = FindArgumentCompareOffset(follow, g.followThisReg);

    const auto a = CollectThisOffsets(attack, g.attackThisReg);
    const auto f = CollectThisOffsets(follow, g.followThisReg);
    if (a.count(0) && f.count(0)) g.localPlayer = 0;
    // A static compare against [this+0] inside attack can be the LocalPlayer, not
    // the AttackTarget. Never promote overlapping semantic fields offline; let
    // the runtime transition resolve the target field.
    if (g.attackTarget == g.localPlayer) g.attackTarget = -1;
    if (g.followTarget == g.localPlayer) g.followTarget = -1;
    g.protocolCandidate = FindProtocolCandidate(attack, g.attackThisReg, follow, g.followThisReg,
                                                g.localPlayer, g.attackTarget, g.followTarget);
    g.inferred = g.localPlayer >= 0 && g.attackTarget >= 0 && g.followTarget >= 0 &&
                 g.attackTarget != g.followTarget;
    return g;
}

struct PositionCandidate {
    int offset = -1;
    uint32_t x = 0;
    uint32_t y = 0;
    uint8_t z = 0;
    int score = 0;
};

std::vector<PositionCandidate> FindPositionCandidates(uintptr_t player) {
    std::vector<PositionCandidate> out;
    for (int off = 0; off <= 0x100; off += 4) {
        uint32_t x = 0, y = 0; uint8_t z = 0;
        if (!SafeRead(player + off, x) || !SafeRead(player + off + 4, y) || !SafeRead(player + off + 8, z)) continue;
        if (x == 0 || y == 0 || x > 65535 || y > 65535 || z > 15) continue;
        PositionCandidate c;
        c.offset = off; c.x = x; c.y = y; c.z = z;
        c.score = 10;
        if (x >= 100 && y >= 100) c.score += 4;
        if (x >= 1000 && y >= 1000 && x <= 50000 && y <= 50000) c.score += 4;
        if (off <= 0x80) c.score += 2;
        out.push_back(c);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.offset < b.offset;
    });
    if (out.size() > 16) out.resize(16);
    return out;
}

bool CaptureWindow(uintptr_t base, std::vector<uint8_t>& out, size_t size) {
    out.resize(size);
    return SafeCopy(base, out.data(), out.size());
}

uint32_t ReadU32From(const std::vector<uint8_t>& b, size_t off) {
    uint32_t v = 0;
    if (off + 4 <= b.size()) std::memcpy(&v, b.data() + off, 4);
    return v;
}

struct RuntimePositionValidation {
    int x = -1, y = -1, z = -1, direction = -1;
    bool ran = false;
    bool xyzPromotedByAdjacency = false;
    std::vector<int> xCandidates;
    std::vector<int> yCandidates;
    std::vector<int> zCandidates;
    std::vector<int> directionCandidates;
};

char AskYesNo(const std::string& text) {
    probe_console::ui() << text << " [S/N]: " << std::flush;
    std::string s;
    std::getline(std::cin, s);
    if (s.empty()) return 'n';
    return static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
}

void WaitEnter(const std::string& text) {
    probe_console::ui() << text << std::flush;
    std::string dummy;
    std::getline(std::cin, dummy);
}

std::vector<int> DiffU32PlusMinusOne(const std::vector<uint8_t>& a,
                                     const std::vector<uint8_t>& b) {
    std::vector<int> out;
    const size_t n = std::min(a.size(), b.size());
    for (size_t off = 0; off + 4 <= n; off += 4) {
        const uint32_t va = ReadU32From(a, off);
        const uint32_t vb = ReadU32From(b, off);
        if (va == 0 || vb == 0 || va > 65535 || vb > 65535) continue;
        const int64_t d = static_cast<int64_t>(vb) - static_cast<int64_t>(va);
        if (d == 1 || d == -1) out.push_back(static_cast<int>(off));
    }
    return out;
}

std::vector<int> DiffBytePlusMinusOneSmall(const std::vector<uint8_t>& a,
                                           const std::vector<uint8_t>& b,
                                           uint8_t maxVal) {
    std::vector<int> out;
    const size_t n = std::min(a.size(), b.size());
    for (size_t off = 0; off < n; ++off) {
        const int va = a[off], vb = b[off];
        if (va > maxVal || vb > maxVal) continue;
        if (std::abs(vb - va) == 1) out.push_back(static_cast<int>(off));
    }
    return out;
}

RuntimePositionValidation RunPositionValidation(uintptr_t player, const std::vector<PositionCandidate>& structural) {
    RuntimePositionValidation r;
    if (AskYesNo("Executar validacao runtime de X/Y/Z/Direction agora?") != 's') return r;
    r.ran = true;
    const size_t window = 0x100;
    std::vector<uint8_t> a,b,c,d,e;
    if (!CaptureWindow(player, a, window)) return r;

    WaitEnter("Mova EXATAMENTE 1 sqm horizontal (direita/esquerda), depois ENTER... ");
    if (!CaptureWindow(player, b, window)) return r;
    auto xs = DiffU32PlusMinusOne(a,b);
    r.xCandidates = xs;
    std::cout << "[X TRANSITION] candidates=" << xs.size() << "\n";
    for (int off : xs) std::cout << "  +" << HexU32(off) << "  " << ReadU32From(a,off) << " -> " << ReadU32From(b,off) << "\n";
    if (xs.size() == 1) r.x = xs.front();

    WaitEnter("Mova EXATAMENTE 1 sqm vertical (cima/baixo), depois ENTER... ");
    if (!CaptureWindow(player, c, window)) return r;
    auto ys = DiffU32PlusMinusOne(b,c);
    r.yCandidates = ys;
    std::cout << "[Y TRANSITION] candidates=" << ys.size() << "\n";
    for (int off : ys) std::cout << "  +" << HexU32(off) << "  " << ReadU32From(b,off) << " -> " << ReadU32From(c,off) << "\n";
    if (ys.size() == 1) r.y = ys.front();

    if (AskYesNo("Consegue subir/descer exatamente 1 andar?") == 's') {
        WaitEnter("Mude exatamente 1 andar, depois ENTER... ");
        if (CaptureWindow(player, d, window)) {
            auto zs = DiffBytePlusMinusOneSmall(c,d,15);
            r.zCandidates = zs;
            std::cout << "[Z TRANSITION] candidates=" << zs.size() << "\n";
            for (int off : zs) std::cout << "  +" << HexU32(off) << "  " << static_cast<int>(c[off]) << " -> " << static_cast<int>(d[off]) << "\n";
            if (zs.size() == 1) r.z = zs.front();
        }
    } else {
        d = c;
    }

    if (AskYesNo("Consegue virar duas vezes sem andar?") == 's') {
        std::vector<uint8_t> t1, t2, t3;
        if (CaptureWindow(player, t1, window)) {
            WaitEnter("Vire uma vez sem andar e ENTER... ");
            CaptureWindow(player, t2, window);
            WaitEnter("Vire novamente sem andar e ENTER... ");
            CaptureWindow(player, t3, window);
            std::vector<int> dirs;
            for (size_t off = 0; off + 4 <= window; off += 4) {
                const uint32_t v1 = ReadU32From(t1,off);
                const uint32_t v2 = ReadU32From(t2,off);
                const uint32_t v3 = ReadU32From(t3,off);
                if (v1 <= 7 && v2 <= 7 && v3 <= 7 && v1 != v2 && v2 != v3) dirs.push_back(static_cast<int>(off));
            }
            r.directionCandidates = dirs;
            std::cout << "[DIRECTION TRANSITION] candidates=" << dirs.size() << "\n";
            for (int off : dirs) {
                std::cout << "  +" << HexU32(off) << "  " << ReadU32From(t1,off)
                          << " -> " << ReadU32From(t2,off) << " -> " << ReadU32From(t3,off) << "\n";
            }
            if (dirs.size() == 1) r.direction = dirs.front();
        }
    }

    // V2: resolve ambiguous X/Z using an actual contiguous Position shape.
    // A candidate is promoted only when the same triple exists structurally and
    // transition evidence matches X at +0, Y at +4 and Z at +8.
    int bestBase = -1;
    int bestScore = -1;
    for (const auto& pc : structural) {
        const int bx = pc.offset;
        const int by = pc.offset + 4;
        const int bz = pc.offset + 8;
        int score = 0;
        if (std::find(r.xCandidates.begin(), r.xCandidates.end(), bx) != r.xCandidates.end()) score += 3;
        if (std::find(r.yCandidates.begin(), r.yCandidates.end(), by) != r.yCandidates.end()) score += 4;
        if (std::find(r.zCandidates.begin(), r.zCandidates.end(), bz) != r.zCandidates.end()) score += 4;
        if (r.x == bx) score += 2;
        if (r.y == by) score += 2;
        if (r.z == bz) score += 2;
        if (score > bestScore) { bestScore = score; bestBase = bx; }
    }
    if (bestBase >= 0 && bestScore >= 9) {
        const int bx = bestBase, by = bestBase + 4, bz = bestBase + 8;
        const bool xEvidence = std::find(r.xCandidates.begin(), r.xCandidates.end(), bx) != r.xCandidates.end();
        const bool yEvidence = std::find(r.yCandidates.begin(), r.yCandidates.end(), by) != r.yCandidates.end();
        const bool zEvidence = std::find(r.zCandidates.begin(), r.zCandidates.end(), bz) != r.zCandidates.end();
        if (xEvidence && yEvidence && zEvidence) {
            if (r.x < 0) r.x = bx;
            if (r.y < 0) r.y = by;
            if (r.z < 0) r.z = bz;
            r.xyzPromotedByAdjacency = true;
            std::cout << "[XYZ ADJACENCY] promoted contiguous Position at +" << HexU32(bx)
                      << " => X +" << HexU32(bx) << " Y +" << HexU32(by)
                      << " Z +" << HexU32(bz) << "\n";
        }
    }
    return r;
}

struct MapCenterCandidate {
    int offset = -1;
};

std::vector<MapCenterCandidate> FindMapCenter(uintptr_t mapOwner,
                                              uintptr_t player,
                                              const RuntimePositionValidation& p) {
    std::vector<MapCenterCandidate> out;
    if (!mapOwner || !player || p.x < 0 || p.y < 0 || p.z < 0) return out;
    uint32_t x = 0, y = 0; uint8_t z = 0;
    if (!SafeRead(player + p.x, x) || !SafeRead(player + p.y, y) || !SafeRead(player + p.z, z)) return out;
    for (int off = 0; off <= 0x800; off += 4) {
        uint32_t mx=0,my=0; uint8_t mz=0;
        if (!SafeRead(mapOwner+off,mx) || !SafeRead(mapOwner+off+4,my) || !SafeRead(mapOwner+off+8,mz)) continue;
        if (mx==x && my==y && mz==z) out.push_back({off});
    }
    return out;
}

struct ValueHit { std::string type; int offset = 0; std::string value; };

struct SelectedField {
    std::string type;
    int offset = -1;
    std::string status = "pending";
};

struct CreatureLayout {
    int x = -1;
    int y = -1;
    int z = -1;
    int direction = -1;
    std::string positionStatus = "pending";

    int nameOffset = -1;
    std::string nameKind;
    std::string nameStatus = "pending";
    int nameMirrorOffset = -1;
    std::string nameMirrorKind;
    std::string nameMirrorStatus = "pending";

    SelectedField hpPercent;
    SelectedField id;

    // V18: direct semantic accessors from the CURRENT build.
    // These are persisted only when the exact Lua/OTC token resolves to a
    // direct field getter (and, when available, the matching setter agrees).
    SelectedField speed;
    SelectedField baseSpeed;
    SelectedField skull;
    SelectedField shield;
    int outfitOffset = -1;
    std::string outfitStatus = "pending";

    // V11: Creature/LocalPlayer light discovery. The runtime test is reversible
    // and read-only. If two adjacent bytes survive both cycles but cannot be
    // labeled safely, they are persisted as candidates instead of guessed.
    SelectedField lightLevel;
    SelectedField lightColor;
    int lightCandidateA = -1;
    int lightCandidateB = -1;
    std::string lightStatus = "pending";
};

struct WorldLightLayout {
    // V13: UIMap -> MapView contract. The offsets are extracted from the
    // current module's own getter/setter wrappers; offline builds are used only
    // to define instruction shapes, never as an RVA/offset oracle.
    uint32_t ownerRva = 0; // kept for backwards profile compatibility
    uint32_t setDrawLightsRva = 0;
    uint32_t isDrawingLightsRva = 0;
    uint32_t setMinimumAmbientLightRva = 0;
    uint32_t getMinimumAmbientLightRva = 0;
    int uiMapToMapView = -1;
    SelectedField drawLights;
    SelectedField minimumAmbient;
    int candidateA = -1;
    int candidateB = -1;
    std::string staticContractStatus = "pending";
    std::string status = "pending";
};

bool SameHit(const ValueHit& a, const ValueHit& b) {
    return a.type == b.type && a.offset == b.offset;
}

int TypeWidth(const std::string& t) {
    if (t == "double") return 8;
    if (t == "float" || t == "u32") return 4;
    if (t == "u16") return 2;
    if (t == "u8") return 1;
    return 0;
}

std::vector<ValueHit> IntersectHits(const std::vector<ValueHit>& a, const std::vector<ValueHit>& b) {
    std::vector<ValueHit> out;
    for (const auto& x : a) {
        for (const auto& y : b) {
            if (SameHit(x,y)) { out.push_back(y); break; }
        }
    }
    return out;
}

std::vector<ValueHit> FindNumericValue(uintptr_t player, double target, int maxOff = 0x1000) {
    std::vector<ValueHit> hits;
    for (int off = 0; off <= maxOff - 8; off += 4) {
        double d = 0;
        if (SafeRead(player+off,d) && std::isfinite(d) && std::fabs(d-target) <= 1e-7) {
            hits.push_back({"double",off, std::to_string(d)});
        }
        float f = 0;
        if (SafeRead(player+off,f) && std::isfinite(f) && std::fabs(static_cast<double>(f)-target) <= 1e-5) {
            hits.push_back({"float",off, std::to_string(f)});
        }
        if (target >= 0 && target <= 4294967295.0 && std::floor(target)==target) {
            uint32_t u = 0;
            if (SafeRead(player+off,u) && u == static_cast<uint32_t>(target)) {
                hits.push_back({"u32",off,std::to_string(u)});
            }
            if (target <= 65535.0) {
                uint16_t u16 = 0;
                if (SafeRead(player+off,u16) && u16 == static_cast<uint16_t>(target)) {
                    hits.push_back({"u16",off,std::to_string(u16)});
                }
            }
            if (target <= 255.0) {
                uint8_t u8 = 0;
                if (SafeRead(player+off,u8) && u8 == static_cast<uint8_t>(target)) {
                    hits.push_back({"u8",off,std::to_string(static_cast<unsigned>(u8))});
                }
            }
        }
        if (hits.size() > 64) break;
    }
    return hits;
}


double AskDouble(const std::string& label);

bool ReadTypedValue(uintptr_t base, const std::string& type, int off, double& out) {
    if (type == "double") { double v=0; if (!SafeRead(base+off,v) || !std::isfinite(v)) return false; out=v; return true; }
    if (type == "float") { float v=0; if (!SafeRead(base+off,v) || !std::isfinite(v)) return false; out=v; return true; }
    if (type == "u32") { uint32_t v=0; if (!SafeRead(base+off,v)) return false; out=static_cast<double>(v); return true; }
    if (type == "u16") { uint16_t v=0; if (!SafeRead(base+off,v)) return false; out=static_cast<double>(v); return true; }
    if (type == "u8") { uint8_t v=0; if (!SafeRead(base+off,v)) return false; out=static_cast<double>(v); return true; }
    return false;
}

bool Nearly(double a, double b, double eps=1e-5) {
    const double scale = std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
    return std::fabs(a-b) <= eps*scale;
}

struct ManaPairResult {
    SelectedField current;
    SelectedField maximum;
    std::string status = "pending";
};

ManaPairResult ResolveCurrentMaxPairTransition(uintptr_t base,
                                               const std::vector<ValueHit>& currentHits,
                                               const std::vector<ValueHit>& maxHits,
                                               double oldCurrent,
                                               double oldMax) {
    ManaPairResult out;
    struct Pair { ValueHit cur; ValueHit mx; };
    std::vector<Pair> pairs;
    for (const auto& c : currentHits) {
        const int w = TypeWidth(c.type);
        if (!w) continue;
        for (const auto& m : maxHits) {
            if (m.type == c.type && m.offset == c.offset + w) pairs.push_back({c,m});
        }
    }
    std::cout << "[MANA PAIR] adjacent candidates=" << pairs.size() << "\n";
    for (const auto& p : pairs) std::cout << "  " << p.cur.type << " +" << HexU32(p.cur.offset)
                                          << " / +" << HexU32(p.mx.offset) << "\n";
    if (pairs.empty()) return out;

    for (int attempt=1; attempt<=2; ++attempt) {
        std::cout << "[MANA PAIR] tentativa " << attempt << "/2\n"
                  << "Gaste mana SEM upar, trocar equipamento ou alterar MaxMana.\n"
                  << "O ideal e ManaAtual ficar MENOR que MaxMana.\n";
        WaitEnter("Depois da mudanca pressione ENTER... ");
        const double newCurrent = AskDouble("Mana atual agora");
        const double newMax = AskDouble("Mana maxima agora");
        if (!Nearly(newMax, oldMax)) {
            std::cout << "[RETRY] MaxMana mudou de " << oldMax << " para " << newMax
                      << ". Esta transicao nao separa Current/Max com seguranca.\n";
            oldCurrent = newCurrent;
            oldMax = newMax;
            continue;
        }
        std::vector<Pair> matched;
        for (const auto& p : pairs) {
            double cv=0,mv=0;
            if (!ReadTypedValue(base,p.cur.type,p.cur.offset,cv) || !ReadTypedValue(base,p.mx.type,p.mx.offset,mv)) continue;
            if (Nearly(cv,newCurrent) && Nearly(mv,newMax) && !Nearly(newCurrent,oldCurrent)) matched.push_back(p);
        }
        std::cout << "[MANA PAIR TRANSITION] candidates=" << matched.size() << "\n";
        for (const auto& p : matched) std::cout << "  " << p.cur.type << " current +" << HexU32(p.cur.offset)
                                               << " max +" << HexU32(p.mx.offset) << "\n";
        if (matched.size()==1) {
            out.current={matched[0].cur.type,matched[0].cur.offset,"runtime_currentmax_pair"};
            out.maximum={matched[0].mx.type,matched[0].mx.offset,"runtime_currentmax_pair"};
            out.status="confirmed";
            return out;
        }
        oldCurrent = newCurrent;
    }
    return out;
}


ManaPairResult ResolveManaPairByLevelUp(uintptr_t base,
                                        const std::vector<ValueHit>& currentHits,
                                        const std::vector<ValueHit>& maxHits,
                                        double oldCurrent,
                                        double oldMax,
                                        const SelectedField& levelField,
                                        double oldLevel) {
    ManaPairResult out;
    struct Pair { ValueHit cur; ValueHit mx; int score = 0; };
    std::vector<Pair> pairs;
    for (const auto& c : currentHits) {
        const int w = TypeWidth(c.type);
        if (!w) continue;
        for (const auto& m : maxHits) {
            if (m.type != c.type || m.offset != c.offset + w) continue;
            int score = 0;
            if (c.type == "double") score += 6;
            else if (c.type == "float") score += 4;
            else if (c.type == "u32") score += 2;
            if (levelField.offset >= 0 && c.offset > levelField.offset && c.offset <= levelField.offset + 0x40) score += 5;
            if (levelField.offset >= 0 && c.offset == levelField.offset + 0x10) score += 3;
            pairs.push_back({c,m,score});
        }
    }
    std::cout << "[MANA LOW-LEVEL] adjacent pair candidates=" << pairs.size() << "\n";
    for (const auto& p : pairs) {
        std::cout << "  score=" << p.score << " " << p.cur.type << " +" << HexU32(p.cur.offset)
                  << " / +" << HexU32(p.mx.offset) << "\n";
    }
    if (pairs.empty()) return out;

    std::cout << "[MANA LOW-LEVEL] Este personagem nao precisa gastar mana.\n"
              << "Valide pelo LEVEL UP: Current e Max podem crescer juntos.\n"
              << "Suba EXATAMENTE 1 level sem relogar.\n";
    WaitEnter("Depois do level up pressione ENTER... ");
    const double newLevel = AskDouble("Level agora");
    const double newCurrent = AskDouble("Mana atual agora");
    const double newMax = AskDouble("Mana maxima agora");

    if (newLevel <= oldLevel) {
        std::cout << "[MANA LOW-LEVEL] Level nao aumentou; teste nao promovido.\n";
        return out;
    }
    if (Nearly(newCurrent, oldCurrent) && Nearly(newMax, oldMax)) {
        std::cout << "[MANA LOW-LEVEL] Mana nao mudou; teste nao promoveu o par.\n";
        return out;
    }
    if (levelField.offset >= 0) {
        double memLevel = 0;
        if (ReadTypedValue(base, levelField.type, levelField.offset, memLevel) && !Nearly(memLevel,newLevel)) {
            std::cout << "[MANA LOW-LEVEL] Campo Level conhecido nao acompanhou o valor informado.\n";
            return out;
        }
    }

    std::vector<Pair> matched;
    for (auto p : pairs) {
        double cv=0,mv=0;
        if (!ReadTypedValue(base,p.cur.type,p.cur.offset,cv) || !ReadTypedValue(base,p.mx.type,p.mx.offset,mv)) continue;
        if (Nearly(cv,newCurrent) && Nearly(mv,newMax)) matched.push_back(p);
    }
    std::sort(matched.begin(), matched.end(), [](const Pair& a, const Pair& b){ return a.score > b.score; });
    std::cout << "[MANA LEVEL-UP PAIR] candidates=" << matched.size() << "\n";
    for (const auto& p : matched) {
        std::cout << "  score=" << p.score << " " << p.cur.type
                  << " first +" << HexU32(p.cur.offset) << " second +" << HexU32(p.mx.offset) << "\n";
    }
    if (matched.empty()) return out;
    const int top = matched.front().score;
    size_t topCount = 0;
    for (const auto& p : matched) if (p.score == top) ++topCount;
    if (topCount == 1) {
        const auto& p = matched.front();
        // When Current==Max before and after, runtime proves the adjacent pair but
        // cannot independently prove which member is Current. OTC layout convention
        // is lower=Current, upper=Max; keep provenance explicit until a differential
        // sample exists in a later level/build.
        const bool orderDifferential = !Nearly(newCurrent,newMax) || !Nearly(oldCurrent,oldMax);
        const std::string st = orderDifferential
            ? "runtime_levelup_pair_differential"
            : "runtime_levelup_pair_order_structural_pending_differential";
        out.current = {p.cur.type,p.cur.offset,st};
        out.maximum = {p.mx.type,p.mx.offset,st};
        out.status = orderDifferential ? "confirmed" : "pair_confirmed_order_structural";
    }
    return out;
}

struct ScaledCapacityPair {
    SelectedField first;
    SelectedField second;
    double scale = 1.0;
    std::string status = "pending";
};

ScaledCapacityPair FindScaledCapacityPair(uintptr_t base, double displayValue, int maxOff=0x1000) {
    ScaledCapacityPair out;
    struct C { int a=-1,b=-1,score=0; double scale=1.0; };
    std::vector<C> cands;
    const std::vector<double> scales={1.0,0.1,0.01,10.0,100.0};
    for (double sc : scales) {
        const double target = displayValue * sc;
        std::vector<int> offs;
        for (int off=0; off+8<=maxOff; off+=4) {
            double v=0;
            if (SafeRead(base+off,v) && std::isfinite(v) && Nearly(v,target,1e-5)) offs.push_back(off);
        }
        for (int a : offs) {
            const int b = a + 8;
            if (std::find(offs.begin(),offs.end(),b)==offs.end()) continue;
            int score=6;
            if (sc==0.01) score+=4;
            if (a>=0x300 && a<=0x700) score+=3;
            cands.push_back({a,b,score,sc});
        }
    }
    std::sort(cands.begin(),cands.end(),[](const C& x,const C& y){return x.score>y.score;});
    std::cout << "[CAPACITY PAIR] scaled adjacent candidates=" << cands.size() << "\n";
    for(size_t i=0;i<cands.size() && i<12;++i) {
        std::cout << "  score=" << cands[i].score << " double +" << HexU32(cands[i].a)
                  << " / +" << HexU32(cands[i].b) << " scale=" << cands[i].scale << "\n";
    }
    if(cands.empty()) return out;
    const int top=cands.front().score;
    size_t nTop=0; for(const auto& c:cands) if(c.score==top) ++nTop;
    if(nTop==1) {
        const auto& c=cands.front();
        out.first={"double",c.a,"structural_scaled_pair_pending_weight_split"};
        out.second={"double",c.b,"structural_scaled_pair_pending_weight_split"};
        out.scale=c.scale;
        out.status="pair_found";
    }
    return out;
}

struct CapacityTransitionCandidate {
    std::string type;
    int offset = -1;
    double before = 0;
    double after = 0;
    double scale = 1.0;
    int score = 0;
};

double ReadTypedFromBuffer(const std::vector<uint8_t>& b, int off, const std::string& type, bool& ok) {
    ok=false;
    if (off < 0 || off >= static_cast<int>(b.size())) return 0;
    if (type=="double" && off+8 <= static_cast<int>(b.size())) { double v=0; std::memcpy(&v,b.data()+off,8); if(std::isfinite(v)){ok=true;return v;} }
    if (type=="float" && off+4 <= static_cast<int>(b.size())) { float v=0; std::memcpy(&v,b.data()+off,4); if(std::isfinite(v)){ok=true;return v;} }
    if (type=="u32" && off+4 <= static_cast<int>(b.size())) { uint32_t v=0; std::memcpy(&v,b.data()+off,4); ok=true;return v; }
    if (type=="u16" && off+2 <= static_cast<int>(b.size())) { uint16_t v=0; std::memcpy(&v,b.data()+off,2); ok=true;return v; }
    if (type=="u8" && off+1 <= static_cast<int>(b.size())) { ok=true;return b[off]; }
    return 0;
}

struct CapacityTransitionResult {
    SelectedField canonical;
    bool mirrorPairConfirmed = false;
    bool mirrorPairConfirmedTwice = false;
    double newDisplay = 0.0;
};

CapacityTransitionResult ResolveCapacityTransitionV6(uintptr_t base,
                                          double oldDisplay,
                                          const ScaledCapacityPair& knownPair,
                                          int maxOff=0x1000) {
    CapacityTransitionResult result;
    SelectedField out;
    std::vector<uint8_t> before, after;
    if (!CaptureWindow(base,before,maxOff)) return result;
    std::cout << "[CAPACITY V12] snapshot capturado com Capacity atual=" << oldDisplay << ". Altere SOMENTE o peso/capacidade.\n";
    WaitEnter("Depois da mudanca pressione ENTER... ");
    const double newDisplay=AskDouble("Capacity agora");
    if (!CaptureWindow(base,after,maxOff)) return result;
    result.newDisplay = newDisplay;
    const double displayDelta=newDisplay-oldDisplay;
    const std::vector<std::string> types={"double","float","u32","u16","u8"};
    const std::vector<double> scales={1.0,10.0,100.0,0.1,0.01};
    std::vector<CapacityTransitionCandidate> cand;
    for (const auto& type:types) {
        const int step = (type=="u8"?1:(type=="u16"?2:4));
        const int width=TypeWidth(type);
        for (int off=0; off+width<=maxOff; off+=step) {
            bool ok1=false,ok2=false;
            const double b=ReadTypedFromBuffer(before,off,type,ok1);
            const double a=ReadTypedFromBuffer(after,off,type,ok2);
            if(!ok1||!ok2||Nearly(a,b)) continue;
            int best=0; double bestScale=1.0;
            for(double sc:scales) {
                int score=0;
                if(Nearly(b,oldDisplay*sc,1e-4) && Nearly(a,newDisplay*sc,1e-4)) score+=10;
                if(Nearly((a-b),displayDelta*sc,1e-4)) score+=5;
                if(Nearly((a-b),-displayDelta*sc,1e-4)) score+=2; // used-capacity inverse candidate
                if(score>best){best=score;bestScale=sc;}
            }
            if(best>0) {
                if(type=="double") best+=3; else if(type=="float") best+=2; else if(type=="u32") best+=1;
                // Stats cluster hint only; never enough by itself to promote.
                if(off>=0x400 && off<=0x600) best+=1;
                // If an adjacent scaled pair was identified immediately before the
                // weight-only transition, prefer the member that actually follows
                // the displayed Capacity. This is runtime evidence, not a hardcoded RVA.
                if(knownPair.first.offset>=0 && type==knownPair.first.type && off==knownPair.first.offset) best+=8;
                if(knownPair.second.offset>=0 && type==knownPair.second.type && off==knownPair.second.offset) best+=8;
                cand.push_back({type,off,b,a,bestScale,best});
            }
        }
    }
    std::sort(cand.begin(),cand.end(),[](const auto&a,const auto&b){return a.score>b.score;});
    std::cout << "[CAPACITY TRANSITION V9] candidates=" << cand.size() << "\n";
    for(size_t i=0;i<cand.size() && i<16;++i) {
        const auto& c=cand[i];
        std::cout << "  score=" << c.score << " " << c.type << " +" << HexU32(c.offset)
                  << " " << c.before << " -> " << c.after << " scale=" << c.scale << "\n";
    }
    if(!cand.empty()) {
        const int top=cand.front().score;
        size_t nTop=0; for(const auto& c:cand) if(c.score==top) ++nTop;
        if(nTop==1 && top>=10) {
            out={cand.front().type,cand.front().offset,"runtime_scaled_transition"};
            result.canonical = out;
        }

        // V9: if both members of the structural scaled pair are tied at the top
        // and each exactly follows the displayed Capacity, classify them as a
        // runtime-confirmed mirrored pair instead of treating the tie as failure.
        if(knownPair.first.offset>=0 && knownPair.second.offset>=0) {
            const CapacityTransitionCandidate* a=nullptr;
            const CapacityTransitionCandidate* b=nullptr;
            for(const auto& c:cand) {
                if(c.type==knownPair.first.type && c.offset==knownPair.first.offset) a=&c;
                if(c.type==knownPair.second.type && c.offset==knownPair.second.offset) b=&c;
            }
            if(a && b && a->score==b->score && a->score==top &&
               Nearly(a->before, oldDisplay*knownPair.scale, 1e-4) &&
               Nearly(a->after, newDisplay*knownPair.scale, 1e-4) &&
               Nearly(b->before, oldDisplay*knownPair.scale, 1e-4) &&
               Nearly(b->after, newDisplay*knownPair.scale, 1e-4)) {
                result.mirrorPairConfirmed = true;
                std::cout << "[CAPACITY MIRROR V9] pair +" << HexU32(knownPair.first.offset)
                          << " / +" << HexU32(knownPair.second.offset)
                          << " acompanhou exatamente " << oldDisplay << " -> " << newDisplay
                          << " (memory scale=" << knownPair.scale << ")\n";

                if(AskYesNo("Confirmar o par espelhado com uma SEGUNDA mudanca de peso sem level-up?")=='s') {
                    std::vector<uint8_t> before2, after2;
                    if(CaptureWindow(base,before2,maxOff)) {
                        WaitEnter("Altere novamente SOMENTE o peso/capacidade e depois ENTER... ");
                        const double display2=AskDouble("Capacity agora (2a transicao)");
                        if(CaptureWindow(base,after2,maxOff)) {
                            bool a1=false,a2=false,b1=false,b2=false;
                            const double va1=ReadTypedFromBuffer(before2,knownPair.first.offset,knownPair.first.type,a1);
                            const double va2=ReadTypedFromBuffer(after2, knownPair.first.offset,knownPair.first.type,a2);
                            const double vb1=ReadTypedFromBuffer(before2,knownPair.second.offset,knownPair.second.type,b1);
                            const double vb2=ReadTypedFromBuffer(after2, knownPair.second.offset,knownPair.second.type,b2);
                            if(a1&&a2&&b1&&b2 &&
                               Nearly(va1,newDisplay*knownPair.scale,1e-4) && Nearly(va2,display2*knownPair.scale,1e-4) &&
                               Nearly(vb1,newDisplay*knownPair.scale,1e-4) && Nearly(vb2,display2*knownPair.scale,1e-4)) {
                                result.mirrorPairConfirmedTwice = true;
                                result.newDisplay = display2;
                                std::cout << "[CAPACITY MIRROR V9] segunda transicao confirmou os dois membros.\n";
                            } else {
                                std::cout << "[CAPACITY MIRROR V9] segunda transicao nao confirmou os dois membros.\n";
                            }
                        }
                    }
                }
            }
        }
    }
    return result;
}

int ValidateMapCenterV3(uintptr_t mapOwner,
                        const std::vector<MapCenterCandidate>& candidates,
                        std::string& status) {
    status="pending";
    if(!mapOwner || candidates.empty()) return -1;
    struct S { int off; uint32_t x0=0,y0=0,x1=0,y1=0,x2=0,y2=0; uint8_t z0=0,z1=0,z2=0; };
    std::vector<S> states;
    for(const auto& c:candidates) {
        S s; s.off=c.offset;
        if(SafeRead(mapOwner+c.offset,s.x0)&&SafeRead(mapOwner+c.offset+4,s.y0)&&SafeRead(mapOwner+c.offset+8,s.z0)) states.push_back(s);
    }
    if(states.empty()) return -1;
    std::cout << "[MAP CENTER V9] validacao por transicao.\n";
    WaitEnter("Mova EXATAMENTE 1 sqm horizontal e depois ENTER... ");
    std::vector<S> h;
    for(auto s:states) {
        if(!SafeRead(mapOwner+s.off,s.x1)||!SafeRead(mapOwner+s.off+4,s.y1)||!SafeRead(mapOwner+s.off+8,s.z1)) continue;
        const bool xstep=(s.x1+1==s.x0)||(s.x0+1==s.x1);
        if(xstep && s.y1==s.y0 && s.z1==s.z0) h.push_back(s);
    }
    std::cout << "[MAP CENTER H] candidates=" << h.size() << "\n";
    for(const auto&s:h) std::cout << "  +" << HexU32(s.off) << " X " << s.x0 << " -> " << s.x1 << "\n";
    if(h.empty()) return -1;
    WaitEnter("Mova EXATAMENTE 1 sqm vertical e depois ENTER... ");
    std::vector<S> v;
    for(auto s:h) {
        if(!SafeRead(mapOwner+s.off,s.x2)||!SafeRead(mapOwner+s.off+4,s.y2)||!SafeRead(mapOwner+s.off+8,s.z2)) continue;
        const bool ystep=(s.y2+1==s.y1)||(s.y1+1==s.y2);
        if(ystep && s.x2==s.x1 && s.z2==s.z1) v.push_back(s);
    }
    std::cout << "[MAP CENTER V] candidates=" << v.size() << "\n";
    for(const auto&s:v) std::cout << "  +" << HexU32(s.off) << " Y " << s.y1 << " -> " << s.y2 << "\n";
    if(v.size()!=1) return -1;
    int chosen=v.front().off;
    status="runtime_xy_transition";
    if(AskYesNo("Validar Map.CenterZ mudando exatamente 1 andar?")=='s') {
        for(int attempt=1; attempt<=2; ++attempt) {
            uint8_t zb=0,za=0;
            SafeRead(mapOwner+chosen+8,zb);
            std::cout << "[MAP CENTER Z] tentativa " << attempt << "/2, Z antes=" << static_cast<unsigned>(zb) << "\n";
            WaitEnter("Mude exatamente 1 andar e depois ENTER... ");
            SafeRead(mapOwner+chosen+8,za);
            std::cout << "[MAP CENTER Z] Z depois=" << static_cast<unsigned>(za) << "\n";
            // Stairs/holes may also move X/Y. Because XY already uniquely confirmed
            // this stage validates only the sibling Z byte at +8.
            if((za+1==zb)||(zb+1==za)) {
                status="runtime_xyz_transition";
                std::cout << "[MAP CENTER Z] confirmado por +/-1 andar.\n";
                break;
            }
            std::cout << "[MAP CENTER Z] nao confirmou nesta tentativa.\n";
            if(attempt==1 && AskYesNo("Tentar Map.CenterZ novamente sem repetir X/Y?")!='s') break;
        }
        if(status!="runtime_xyz_transition")
            std::cout << "[MAP CENTER Z] mantendo confirmacao XY; Z fica pendente.\n";
    }
    return chosen;
}

double AskDouble(const std::string& label);

struct GameRuntimeValidation {
    int attackTarget = -1;
    int followTarget = -1;
    bool attackRan = false;
    bool followRan = false;
};

std::string KnownCreatureClass(uint32_t ptr, const ModuleView& m,
                               const std::map<std::string,RttiResult>& rtti) {
    if (!ptr) return {};
    uint32_t vt = 0;
    if (!SafeRead(ptr, vt) || !m.inModule(vt)) return {};
    const uint32_t vr = m.rva(vt);
    for (const auto& name : {std::string("Monster"),std::string("Player"),std::string("Npc"),std::string("LocalPlayer")}) {
        auto it = rtti.find(name);
        if (it == rtti.end()) continue;
        if (std::find(it->second.vtableRvas.begin(), it->second.vtableRvas.end(), vr) != it->second.vtableRvas.end()) return name;
    }
    return {};
}

std::vector<int> FindGamePointerTransition(const std::vector<uint8_t>& before,
                                           const std::vector<uint8_t>& active,
                                           const std::vector<uint8_t>& after,
                                           const ModuleView& m,
                                           const std::map<std::string,RttiResult>& rtti,
                                           int excludeOffset) {
    std::vector<std::pair<int,int>> scored;
    const size_t n = std::min({before.size(),active.size(),after.size()});
    for (size_t off=0; off+4<=n && off<=0x40; off+=4) {
        if (static_cast<int>(off)==excludeOffset) continue;
        const uint32_t b=ReadU32From(before,off), a=ReadU32From(active,off), e=ReadU32From(after,off);
        if (!a || a==b || a==e) continue;
        int score=2;
        const std::string cls=KnownCreatureClass(a,m,rtti);
        if (!cls.empty()) score+=8;
        if (e==b || e==0) score+=4;
        scored.push_back({static_cast<int>(off),score});
    }
    std::sort(scored.begin(),scored.end(),[](auto a,auto b){return a.second>b.second;});
    std::vector<int> out;
    if (scored.empty()) return out;
    const int top=scored.front().second;
    for (auto [off,score]:scored) if (score==top) out.push_back(off);
    return out;
}

GameRuntimeValidation RunGameTargetValidation(uintptr_t game,
                                              int localPlayerOffset,
                                              const ModuleView& m,
                                              const std::map<std::string,RttiResult>& rtti,
                                              bool validateAttack = true,
                                              bool validateFollow = true) {
    GameRuntimeValidation r;
    const size_t win=0x60;
    std::vector<uint8_t> before,active,after;
    if (validateAttack && AskYesNo("Validar Game.AttackTarget por transicao runtime?")=='s') {
        r.attackRan=true;
        WaitEnter("Pare qualquer ataque e deixe sem target. Depois ENTER... ");
        if (CaptureWindow(game,before,win)) {
            WaitEnter("Ataque uma criatura e mantenha o target. Depois ENTER... ");
            CaptureWindow(game,active,win);
            WaitEnter("Pare o ataque (ESC) e depois ENTER... ");
            CaptureWindow(game,after,win);
            auto c=FindGamePointerTransition(before,active,after,m,rtti,localPlayerOffset);
            std::cout << "[ATTACK TARGET TRANSITION] candidates=" << c.size() << "\n";
            for(int off:c) {
                uint32_t p=ReadU32From(active,off);
                std::cout << "  +" << HexU32(off) << " ptr=" << HexU32(p);
                auto cls=KnownCreatureClass(p,m,rtti); if(!cls.empty()) std::cout << " RTTI=" << cls;
                std::cout << "\n";
            }
            if(c.size()==1) r.attackTarget=c.front();
        }
    }
    if (validateFollow && AskYesNo("Validar Game.FollowTarget por transicao runtime?")=='s') {
        r.followRan=true;
        WaitEnter("Pare qualquer follow e deixe sem follow target. Depois ENTER... ");
        if (CaptureWindow(game,before,win)) {
            WaitEnter("Ative follow em uma criatura/personagem e mantenha. Depois ENTER... ");
            CaptureWindow(game,active,win);
            WaitEnter("Pare o follow e depois ENTER... ");
            CaptureWindow(game,after,win);
            auto c=FindGamePointerTransition(before,active,after,m,rtti,localPlayerOffset);
            std::cout << "[FOLLOW TARGET TRANSITION] candidates=" << c.size() << "\n";
            for(int off:c) {
                uint32_t p=ReadU32From(active,off);
                std::cout << "  +" << HexU32(off) << " ptr=" << HexU32(p);
                auto cls=KnownCreatureClass(p,m,rtti); if(!cls.empty()) std::cout << " RTTI=" << cls;
                std::cout << "\n";
            }
            if(c.size()==1) r.followTarget=c.front();
        }
    }
    return r;
}

SelectedField SelectPreferredUnique(const std::vector<ValueHit>& hits, const std::string& status) {
    SelectedField out;
    const std::vector<std::string> order={"double","float","u32","u16","u8"};
    for(size_t i=0;i<order.size();++i) {
        std::vector<ValueHit> same;
        bool higherPresent=false;
        for(const auto& h:hits) {
            if(h.type==order[i]) same.push_back(h);
            for(size_t j=0;j<i;++j) if(h.type==order[j]) higherPresent=true;
        }
        if(higherPresent) continue;
        if(same.size()==1) { out.type=order[i]; out.offset=same[0].offset; out.status=status; return out; }
        if(same.size()>1) return out;
    }
    if(hits.size()==1) { out.type=hits[0].type; out.offset=hits[0].offset; out.status=status; }
    return out;
}

SelectedField ResolveChangedValueTransition(uintptr_t base,
                                            const std::vector<ValueHit>& beforeHits,
                                            const std::string& label,
                                            int maxOff=0x1000) {
    SelectedField out;
    WaitEnter("Altere " + label + " agora e depois pressione ENTER... ");
    const double newValue=AskDouble(label + " novo");
    auto afterHits=FindNumericValue(base,newValue,maxOff);
    auto both=IntersectHits(beforeHits,afterHits);
    std::cout << "[TRANSITION FIELD] " << label << " candidates=" << both.size() << "\n";
    for(const auto& h:both) std::cout << "  " << h.type << " +" << HexU32(h.offset) << "\n";
    out=SelectPreferredUnique(both,"runtime_transition");
    return out;
}

double AskDouble(const std::string& label) {
    for (;;) {
        probe_console::ui() << label << ": " << std::flush;
        std::string s;
        std::getline(std::cin, s);
        try { return std::stod(s); }
        catch (...) { probe_console::ui() << "Valor invalido.\n"; }
    }
}

struct ActionAbiArgV34 {
    int index = 0;
    std::string semantic;
    std::string abiType;
    std::string status;
    std::string rtti;
    bool hasObservedBool = false;
    bool observedBool = false;
    bool hasRefCountOffset = false;
    uint32_t refCountOffset = 0;
    std::string ownership;
};

struct ActionAbiV34 {
    std::string name;
    uint32_t methodRva = 0;
    uint32_t ownerRva = 0;
    std::string callingConvention = "x86_thiscall_like";
    std::string thisSemantic = "Game";
    std::string thisLocation = "ecx";
    std::string status = "pending";
    std::vector<ActionAbiArgV34> args;
};

struct Report {
    ModuleView module;
    DiskFingerprint fp;
    std::vector<std::string> imports;
    std::string renderer;
    std::map<std::string,RttiResult> rtti;
    std::map<std::string,BindResult> binds;
    uint32_t gameOwnerRva = 0;
    uint32_t mapOwnerRva = 0;
    GameLayout gameLayout;
    GameRuntimeValidation gameRuntime;
    uintptr_t localPlayerPtr = 0;
    uint32_t localPlayerVtableRva = 0;
    bool localPlayerVtableMatch = false;
    std::vector<PositionCandidate> positionCandidates;
    RuntimePositionValidation positionRuntime;
    std::vector<MapCenterCandidate> mapCenters;
    int mapCenterOffset = -1;
    std::string mapCenterStatus = "pending";
    std::map<std::string,std::vector<ValueHit>> valueHits;
    std::map<std::string,SelectedField> selectedFields;
    CreatureLayout creatureLayout;
    WorldLightLayout worldLight;
    uintptr_t creatureTargetPtr = 0;
    std::string creatureTargetClass;
    uint64_t mappedTextHash = 0;
    std::string attackTargetSource = "offline_abi_candidate";
    std::string followTargetSource = "offline_abi_candidate";
    std::string positionSource = "pending";
    std::string checkpoint = "startup";
    bool profileLoaded = false;
    bool profileImportedLegacy = false;
    std::vector<std::string> profileSources;
    std::map<std::string,ActionAbiV34> actionAbi;
    std::map<std::string,std::string> actionAbiRaw;
    std::map<std::string,std::string> guidedEvidence;
};

std::string ProbeDirectory() {
    char path[MAX_PATH * 4]{};
    DWORD n = GetModuleFileNameA(g_self, path, static_cast<DWORD>(sizeof(path)));
    if (!n) return ".";
    std::string s(path, path+n);
    const auto pos = s.find_last_of("\\/");
    return pos == std::string::npos ? "." : s.substr(0,pos);
}

struct ProfileCache {
    bool exactBuild = false;
    uint32_t gameOwnerRva = 0;
    uint32_t mapOwnerRva = 0;
    int localPlayer = -1;
    int attackTarget = -1;
    int followTarget = -1;
    int protocolCandidate = -1;
    std::string attackSource;
    std::string followSource;
    int posX = -1, posY = -1, posZ = -1, direction = -1;
    std::string positionSource;
    int mapCenter = -1;
    std::string mapCenterStatus;
    uint32_t localPlayerVtableRva = 0;
    std::map<std::string,SelectedField> fields;
    CreatureLayout creatureLayout;
    WorldLightLayout worldLight;
    std::map<std::string,std::string> actionAbiRaw;
    std::map<std::string,std::string> guidedEvidence;
    std::vector<std::string> sources;
};

bool FileExistsA2(const std::string& path) {
    const DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string ReadWholeFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool EnsureDir(const std::string& path) {
    if (CreateDirectoryA(path.c_str(), nullptr)) return true;
    const DWORD e = GetLastError();
    return e == ERROR_ALREADY_EXISTS;
}

std::string FingerprintStem(const Report& r) {
    const std::string hash = r.fp.ok ? HexU64(r.fp.textHash).substr(2) : "NOHASH";
    std::ostringstream os;
    os << std::uppercase << std::hex << r.module.timestamp << "_" << hash;
    return os.str();
}

std::string PersistentProfileDirectory() {
    const std::string p1 = ProbeDirectory() + "\\profiles";
    const std::string p2 = p1 + "\\ramonia_x86";
    EnsureDir(p1);
    EnsureDir(p2);
    return p2;
}

std::string PersistentProfilePath(const Report& r) {
    return PersistentProfileDirectory() + "\\" + FingerprintStem(r) + ".json";
}

std::string CanonicalProjectProfilePath(const Report& r) {
    const std::string probeDir = ProbeDirectory();
    const std::string low = Lower(probeDir);
    const std::string marker = "\\build\\ramonia_x86_test";
    const size_t p = low.rfind(marker);
    if (p == std::string::npos) return {};

    const std::string projectRoot = probeDir.substr(0, p);
    const std::string profiles = projectRoot + "\\profiles";
    const std::string ramonia = profiles + "\\ramonia_x86";
    EnsureDir(profiles);
    EnsureDir(ramonia);
    return ramonia + "\\" + FingerprintStem(r) + ".json";
}

std::string FindJsonObject(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return {};
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return {};
    p = json.find('{', p + 1);
    if (p == std::string::npos) return {};
    size_t start = p;
    int depth = 0;
    bool inString = false, escape = false;
    for (; p < json.size(); ++p) {
        const char c = json[p];
        if (inString) {
            if (escape) { escape = false; continue; }
            if (c == '\\') { escape = true; continue; }
            if (c == '"') inString = false;
            continue;
        }
        if (c == '"') { inString = true; continue; }
        if (c == '{') ++depth;
        else if (c == '}') {
            --depth;
            if (depth == 0) return json.substr(start, p - start + 1);
        }
    }
    return {};
}

std::string JsonStringValue(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return {};
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return {};
    p = json.find('"', p + 1);
    if (p == std::string::npos) return {};
    ++p;
    std::string out;
    bool escape = false;
    for (; p < json.size(); ++p) {
        const char c = json[p];
        if (escape) {
            if (c == 'n') out.push_back('\n');
            else if (c == 'r') out.push_back('\r');
            else if (c == 't') out.push_back('\t');
            else out.push_back(c);
            escape = false;
        } else if (c == '\\') escape = true;
        else if (c == '"') break;
        else out.push_back(c);
    }
    return out;
}

int HexOffsetValue(const std::string& s, int missing = -1) {
    if (s.empty()) return missing;
    try {
        unsigned long v = std::stoul(s, nullptr, 0);
        return static_cast<int>(v);
    } catch (...) { return missing; }
}

uint32_t HexU32Value(const std::string& s) {
    const int v = HexOffsetValue(s, -1);
    return v < 0 ? 0u : static_cast<uint32_t>(v);
}

int EvidenceScore(const std::string& status) {
    const std::string s = Lower(status);
    if (s.find("semantic_getcontainer_exact_inline_root_closed_open_reopen") != std::string::npos) return 135;
    if (s.find("semantic_getcontainer_registry_key_match_reopen") != std::string::npos) return 135;
    if (s.find("runtime_same_container_reopen") != std::string::npos) return 132;
    if (s.find("runtime_transition_container") != std::string::npos) return 118;
    if (s.find("semantic_inline_registry") != std::string::npos) return 120;
    if (s.find("runtime_tile_") != std::string::npos) return 132;
    if (s.find("runtime_inventory_slot_transition_item_rtti") != std::string::npos) return 125;
    if (s.find("semantic_current_build_item_count_body_getter_setter") != std::string::npos) return 125;
    if (s.find("semantic_current_build_item_id_count_adjacency_multi_item") != std::string::npos) return 132;
    if (s.find("runtime_item_id_two_items_aba") != std::string::npos) return 135;
    if (s.find("two_transitions") != std::string::npos) return 120;
    if (s.find("runtime_levelup") != std::string::npos) return 110;
    if (s.find("runtime_xyz") != std::string::npos) return 105;
    if (s.find("runtime_transition") != std::string::npos) return 100;
    if (s.find("runtime_mirrored") != std::string::npos) return 100;
    if (s.find("runtime_hppercent_two_creatures") != std::string::npos) return 125;
    if (s.find("runtime_creature_id_semantic_two_targets") != std::string::npos) return 130;
    if (s.find("semantic_current_build_getter_setter") != std::string::npos) return 122;
    if (s.find("semantic_current_build_getter") != std::string::npos) return 108;
    if (s.find("semantic_current_build_outfit_getter_setter") != std::string::npos) return 122;
    if (s.find("semantic_current_build_outfit_getter") != std::string::npos) return 108;
    if (s.find("runtime_world_ambient_two_cycles") != std::string::npos) return 120;
    if (s.find("semantic_uimap_mapview_setter_getter") != std::string::npos) return 118;
    if (s.find("semantic_uimap_mapview_partial") != std::string::npos) return 85;
    if (s.find("runtime_creature_hp") != std::string::npos) return 110;
    if (s.find("runtime_creature_name_mirrored_two_targets") != std::string::npos) return 115;
    if (s.find("runtime_creature_name") != std::string::npos) return 105;
    if (s.find("runtime_reversible_light_two_cycles") != std::string::npos) return 115;
    if (s.find("runtime_reversible_light_pair") != std::string::npos) return 100;
    if (s.find("runtime_target_structural") != std::string::npos) return 100;
    if (s.find("runtime_xy") != std::string::npos) return 90;
    if (s.find("unique_typed_value") != std::string::npos) return 60;
    if (s.find("structural") != std::string::npos) return 40;
    if (s.find("offline") != std::string::npos) return 20;
    if (s.find("no_single_canonical") != std::string::npos) return 0;
    if (s.find("pending") != std::string::npos) return 0;
    return 10;
}

ProfileCache ParseProfileFile(const std::string& path, const Report& r) {
    ProfileCache p;
    const std::string json = ReadWholeFile(path);
    if (json.empty()) return p;
    const std::string build = FindJsonObject(json, "build");
    if (build.empty()) return p;
    const uint32_t ts = HexU32Value(JsonStringValue(build, "timestamp"));
    const std::string hash = Lower(JsonStringValue(build, "textHashFNV1A64"));
    const std::string expectedHash = Lower(r.fp.ok ? HexU64(r.fp.textHash) : std::string("0x0"));
    if (ts != r.module.timestamp || hash != expectedHash) return p;
    p.exactBuild = true;
    p.sources.push_back(path);

    const std::string owners = FindJsonObject(json, "owners");
    p.gameOwnerRva = HexU32Value(JsonStringValue(owners, "gameRva"));
    p.mapOwnerRva = HexU32Value(JsonStringValue(owners, "mapRva"));

    const std::string actions = FindJsonObject(json, "actions");
    const std::string guided = FindJsonObject(json, "guidedValidation");
    if(!guided.empty()) {
        try {
            const auto parsed=crome::json::Parser(guided).parse();
            if(parsed.isObject()) for(const auto& entry:parsed.object()) {
                if(!entry.second.isObject()) continue;
                const auto object=FindJsonObject(guided,entry.first);
                if(!object.empty()) p.guidedEvidence[entry.first]=object;
            }
        } catch(...) { /* Malformed optional evidence is never imported. */ }
    }
    for (const char* tok : ramonia_baseline::abiMethods) {
        const std::string obj = FindJsonObject(actions, tok);
        if (!obj.empty()) p.actionAbiRaw[tok] = obj;
    }

    const std::string gl = FindJsonObject(json, "gameLayout");
    p.localPlayer = HexOffsetValue(JsonStringValue(gl, "localPlayerOffset"));
    p.attackTarget = HexOffsetValue(JsonStringValue(gl, "attackTargetOffset"));
    p.followTarget = HexOffsetValue(JsonStringValue(gl, "followTargetOffset"));
    p.protocolCandidate = HexOffsetValue(JsonStringValue(gl, "protocolCandidateOffset"));
    p.attackSource = JsonStringValue(gl, "attackTargetSource");
    p.followSource = JsonStringValue(gl, "followTargetSource");

    const std::string rt = FindJsonObject(json, "runtime");
    p.localPlayerVtableRva = HexU32Value(JsonStringValue(rt, "localPlayerVtableRva"));
    const std::string pos = FindJsonObject(rt, "position");
    p.posX = HexOffsetValue(JsonStringValue(pos, "x"));
    p.posY = HexOffsetValue(JsonStringValue(pos, "y"));
    p.posZ = HexOffsetValue(JsonStringValue(pos, "z"));
    p.direction = HexOffsetValue(JsonStringValue(pos, "direction"));
    p.positionSource = JsonStringValue(rt, "positionSource");
    p.mapCenter = HexOffsetValue(JsonStringValue(rt, "mapCenterOffset"));
    p.mapCenterStatus = JsonStringValue(rt, "mapCenterStatus");

    const std::string sf = FindJsonObject(rt, "selectedFields");
    const std::vector<std::string> names = {
        "health","maxHealth","mana","maxMana","experience","level","capacity",
        "capacityPairA","capacityPairB","magicLevel","speed","baseSpeed","states","soul","stamina",
        "inventoryArray","inventoryStride","itemId","itemCount",
        "openContainersMap","openContainersSize","openContainersNodeKey","openContainersNodeValue",
        "openContainersPathDepth","openContainersPath0","openContainersPath1","openContainersPath2",
        "openContainersPath3","openContainersPath4","openContainersPath5","openContainersPath6","openContainersPath7",
        "containerItems","containerItemStride","containerId","containerCapacity","containerName",
        "tilePosition","tileThings","tileThingsEnd","tileThingsCap","tileThingsElem","tileVtableRva"
    };
    for (const auto& name : names) {
        const std::string obj = FindJsonObject(sf, name);
        if (obj.empty()) continue;
        SelectedField f;
        f.type = JsonStringValue(obj, "type");
        f.offset = HexOffsetValue(JsonStringValue(obj, "offset"));
        f.status = JsonStringValue(obj, "status");
        if (f.offset >= 0 || !f.status.empty()) p.fields[name] = f;
    }

    const std::string creature = FindJsonObject(rt, "creatureLayout");
    if (!creature.empty()) {
        const std::string cpos = FindJsonObject(creature, "position");
        p.creatureLayout.x = HexOffsetValue(JsonStringValue(cpos, "x"));
        p.creatureLayout.y = HexOffsetValue(JsonStringValue(cpos, "y"));
        p.creatureLayout.z = HexOffsetValue(JsonStringValue(cpos, "z"));
        p.creatureLayout.direction = HexOffsetValue(JsonStringValue(cpos, "direction"));
        p.creatureLayout.positionStatus = JsonStringValue(creature, "positionStatus");

        const std::string cname = FindJsonObject(creature, "name");
        p.creatureLayout.nameOffset = HexOffsetValue(JsonStringValue(cname, "offset"));
        p.creatureLayout.nameKind = JsonStringValue(cname, "kind");
        p.creatureLayout.nameStatus = JsonStringValue(cname, "status");
        p.creatureLayout.nameMirrorOffset = HexOffsetValue(JsonStringValue(cname, "mirrorOffset"));
        p.creatureLayout.nameMirrorKind = JsonStringValue(cname, "mirrorKind");
        p.creatureLayout.nameMirrorStatus = JsonStringValue(cname, "mirrorStatus");

        const std::string chp = FindJsonObject(creature, "hpPercent");
        p.creatureLayout.hpPercent.type = JsonStringValue(chp, "type");
        p.creatureLayout.hpPercent.offset = HexOffsetValue(JsonStringValue(chp, "offset"));
        p.creatureLayout.hpPercent.status = JsonStringValue(chp, "status");

        const std::string cid = FindJsonObject(creature, "id");
        p.creatureLayout.id.type = JsonStringValue(cid, "type");
        p.creatureLayout.id.offset = HexOffsetValue(JsonStringValue(cid, "offset"));
        p.creatureLayout.id.status = JsonStringValue(cid, "status");

        const auto readCreatureScalar = [&](const char* key, SelectedField& out) {
            const std::string obj = FindJsonObject(creature, key);
            out.type = JsonStringValue(obj, "type");
            out.offset = HexOffsetValue(JsonStringValue(obj, "offset"));
            out.status = JsonStringValue(obj, "status");
        };
        readCreatureScalar("speed", p.creatureLayout.speed);
        readCreatureScalar("baseSpeed", p.creatureLayout.baseSpeed);
        readCreatureScalar("skull", p.creatureLayout.skull);
        readCreatureScalar("shield", p.creatureLayout.shield);
        const std::string coutfit = FindJsonObject(creature, "outfit");
        p.creatureLayout.outfitOffset = HexOffsetValue(JsonStringValue(coutfit, "offset"));
        p.creatureLayout.outfitStatus = JsonStringValue(coutfit, "status");

        const std::string clight = FindJsonObject(creature, "light");
        const std::string clevel = FindJsonObject(clight, "level");
        p.creatureLayout.lightLevel.type = JsonStringValue(clevel, "type");
        p.creatureLayout.lightLevel.offset = HexOffsetValue(JsonStringValue(clevel, "offset"));
        p.creatureLayout.lightLevel.status = JsonStringValue(clevel, "status");
        const std::string ccolor = FindJsonObject(clight, "color");
        p.creatureLayout.lightColor.type = JsonStringValue(ccolor, "type");
        p.creatureLayout.lightColor.offset = HexOffsetValue(JsonStringValue(ccolor, "offset"));
        p.creatureLayout.lightColor.status = JsonStringValue(ccolor, "status");
        p.creatureLayout.lightCandidateA = HexOffsetValue(JsonStringValue(clight, "candidateA"));
        p.creatureLayout.lightCandidateB = HexOffsetValue(JsonStringValue(clight, "candidateB"));
        p.creatureLayout.lightStatus = JsonStringValue(clight, "status");
    }

    const std::string worldLight = FindJsonObject(rt, "worldLight");
    if (!worldLight.empty()) {
        p.worldLight.ownerRva = HexU32Value(JsonStringValue(worldLight, "ownerRva"));
        p.worldLight.setDrawLightsRva = HexU32Value(JsonStringValue(worldLight, "setDrawLightsRva"));
        p.worldLight.isDrawingLightsRva = HexU32Value(JsonStringValue(worldLight, "isDrawingLightsRva"));
        p.worldLight.setMinimumAmbientLightRva = HexU32Value(JsonStringValue(worldLight, "setMinimumAmbientLightRva"));
        p.worldLight.getMinimumAmbientLightRva = HexU32Value(JsonStringValue(worldLight, "getMinimumAmbientLightRva"));
        p.worldLight.uiMapToMapView = HexOffsetValue(JsonStringValue(worldLight, "uiMapToMapView"));
        const std::string drawLights = FindJsonObject(worldLight, "drawLights");
        p.worldLight.drawLights.type = JsonStringValue(drawLights, "type");
        p.worldLight.drawLights.offset = HexOffsetValue(JsonStringValue(drawLights, "offset"));
        p.worldLight.drawLights.status = JsonStringValue(drawLights, "status");
        const std::string minAmbient = FindJsonObject(worldLight, "minimumAmbient");
        p.worldLight.minimumAmbient.type = JsonStringValue(minAmbient, "type");
        p.worldLight.minimumAmbient.offset = HexOffsetValue(JsonStringValue(minAmbient, "offset"));
        p.worldLight.minimumAmbient.status = JsonStringValue(minAmbient, "status");
        p.worldLight.candidateA = HexOffsetValue(JsonStringValue(worldLight, "candidateA"));
        p.worldLight.candidateB = HexOffsetValue(JsonStringValue(worldLight, "candidateB"));
        p.worldLight.staticContractStatus = JsonStringValue(worldLight, "staticContractStatus");
        p.worldLight.status = JsonStringValue(worldLight, "status");
    }
    return p;
}

void MergeProfile(ProfileCache& dst, const ProfileCache& src) {
    if (!src.exactBuild) return;
    dst.exactBuild = true;
    for (const auto& x : src.sources) dst.sources.push_back(x);
    if (!dst.gameOwnerRva && src.gameOwnerRva) dst.gameOwnerRva = src.gameOwnerRva;
    if (!dst.mapOwnerRva && src.mapOwnerRva) dst.mapOwnerRva = src.mapOwnerRva;
    if (dst.localPlayer < 0 && src.localPlayer >= 0) dst.localPlayer = src.localPlayer;
    if (src.attackTarget >= 0 && (dst.attackTarget < 0 || EvidenceScore(src.attackSource) > EvidenceScore(dst.attackSource))) {
        dst.attackTarget = src.attackTarget; dst.attackSource = src.attackSource;
    }
    if (src.followTarget >= 0 && (dst.followTarget < 0 || EvidenceScore(src.followSource) > EvidenceScore(dst.followSource))) {
        dst.followTarget = src.followTarget; dst.followSource = src.followSource;
    }
    if (dst.protocolCandidate < 0 && src.protocolCandidate >= 0) dst.protocolCandidate = src.protocolCandidate;
    if (src.posX >= 0 && src.posY >= 0 && src.posZ >= 0 &&
        (dst.posX < 0 || EvidenceScore(src.positionSource) > EvidenceScore(dst.positionSource))) {
        dst.posX = src.posX; dst.posY = src.posY; dst.posZ = src.posZ; dst.direction = src.direction;
        dst.positionSource = src.positionSource;
    }
    if (src.mapCenter >= 0 && (dst.mapCenter < 0 || EvidenceScore(src.mapCenterStatus) > EvidenceScore(dst.mapCenterStatus))) {
        dst.mapCenter = src.mapCenter; dst.mapCenterStatus = src.mapCenterStatus;
    }
    if (!dst.localPlayerVtableRva && src.localPlayerVtableRva) dst.localPlayerVtableRva = src.localPlayerVtableRva;

    if (src.creatureLayout.x >= 0 && src.creatureLayout.y >= 0 && src.creatureLayout.z >= 0 &&
        (dst.creatureLayout.x < 0 || EvidenceScore(src.creatureLayout.positionStatus) > EvidenceScore(dst.creatureLayout.positionStatus))) {
        dst.creatureLayout.x = src.creatureLayout.x;
        dst.creatureLayout.y = src.creatureLayout.y;
        dst.creatureLayout.z = src.creatureLayout.z;
        dst.creatureLayout.direction = src.creatureLayout.direction;
        dst.creatureLayout.positionStatus = src.creatureLayout.positionStatus;
    }
    if (src.creatureLayout.nameOffset >= 0 &&
        (dst.creatureLayout.nameOffset < 0 || EvidenceScore(src.creatureLayout.nameStatus) > EvidenceScore(dst.creatureLayout.nameStatus))) {
        dst.creatureLayout.nameOffset = src.creatureLayout.nameOffset;
        dst.creatureLayout.nameKind = src.creatureLayout.nameKind;
        dst.creatureLayout.nameStatus = src.creatureLayout.nameStatus;
    }
    if (src.creatureLayout.nameMirrorOffset >= 0 &&
        (dst.creatureLayout.nameMirrorOffset < 0 || EvidenceScore(src.creatureLayout.nameMirrorStatus) > EvidenceScore(dst.creatureLayout.nameMirrorStatus))) {
        dst.creatureLayout.nameMirrorOffset = src.creatureLayout.nameMirrorOffset;
        dst.creatureLayout.nameMirrorKind = src.creatureLayout.nameMirrorKind;
        dst.creatureLayout.nameMirrorStatus = src.creatureLayout.nameMirrorStatus;
    }
    if (src.creatureLayout.hpPercent.offset >= 0 &&
        (dst.creatureLayout.hpPercent.offset < 0 || EvidenceScore(src.creatureLayout.hpPercent.status) > EvidenceScore(dst.creatureLayout.hpPercent.status))) {
        dst.creatureLayout.hpPercent = src.creatureLayout.hpPercent;
    }
    if (src.creatureLayout.id.offset >= 0 &&
        (dst.creatureLayout.id.offset < 0 || EvidenceScore(src.creatureLayout.id.status) > EvidenceScore(dst.creatureLayout.id.status))) {
        dst.creatureLayout.id = src.creatureLayout.id;
    }
    const auto mergeCreatureScalar = [&](const SelectedField& srcField, SelectedField& dstField) {
        if (srcField.offset >= 0 &&
            (dstField.offset < 0 || EvidenceScore(srcField.status) > EvidenceScore(dstField.status))) {
            dstField = srcField;
        }
    };
    mergeCreatureScalar(src.creatureLayout.speed, dst.creatureLayout.speed);
    mergeCreatureScalar(src.creatureLayout.baseSpeed, dst.creatureLayout.baseSpeed);
    mergeCreatureScalar(src.creatureLayout.skull, dst.creatureLayout.skull);
    mergeCreatureScalar(src.creatureLayout.shield, dst.creatureLayout.shield);
    if (src.creatureLayout.outfitOffset >= 0 &&
        (dst.creatureLayout.outfitOffset < 0 || EvidenceScore(src.creatureLayout.outfitStatus) > EvidenceScore(dst.creatureLayout.outfitStatus))) {
        dst.creatureLayout.outfitOffset = src.creatureLayout.outfitOffset;
        dst.creatureLayout.outfitStatus = src.creatureLayout.outfitStatus;
    }
    if (src.creatureLayout.lightLevel.offset >= 0 &&
        (dst.creatureLayout.lightLevel.offset < 0 || EvidenceScore(src.creatureLayout.lightLevel.status) > EvidenceScore(dst.creatureLayout.lightLevel.status))) {
        dst.creatureLayout.lightLevel = src.creatureLayout.lightLevel;
    }
    if (src.creatureLayout.lightColor.offset >= 0 &&
        (dst.creatureLayout.lightColor.offset < 0 || EvidenceScore(src.creatureLayout.lightColor.status) > EvidenceScore(dst.creatureLayout.lightColor.status))) {
        dst.creatureLayout.lightColor = src.creatureLayout.lightColor;
    }
    if (EvidenceScore(src.creatureLayout.lightStatus) > EvidenceScore(dst.creatureLayout.lightStatus)) {
        dst.creatureLayout.lightCandidateA = src.creatureLayout.lightCandidateA;
        dst.creatureLayout.lightCandidateB = src.creatureLayout.lightCandidateB;
        dst.creatureLayout.lightStatus = src.creatureLayout.lightStatus;
    }

    if (src.worldLight.ownerRva && !dst.worldLight.ownerRva) dst.worldLight.ownerRva = src.worldLight.ownerRva;
    if (src.worldLight.setDrawLightsRva && !dst.worldLight.setDrawLightsRva) dst.worldLight.setDrawLightsRva = src.worldLight.setDrawLightsRva;
    if (src.worldLight.isDrawingLightsRva && !dst.worldLight.isDrawingLightsRva) dst.worldLight.isDrawingLightsRva = src.worldLight.isDrawingLightsRva;
    if (src.worldLight.setMinimumAmbientLightRva && !dst.worldLight.setMinimumAmbientLightRva)
        dst.worldLight.setMinimumAmbientLightRva = src.worldLight.setMinimumAmbientLightRva;
    if (src.worldLight.getMinimumAmbientLightRva && !dst.worldLight.getMinimumAmbientLightRva)
        dst.worldLight.getMinimumAmbientLightRva = src.worldLight.getMinimumAmbientLightRva;
    if (src.worldLight.uiMapToMapView >= 0 && dst.worldLight.uiMapToMapView < 0)
        dst.worldLight.uiMapToMapView = src.worldLight.uiMapToMapView;
    if (src.worldLight.drawLights.offset >= 0 &&
        (dst.worldLight.drawLights.offset < 0 || EvidenceScore(src.worldLight.drawLights.status) > EvidenceScore(dst.worldLight.drawLights.status))) {
        dst.worldLight.drawLights = src.worldLight.drawLights;
    }
    if (src.worldLight.minimumAmbient.offset >= 0 &&
        (dst.worldLight.minimumAmbient.offset < 0 || EvidenceScore(src.worldLight.minimumAmbient.status) > EvidenceScore(dst.worldLight.minimumAmbient.status))) {
        dst.worldLight.minimumAmbient = src.worldLight.minimumAmbient;
    }
    if (EvidenceScore(src.worldLight.staticContractStatus) > EvidenceScore(dst.worldLight.staticContractStatus))
        dst.worldLight.staticContractStatus = src.worldLight.staticContractStatus;
    if (EvidenceScore(src.worldLight.status) > EvidenceScore(dst.worldLight.status)) {
        dst.worldLight.candidateA = src.worldLight.candidateA;
        dst.worldLight.candidateB = src.worldLight.candidateB;
        dst.worldLight.status = src.worldLight.status;
    }

    for (const auto& kv : src.fields) {
        auto it = dst.fields.find(kv.first);
        if (it == dst.fields.end() || EvidenceScore(kv.second.status) > EvidenceScore(it->second.status)) {
            dst.fields[kv.first] = kv.second;
        }
    }
    for (const auto& kv : src.actionAbiRaw) {
        if (!dst.actionAbiRaw.count(kv.first)) dst.actionAbiRaw[kv.first] = kv.second;
    }
    for(const auto& kv:src.guidedEvidence)
        if(!dst.guidedEvidence.count(kv.first)) dst.guidedEvidence[kv.first]=kv.second;
}

ProfileCache LoadProfileForBuild(const Report& r) {
    const std::string primary = PersistentProfilePath(r);
    if (FileExistsA2(primary)) {
        ProfileCache p = ParseProfileFile(primary, r);
        if (p.exactBuild) return p;
    }

    // Legacy import happens only when the canonical persistent profile does not
    // exist yet. After the first save, future runs use one source of truth.
    ProfileCache merged;
    const std::string base = ProbeDirectory() + "\\OTCLab_RamoniaResearch_" + FingerprintStem(r);
    std::vector<std::string> paths;
    paths.push_back(base + ".json");
    for (int v = 8; v >= 2; --v) paths.push_back(base + "_V" + std::to_string(v) + ".json");
    std::set<std::string> seen;
    for (const auto& path : paths) {
        if (!seen.insert(Lower(path)).second || !FileExistsA2(path)) continue;
        ProfileCache one = ParseProfileFile(path, r);
        MergeProfile(merged, one);
    }
    return merged;
}

void ApplyLoadedProfile(Report& r, const ProfileCache& p) {
    if (!p.exactBuild) return;
    r.profileLoaded = true;
    r.profileSources = p.sources;
    if (p.gameOwnerRva && !r.gameOwnerRva) r.gameOwnerRva = p.gameOwnerRva;
    if (p.mapOwnerRva && !r.mapOwnerRva) r.mapOwnerRva = p.mapOwnerRva;
    if (p.localPlayer >= 0) r.gameLayout.localPlayer = p.localPlayer;
    if (p.attackTarget >= 0 && EvidenceScore(p.attackSource) >= 90) {
        r.gameLayout.attackTarget = p.attackTarget; r.attackTargetSource = p.attackSource;
    }
    if (p.followTarget >= 0 && EvidenceScore(p.followSource) >= 90) {
        r.gameLayout.followTarget = p.followTarget; r.followTargetSource = p.followSource;
    }
    if (p.protocolCandidate >= 0) r.gameLayout.protocolCandidate = p.protocolCandidate;
    if (p.posX >= 0 && p.posY >= 0 && p.posZ >= 0 && EvidenceScore(p.positionSource) >= 90) {
        r.positionRuntime.x = p.posX; r.positionRuntime.y = p.posY; r.positionRuntime.z = p.posZ;
        r.positionRuntime.direction = p.direction; r.positionRuntime.xyzPromotedByAdjacency = true;
        r.positionSource = p.positionSource;
    }
    if (p.mapCenter >= 0 && EvidenceScore(p.mapCenterStatus) >= 80) {
        r.mapCenterOffset = p.mapCenter; r.mapCenterStatus = p.mapCenterStatus;
    }

    if (p.creatureLayout.x >= 0 && p.creatureLayout.y >= 0 && p.creatureLayout.z >= 0 &&
        EvidenceScore(p.creatureLayout.positionStatus) >= 80) {
        r.creatureLayout.x = p.creatureLayout.x;
        r.creatureLayout.y = p.creatureLayout.y;
        r.creatureLayout.z = p.creatureLayout.z;
        r.creatureLayout.direction = p.creatureLayout.direction;
        r.creatureLayout.positionStatus = p.creatureLayout.positionStatus;
    }
    if (p.creatureLayout.nameOffset >= 0 && EvidenceScore(p.creatureLayout.nameStatus) >= 80) {
        r.creatureLayout.nameOffset = p.creatureLayout.nameOffset;
        r.creatureLayout.nameKind = p.creatureLayout.nameKind;
        r.creatureLayout.nameStatus = p.creatureLayout.nameStatus;
    }
    if (p.creatureLayout.nameMirrorOffset >= 0 && EvidenceScore(p.creatureLayout.nameMirrorStatus) >= 80) {
        r.creatureLayout.nameMirrorOffset = p.creatureLayout.nameMirrorOffset;
        r.creatureLayout.nameMirrorKind = p.creatureLayout.nameMirrorKind;
        r.creatureLayout.nameMirrorStatus = p.creatureLayout.nameMirrorStatus;
    }
    if (p.creatureLayout.hpPercent.offset >= 0 && EvidenceScore(p.creatureLayout.hpPercent.status) >= 80) {
        r.creatureLayout.hpPercent = p.creatureLayout.hpPercent;
    }
    if (p.creatureLayout.id.offset >= 0 && EvidenceScore(p.creatureLayout.id.status) >= 80) {
        r.creatureLayout.id = p.creatureLayout.id;
    }
    const auto applyCreatureScalar = [&](const SelectedField& srcField, SelectedField& dstField) {
        if (srcField.offset >= 0 && EvidenceScore(srcField.status) >= 80) dstField = srcField;
    };
    applyCreatureScalar(p.creatureLayout.speed, r.creatureLayout.speed);
    applyCreatureScalar(p.creatureLayout.baseSpeed, r.creatureLayout.baseSpeed);
    applyCreatureScalar(p.creatureLayout.skull, r.creatureLayout.skull);
    applyCreatureScalar(p.creatureLayout.shield, r.creatureLayout.shield);
    if (p.creatureLayout.outfitOffset >= 0 && EvidenceScore(p.creatureLayout.outfitStatus) >= 80) {
        r.creatureLayout.outfitOffset = p.creatureLayout.outfitOffset;
        r.creatureLayout.outfitStatus = p.creatureLayout.outfitStatus;
    }
    if (p.creatureLayout.lightLevel.offset >= 0 && EvidenceScore(p.creatureLayout.lightLevel.status) >= 80) {
        r.creatureLayout.lightLevel = p.creatureLayout.lightLevel;
    }
    if (p.creatureLayout.lightColor.offset >= 0 && EvidenceScore(p.creatureLayout.lightColor.status) >= 80) {
        r.creatureLayout.lightColor = p.creatureLayout.lightColor;
    }
    if (EvidenceScore(p.creatureLayout.lightStatus) >= 80) {
        r.creatureLayout.lightCandidateA = p.creatureLayout.lightCandidateA;
        r.creatureLayout.lightCandidateB = p.creatureLayout.lightCandidateB;
        r.creatureLayout.lightStatus = p.creatureLayout.lightStatus;
    }

    if (p.worldLight.ownerRva) r.worldLight.ownerRva = p.worldLight.ownerRva;
    if (p.worldLight.setDrawLightsRva) r.worldLight.setDrawLightsRva = p.worldLight.setDrawLightsRva;
    if (p.worldLight.isDrawingLightsRva) r.worldLight.isDrawingLightsRva = p.worldLight.isDrawingLightsRva;
    if (p.worldLight.setMinimumAmbientLightRva) r.worldLight.setMinimumAmbientLightRva = p.worldLight.setMinimumAmbientLightRva;
    if (p.worldLight.getMinimumAmbientLightRva) r.worldLight.getMinimumAmbientLightRva = p.worldLight.getMinimumAmbientLightRva;
    if (p.worldLight.uiMapToMapView >= 0) r.worldLight.uiMapToMapView = p.worldLight.uiMapToMapView;
    if (p.worldLight.drawLights.offset >= 0 && EvidenceScore(p.worldLight.drawLights.status) >= 80)
        r.worldLight.drawLights = p.worldLight.drawLights;
    if (p.worldLight.minimumAmbient.offset >= 0 && EvidenceScore(p.worldLight.minimumAmbient.status) >= 80)
        r.worldLight.minimumAmbient = p.worldLight.minimumAmbient;
    if (EvidenceScore(p.worldLight.staticContractStatus) >= 80)
        r.worldLight.staticContractStatus = p.worldLight.staticContractStatus;
    if (EvidenceScore(p.worldLight.status) >= 80) {
        r.worldLight.candidateA = p.worldLight.candidateA;
        r.worldLight.candidateB = p.worldLight.candidateB;
        r.worldLight.status = p.worldLight.status;
    }

    for (const auto& kv : p.fields) {
        if (kv.second.offset >= 0 && EvidenceScore(kv.second.status) > 0) r.selectedFields[kv.first] = kv.second;
    }
    r.actionAbiRaw = p.actionAbiRaw;
    r.guidedEvidence = p.guidedEvidence;
}

bool QuickCheckPosition(uintptr_t lp, const RuntimePositionValidation& p,
                        uint32_t* ox=nullptr, uint32_t* oy=nullptr, uint8_t* oz=nullptr) {
    if (!lp || p.x < 0 || p.y < 0 || p.z < 0) return false;
    uint32_t x=0,y=0; uint8_t z=0;
    if (!SafeRead(lp+p.x,x) || !SafeRead(lp+p.y,y) || !SafeRead(lp+p.z,z)) return false;
    if (!x || !y || x > 65535 || y > 65535 || z > 15) return false;
    if (p.direction >= 0) { uint32_t d=0; if (!SafeRead(lp+p.direction,d) || d > 7) return false; }
    if (ox) *ox=x; if (oy) *oy=y; if (oz) *oz=z;
    return true;
}

bool QuickCheckMapCenter(uintptr_t mapObj, uintptr_t lp,
                         const RuntimePositionValidation& p, int centerOff) {
    if (!mapObj || centerOff < 0) return false;
    uint32_t x=0,y=0,mx=0,my=0; uint8_t z=0,mz=0;
    if (!QuickCheckPosition(lp,p,&x,&y,&z)) return false;
    return SafeRead(mapObj+centerOff,mx) && SafeRead(mapObj+centerOff+4,my) &&
           SafeRead(mapObj+centerOff+8,mz) && mx==x && my==y && mz==z;
}

bool ReadSelectedValue(uintptr_t lp, const SelectedField& f, double& out) {
    if (!lp || f.offset < 0) return false;
    if (f.type == "double") { double v=0; if (!SafeRead(lp+f.offset,v) || !std::isfinite(v)) return false; out=v; return true; }
    if (f.type == "float") { float v=0; if (!SafeRead(lp+f.offset,v) || !std::isfinite(v)) return false; out=v; return true; }
    if (f.type == "u32") { uint32_t v=0; if (!SafeRead(lp+f.offset,v)) return false; out=v; return true; }
    if (f.type == "u16") { uint16_t v=0; if (!SafeRead(lp+f.offset,v)) return false; out=v; return true; }
    if (f.type == "u8") { uint8_t v=0; if (!SafeRead(lp+f.offset,v)) return false; out=v; return true; }
    return false;
}

void PrintLoadedFieldQuickChecks(Report& r) {
    if (r.selectedFields.empty()) return;
    const std::set<std::string> localPlayerFields = {
        "health","maxHealth","mana","maxMana","experience","level","capacity",
        "capacityPairA","capacityPairB","magicLevel","states","soul","stamina"
    };
    std::cout << "\n[PROFILE FIELD QUICK-CHECK]\n";
    for (auto& kv : r.selectedFields) {
        if (kv.second.offset < 0 || !localPlayerFields.count(kv.first)) continue;
        double v=0;
        const bool ok = ReadSelectedValue(r.localPlayerPtr,kv.second,v);
        std::cout << "  " << std::left << std::setw(14) << kv.first << std::right
                  << " +" << HexU32(kv.second.offset) << " " << kv.second.type
                  << " -> " << (ok?std::to_string(v):std::string("<read failed>"))
                  << " [" << kv.second.status << "]\n";
    }
}



void SaveCheckpoint(Report& r, const std::string& checkpoint);

std::string AskText(const std::string& label) {
    probe_console::ui() << label << ": " << std::flush;
    std::string s;
    std::getline(std::cin, s);
    return s;
}

bool ReadCreatureNameAt(uintptr_t creature, int offset, const std::string& kind, std::string& out) {
    out.clear();
    if (!creature || offset < 0) return false;
    if (kind == "string_ptr") {
        uint32_t p = 0;
        if (!SafeRead(creature + offset, p) || !p) return false;
        return SafeReadCString(p, out, 96) && !out.empty();
    }
    if (kind == "inline_cstring") {
        return SafeReadCString(creature + offset, out, 96) && !out.empty();
    }
    return false;
}

bool ReadCreatureName(uintptr_t creature, const CreatureLayout& c, std::string& out) {
    if (ReadCreatureNameAt(creature, c.nameOffset, c.nameKind, out)) return true;
    if (ReadCreatureNameAt(creature, c.nameMirrorOffset, c.nameMirrorKind, out)) return true;
    out.clear();
    return false;
}

struct CreatureNameCandidate {
    int offset = -1;
    std::string kind;
    std::string value;
};

std::vector<CreatureNameCandidate> FindCreatureNameCandidates(uintptr_t creature,
                                                               const std::string& expected,
                                                               int maxOff = 0x200) {
    std::vector<CreatureNameCandidate> out;
    if (!creature || expected.empty()) return out;
    const std::string wanted = Lower(expected);

    for (int off = 0; off <= maxOff - 4; off += 4) {
        uint32_t p = 0;
        if (!SafeRead(creature + off, p) || !p) continue;
        std::string text;
        if (SafeReadCString(p, text, 96) && !text.empty() && Lower(text) == wanted) {
            out.push_back({off, "string_ptr", text});
        }
    }

    std::vector<uint8_t> bytes;
    if (CaptureWindow(creature, bytes, static_cast<size_t>(maxOff))) {
        const std::string raw(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        size_t pos = 0;
        while ((pos = raw.find(expected, pos)) != std::string::npos) {
            const size_t end = pos + expected.size();
            if (end < raw.size() && raw[end] == '\0') {
                out.push_back({static_cast<int>(pos), "inline_cstring", expected});
            }
            ++pos;
        }
    }

    std::sort(out.begin(), out.end(), [](const CreatureNameCandidate& a, const CreatureNameCandidate& b) {
        if (a.offset != b.offset) return a.offset < b.offset;
        return a.kind < b.kind;
    });
    out.erase(std::unique(out.begin(), out.end(), [](const CreatureNameCandidate& a, const CreatureNameCandidate& b) {
        return a.offset == b.offset && a.kind == b.kind;
    }), out.end());
    return out;
}

struct CreatureHpCandidate {
    std::string type;
    int offset = -1;
    std::string encoding;
};

bool SameHpCandidate(const CreatureHpCandidate& a, const CreatureHpCandidate& b) {
    return a.type == b.type && a.offset == b.offset && a.encoding == b.encoding;
}

std::vector<CreatureHpCandidate> FindCreatureHpCandidates(uintptr_t creature, double displayed, int maxOff = 0x400) {
    std::vector<CreatureHpCandidate> hits;
    if (!creature || displayed < 0.0 || displayed > 100.0) return hits;
    const double norm = displayed / 100.0;
    auto add=[&](const std::string& t,int off,const std::string& enc){
        CreatureHpCandidate c{t,off,enc};
        for(const auto& h:hits) if(SameHpCandidate(h,c)) return;
        hits.push_back(c);
    };
    for (int off = 0; off < maxOff; ++off) {
        uint8_t u8=0;
        if (SafeRead(creature+off,u8) && std::fabs(static_cast<double>(u8)-displayed) <= 0.001) add("u8",off,"percent_0_100");
        if ((off%2)==0 && off+2<=maxOff) {
            uint16_t v=0; if(SafeRead(creature+off,v) && std::fabs(static_cast<double>(v)-displayed)<=0.001) add("u16",off,"percent_0_100");
        }
        if ((off%4)==0 && off+4<=maxOff) {
            uint32_t v=0; if(SafeRead(creature+off,v) && std::fabs(static_cast<double>(v)-displayed)<=0.001) add("u32",off,"percent_0_100");
            float f=0; if(SafeRead(creature+off,f) && std::isfinite(f)) {
                if(std::fabs(static_cast<double>(f)-displayed)<=1e-4) add("float",off,"percent_0_100");
                if(std::fabs(static_cast<double>(f)-norm)<=1e-5) add("float",off,"normalized_0_1");
            }
        }
        if ((off%4)==0 && off+8<=maxOff) {
            double d=0; if(SafeRead(creature+off,d) && std::isfinite(d)) {
                if(std::fabs(d-displayed)<=1e-7) add("double",off,"percent_0_100");
                if(std::fabs(d-norm)<=1e-7) add("double",off,"normalized_0_1");
            }
        }
    }
    return hits;
}

std::vector<CreatureHpCandidate> IntersectHpCandidates(const std::vector<CreatureHpCandidate>& a,
                                                       const std::vector<CreatureHpCandidate>& b) {
    std::vector<CreatureHpCandidate> out;
    for(const auto& x:a) for(const auto& y:b) if(SameHpCandidate(x,y)) { out.push_back(x); break; }
    return out;
}

bool ReadCreatureHpDisplay(uintptr_t creature, const SelectedField& f, double& out) {
    double raw=0;
    if(!ReadSelectedValue(creature,f,raw)) return false;
    out = (Lower(f.status).find("normalized_0_1") != std::string::npos) ? raw*100.0 : raw;
    return std::isfinite(out) && out >= 0.0 && out <= 100.0;
}

bool ReadHpCandidateRaw(uintptr_t creature, const CreatureHpCandidate& c, double& raw) {
    raw = 0.0;
    if (!creature || c.offset < 0) return false;
    if (c.type == "u8") {
        uint8_t v=0; if(!SafeRead(creature+c.offset,v)) return false; raw=v; return true;
    }
    if (c.type == "u16") {
        uint16_t v=0; if(!SafeRead(creature+c.offset,v)) return false; raw=v; return true;
    }
    if (c.type == "u32") {
        uint32_t v=0; if(!SafeRead(creature+c.offset,v)) return false; raw=v; return true;
    }
    if (c.type == "float") {
        float v=0; if(!SafeRead(creature+c.offset,v) || !std::isfinite(v)) return false; raw=v; return true;
    }
    if (c.type == "double") {
        double v=0; if(!SafeRead(creature+c.offset,v) || !std::isfinite(v)) return false; raw=v; return true;
    }
    return false;
}

bool HpCandidateIsFull(uintptr_t creature, const CreatureHpCandidate& c) {
    double raw=0.0;
    if(!ReadHpCandidateRaw(creature,c,raw)) return false;
    if(c.encoding=="normalized_0_1") return std::fabs(raw-1.0) <= 1e-5;
    return std::fabs(raw-100.0) <= 1e-4;
}

bool HpCandidateIsDamaged(uintptr_t creature, const CreatureHpCandidate& c, double* outRaw=nullptr) {
    double raw=0.0;
    if(!ReadHpCandidateRaw(creature,c,raw)) return false;
    if(outRaw) *outRaw=raw;
    if(c.encoding=="normalized_0_1") return raw > 0.000001 && raw < 0.99999;
    return raw > 0.0 && raw < 100.0;
}

std::vector<CreatureHpCandidate> FilterHpDamaged(uintptr_t creature,
                                                  const std::vector<CreatureHpCandidate>& in) {
    std::vector<CreatureHpCandidate> out;
    for(const auto& c:in) if(HpCandidateIsDamaged(creature,c)) out.push_back(c);
    return out;
}

std::vector<CreatureHpCandidate> FilterHpFull(uintptr_t creature,
                                               const std::vector<CreatureHpCandidate>& in) {
    std::vector<CreatureHpCandidate> out;
    for(const auto& c:in) if(HpCandidateIsFull(creature,c)) out.push_back(c);
    return out;
}

void PrintHpCandidatesWithCurrent(uintptr_t creature,
                                  const std::vector<CreatureHpCandidate>& v,
                                  const char* label) {
    std::cout << "[" << label << "] candidates=" << v.size() << "\n";
    for(size_t i=0;i<v.size() && i<32;++i) {
        double raw=0.0; const bool ok=ReadHpCandidateRaw(creature,v[i],raw);
        std::cout << "  " << v[i].type << " +" << HexU32(static_cast<uint32_t>(v[i].offset))
                  << " " << v[i].encoding << " -> "
                  << (ok?std::to_string(raw):std::string("<read failed>")) << "\n";
    }
    if(v.size()>32) std::cout << "  ... (" << (v.size()-32) << " more)\n";
}

bool SelectHpCandidateUniqueOffset(const std::vector<CreatureHpCandidate>& in,
                                   CreatureHpCandidate& out) {
    if(in.empty()) return false;
    std::set<int> offsets;
    for(const auto& c:in) offsets.insert(c.offset);
    if(offsets.size()!=1) return false;
    const int off=*offsets.begin();
    const std::vector<std::string> priority={"u8","u16","u32","float","double"};
    for(const auto& type:priority) {
        for(const auto& c:in) if(c.offset==off && c.type==type) { out=c; return true; }
    }
    out=in.front();
    return true;
}

bool QuickCheckCreaturePosition(uintptr_t creature,
                                uintptr_t localPlayer,
                                const RuntimePositionValidation& playerPos,
                                const CreatureLayout& layout,
                                uint32_t* outX = nullptr,
                                uint32_t* outY = nullptr,
                                uint8_t* outZ = nullptr) {
    if (!creature || !localPlayer || layout.x < 0 || layout.y < 0 || layout.z < 0) return false;
    uint32_t px = 0, py = 0, x = 0, y = 0, d = 0;
    uint8_t pz = 0, z = 0;
    if (!QuickCheckPosition(localPlayer, playerPos, &px, &py, &pz)) return false;
    if (!SafeRead(creature + layout.x, x) || !SafeRead(creature + layout.y, y) || !SafeRead(creature + layout.z, z)) return false;
    if (!x || !y || x > 65535 || y > 65535 || z > 15) return false;
    if (std::abs(static_cast<int64_t>(x) - static_cast<int64_t>(px)) > 64) return false;
    if (std::abs(static_cast<int64_t>(y) - static_cast<int64_t>(py)) > 64) return false;
    if (layout.direction >= 0) {
        if (!SafeRead(creature + layout.direction, d) || d > 7) return false;
    }
    if (outX) *outX = x;
    if (outY) *outY = y;
    if (outZ) *outZ = z;
    return true;
}

bool PromoteCreaturePositionFromPlayerLayout(Report& r, uintptr_t creature) {
    CreatureLayout candidate = r.creatureLayout;
    if (candidate.x < 0 || candidate.y < 0 || candidate.z < 0) {
        candidate.x = r.positionRuntime.x;
        candidate.y = r.positionRuntime.y;
        candidate.z = r.positionRuntime.z;
        candidate.direction = r.positionRuntime.direction;
    }
    uint32_t x = 0, y = 0;
    uint8_t z = 0;
    if (!QuickCheckCreaturePosition(creature, r.localPlayerPtr, r.positionRuntime, candidate, &x, &y, &z)) return false;
    r.creatureLayout.x = candidate.x;
    r.creatureLayout.y = candidate.y;
    r.creatureLayout.z = candidate.z;
    r.creatureLayout.direction = candidate.direction;
    r.creatureLayout.positionStatus = "runtime_target_structural_match";
    std::cout << "[CREATURE POSITION] confirmed same base layout -> X +" << HexU32(r.creatureLayout.x)
              << " Y +" << HexU32(r.creatureLayout.y)
              << " Z +" << HexU32(r.creatureLayout.z)
              << " Direction +" << HexU32(r.creatureLayout.direction)
              << " current=" << x << "," << y << "," << static_cast<unsigned>(z) << "\n";
    return true;
}

uintptr_t AcquireCreatureFromAttackTarget(Report& r, bool allowPrompt) {
    if (!r.gameOwnerRva || r.gameLayout.attackTarget < 0) return 0;
    const uintptr_t game = r.module.base + r.gameOwnerRva;
    auto readTarget = [&]() -> uintptr_t {
        uint32_t p = 0;
        if (!SafeRead(game + r.gameLayout.attackTarget, p) || !p) return 0;
        const std::string cls = KnownCreatureClass(p, r.module, r.rtti);
        if (cls.empty()) return 0;
        r.creatureTargetClass = cls;
        return p;
    };

    uintptr_t target = readTarget();
    if (target) {
        std::cout << "[CREATURE TARGET] reutilizando AttackTarget ativo ptr=" << HexU64(target)
                  << " RTTI=" << r.creatureTargetClass << "\n";
        return target;
    }
    if (!allowPrompt) return 0;
    if (AskYesNo("AttackTarget esta vazio. Mapear Creature agora atacando uma criatura?") != 's') return 0;
    WaitEnter("Ataque uma criatura e MANTENHA o target. Depois ENTER... ");
    target = readTarget();
    if (!target) {
        std::cout << "[CREATURE TARGET] target ausente ou RTTI desconhecido; discovery adiado.\n";
        return 0;
    }
    std::cout << "[CREATURE TARGET] ptr=" << HexU64(target) << " RTTI=" << r.creatureTargetClass << "\n";
    return target;
}


struct ReversibleByteHit {
    int offset = -1;
    uint8_t base = 0;
    uint8_t active = 0;
    uint8_t restored = 0;
};

std::vector<ReversibleByteHit> FindReversibleByteHits(const std::vector<uint8_t>& a,
                                                       const std::vector<uint8_t>& b,
                                                       const std::vector<uint8_t>& c) {
    std::vector<ReversibleByteHit> out;
    const size_t n = std::min({a.size(), b.size(), c.size()});
    for (size_t i = 0; i < n; ++i) {
        if (a[i] == c[i] && a[i] != b[i]) {
            out.push_back({static_cast<int>(i), a[i], b[i], c[i]});
        }
    }
    return out;
}

const ReversibleByteHit* FindByteHitAt(const std::vector<ReversibleByteHit>& v, int off) {
    for (const auto& h : v) if (h.offset == off) return &h;
    return nullptr;
}

std::vector<int> IntersectByteHitOffsets(const std::vector<ReversibleByteHit>& a,
                                         const std::vector<ReversibleByteHit>& b) {
    std::vector<int> out;
    for (const auto& x : a) if (FindByteHitAt(b, x.offset)) out.push_back(x.offset);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void PrintByteHitGroups(const std::vector<ReversibleByteHit>& hits, const char* label) {
    std::cout << "[" << label << "] reversible bytes=" << hits.size() << "\n";
    size_t shown = 0;
    for (const auto& h : hits) {
        std::cout << "  +" << HexU32(static_cast<uint32_t>(h.offset))
                  << " " << static_cast<unsigned>(h.base)
                  << " -> " << static_cast<unsigned>(h.active)
                  << " -> " << static_cast<unsigned>(h.restored) << "\n";
        if (++shown >= 32) {
            if (hits.size() > shown) std::cout << "  ... (" << (hits.size()-shown) << " more)\n";
            break;
        }
    }
}

bool CaptureCreatureLightCycle(uintptr_t player, size_t window,
                               int cycle, std::vector<ReversibleByteHit>& out) {
    std::vector<uint8_t> a,b,c;
    std::cout << "\n[CREATURE LIGHT] ciclo " << cycle << "/2.\n"
              << "Use de preferencia uma mudanca de luz que NAO mova o personagem.\n"
              << "Pode ser magia/fonte de luz; se usar item, nao altere mais nada.\n";
    WaitEnter("Deixe a luz no estado BASE/BAIXO e, quando a tela estabilizar, ENTER... ");
    if (!CaptureWindow(player,a,window)) return false;
    WaitEnter("Ative/aumente a luz e, quando a tela estabilizar, ENTER... ");
    if (!CaptureWindow(player,b,window)) return false;
    WaitEnter("Volte exatamente ao estado BASE/BAIXO e, quando estabilizar, ENTER... ");
    if (!CaptureWindow(player,c,window)) return false;
    out = FindReversibleByteHits(a,b,c);
    PrintByteHitGroups(out, cycle==1 ? "LIGHT CYCLE A" : "LIGHT CYCLE B");
    return true;
}

void PrintGetLightCodeUse(const Report& r, const std::vector<int>& runtimeOffsets) {
    auto it = r.binds.find("getLight");
    if (it == r.binds.end() || !it->second.selectedOk) return;
    const uintptr_t fn = r.module.base + it->second.selected.methodRva;
    const int thisReg = DetectThisRegister(fn);
    const auto offsets = CollectThisOffsets(fn,thisReg,256);
    std::cout << "[GETLIGHT CODE-USE] method=" << HexU32(it->second.selected.methodRva)
              << " thisReg=" << thisReg << " member refs=" << offsets.size() << "\n";
    for (const auto& kv : offsets) {
        if (kv.first < 0 || kv.first > 0x300) continue;
        const bool runtime = std::find(runtimeOffsets.begin(),runtimeOffsets.end(),kv.first) != runtimeOffsets.end();
        std::cout << "  +" << HexU32(static_cast<uint32_t>(kv.first))
                  << " refs=" << kv.second << (runtime?" [RUNTIME INTERSECTION]":"") << "\n";
    }
}


std::map<int,int> CollectThisOffsetsExtended(uintptr_t fn, int thisReg, size_t scan = 384) {
    auto counts = CollectThisOffsets(fn,thisReg,scan);
    std::vector<uint8_t> b(scan);
    if(!SafeCopy(fn,b.data(),b.size())) return counts;
    for(size_t i=0;i+7<b.size();++i) {
        if(b[i]!=0x0F) continue;
        const uint8_t op=b[i+1];
        if(!(op==0xB6 || op==0xB7 || op==0xBE || op==0xBF)) continue; // movzx/movsx
        const uint8_t modrm=b[i+2];
        const int mod=(modrm>>6)&3;
        const int rm=modrm&7;
        if(rm!=thisReg || mod==3) continue;
        int disp=0;
        if(mod==0) {
            if(rm==5) continue;
            disp=0;
        } else if(mod==1) {
            disp=static_cast<int8_t>(b[i+3]);
        } else if(mod==2) {
            int32_t d=0; std::memcpy(&d,b.data()+i+3,4); disp=d;
        }
        if(disp>=0 && disp<=0x300) counts[disp]++;
    }
    return counts;
}

struct AmbientTypedHit {
    std::string type;
    int offset=-1;
    double base=0.0;
    double active=0.0;
    double restored=0.0;
};

bool NearlySameAmbient(double a,double b,double eps=1e-6) {
    const double scale=std::max({1.0,std::fabs(a),std::fabs(b)});
    return std::fabs(a-b)<=eps*scale;
}

bool ReadScalarFromBytes(const std::vector<uint8_t>& b,int off,const std::string& type,double& out) {
    if(off<0 || static_cast<size_t>(off)>=b.size()) return false;
    if(type=="u8") {
        out=b[off]; return true;
    }
    if(type=="u16") {
        if(static_cast<size_t>(off)+2>b.size()) return false;
        uint16_t v=0; std::memcpy(&v,b.data()+off,2); out=v; return true;
    }
    if(type=="u32") {
        if(static_cast<size_t>(off)+4>b.size()) return false;
        uint32_t v=0; std::memcpy(&v,b.data()+off,4); out=v; return true;
    }
    if(type=="float") {
        if(static_cast<size_t>(off)+4>b.size()) return false;
        float v=0; std::memcpy(&v,b.data()+off,4); if(!std::isfinite(v)) return false; out=v; return true;
    }
    if(type=="double") {
        if(static_cast<size_t>(off)+8>b.size()) return false;
        double v=0; std::memcpy(&v,b.data()+off,8); if(!std::isfinite(v)) return false; out=v; return true;
    }
    return false;
}

std::vector<AmbientTypedHit> FindAmbientTypedHits(const std::vector<uint8_t>& a,
                                                   const std::vector<uint8_t>& b,
                                                   const std::vector<uint8_t>& c,
                                                   const std::vector<int>& semanticOffsets,
                                                   int excludeBegin=-1,
                                                   int excludeEnd=-1) {
    std::vector<AmbientTypedHit> out;
    std::vector<int> offsets=semanticOffsets;
    if(offsets.empty()) {
        const int n=static_cast<int>(std::min({a.size(),b.size(),c.size()}));
        for(int off=0;off<n;++off) offsets.push_back(off);
    }
    std::sort(offsets.begin(),offsets.end());
    offsets.erase(std::unique(offsets.begin(),offsets.end()),offsets.end());

    const std::vector<std::string> types={"u8","u16","u32","float","double"};
    for(int off:offsets) {
        if(off<0) continue;
        if(excludeBegin>=0 && off>=excludeBegin && off<=excludeEnd) continue;
        for(const auto& type:types) {
            if((type=="u16" && (off%2)!=0) ||
               ((type=="u32" || type=="float" || type=="double") && (off%4)!=0)) continue;
            double va=0,vb=0,vc=0;
            if(!ReadScalarFromBytes(a,off,type,va) || !ReadScalarFromBytes(b,off,type,vb) || !ReadScalarFromBytes(c,off,type,vc)) continue;
            const double eps=(type=="float" || type=="double")?1e-5:0.0;
            const bool restored = eps>0 ? NearlySameAmbient(va,vc,eps) : va==vc;
            const bool changed  = eps>0 ? !NearlySameAmbient(va,vb,eps) : va!=vb;
            if(!restored || !changed) continue;
            // Avoid pointer-like/raw gigantic integers when no semantic type is known.
            if((type=="u32") && (va>1000000.0 || vb>1000000.0 || vc>1000000.0)) continue;
            if((type=="float" || type=="double") &&
               (std::fabs(va)>1000000.0 || std::fabs(vb)>1000000.0 || std::fabs(vc)>1000000.0)) continue;
            out.push_back({type,off,va,vb,vc});
        }
    }
    return out;
}

bool SameAmbientKey(const AmbientTypedHit& a,const AmbientTypedHit& b) {
    return a.offset==b.offset && a.type==b.type;
}

std::vector<AmbientTypedHit> IntersectAmbientHits(const std::vector<AmbientTypedHit>& a,
                                                   const std::vector<AmbientTypedHit>& b) {
    std::vector<AmbientTypedHit> out;
    for(const auto& x:a) {
        for(const auto& y:b) {
            if(!SameAmbientKey(x,y)) continue;
            const double eps=(x.type=="float" || x.type=="double")?1e-4:0.0;
            const bool sameBase = eps>0 ? NearlySameAmbient(x.base,y.base,eps) : x.base==y.base;
            if(sameBase) out.push_back(x);
            break;
        }
    }
    return out;
}

void PrintAmbientHits(const std::vector<AmbientTypedHit>& hits,const char* label) {
    std::cout << "[" << label << "] candidates=" << hits.size() << "\n";
    for(size_t i=0;i<hits.size() && i<40;++i) {
        const auto& h=hits[i];
        std::cout << "  " << h.type << " +" << HexU32(static_cast<uint32_t>(h.offset))
                  << " " << h.base << " -> " << h.active << " -> " << h.restored << "\n";
    }
    if(hits.size()>40) std::cout << "  ... (" << (hits.size()-40) << " more)\n";
}

bool CaptureWorldAmbientCycle(uintptr_t owner,size_t window,int cycle,
                              const std::vector<int>& semanticOffsets,
                              int excludeBegin,int excludeEnd,
                              std::vector<AmbientTypedHit>& out) {
    std::vector<uint8_t> a,b,c;
    std::cout << "\n[WORLD AMBIENT] ciclo " << cycle << "/2.\n"
              << "Este teste e da LUZ DO AMBIENTE, nao da Creature.\n"
              << "Pode mudar de andar/area se isso for necessario para trocar a iluminacao.\n";
    WaitEnter("Fique no ambiente ESCURO/BASE e espere estabilizar. ENTER... ");
    if(!CaptureWindow(owner,a,window)) return false;
    WaitEnter("Va para o ambiente MAIS CLARO e espere estabilizar. ENTER... ");
    if(!CaptureWindow(owner,b,window)) return false;
    WaitEnter("Volte ao mesmo ambiente ESCURO/BASE e espere estabilizar. ENTER... ");
    if(!CaptureWindow(owner,c,window)) return false;
    out=FindAmbientTypedHits(a,b,c,semanticOffsets,excludeBegin,excludeEnd);
    PrintAmbientHits(out,cycle==1?"AMBIENT CYCLE A":"AMBIENT CYCLE B");
    return true;
}

bool SelectAmbientUniqueOffset(const std::vector<AmbientTypedHit>& in,AmbientTypedHit& out) {
    if(in.empty()) return false;
    std::set<int> offsets;
    for(const auto& h:in) offsets.insert(h.offset);
    if(offsets.size()!=1) return false;
    const int off=*offsets.begin();
    const std::vector<std::string> priority={"u8","u16","float","u32","double"};
    for(const auto& t:priority) for(const auto& h:in) if(h.offset==off && h.type==t) { out=h; return true; }
    out=in.front(); return true;
}


struct UiMapWrapperDecodedV13 {
    bool ok = false;
    int outer = -1; // UIMap + outer -> MapView*
    int inner = -1; // MapView + inner -> field
};

bool DecodeOuterMapViewLoadV13(uintptr_t fn, int& outer, int& mapReg, size_t& after) {
    std::array<uint8_t, 48> b{};
    if (!SafeCopy(fn, b.data(), b.size())) return false;
    for (size_t i = 0; i + 6 <= b.size() && i < 20; ++i) {
        if (b[i] != 0x8B) continue;
        const uint8_t mr = b[i + 1];
        const int mod = (mr >> 6) & 3;
        const int rm = mr & 7;
        if (mod != 2 || rm != 1) continue; // [ECX+disp32]
        int32_t d = 0;
        std::memcpy(&d, b.data() + i + 2, 4);
        if (d < 0 || d > 0x2000) continue;
        outer = d;
        mapReg = (mr >> 3) & 7;
        after = i + 6;
        return true;
    }
    return false;
}

UiMapWrapperDecodedV13 DecodeMinimumAmbientSetterV13(uintptr_t fn) {
    UiMapWrapperDecodedV13 out;
    int outer=-1, mapReg=-1; size_t after=0;
    if (!DecodeOuterMapViewLoadV13(fn, outer, mapReg, after)) return out;
    std::array<uint8_t, 64> b{};
    if (!SafeCopy(fn, b.data(), b.size())) return out;
    bool sawStackFloat=false;
    for (size_t i=0; i+5<=b.size(); ++i) {
        if (b[i]==0xF3 && b[i+1]==0x0F && b[i+2]==0x10 && b[i+3]==0x45 && b[i+4]==0x08)
            sawStackFloat=true;
    }
    if (!sawStackFloat) return out;
    for (size_t i=after; i+8<=b.size() && i<48; ++i) {
        if (!(b[i]==0xF3 && b[i+1]==0x0F && b[i+2]==0x11)) continue;
        const uint8_t mr=b[i+3];
        if (((mr>>6)&3)!=2 || (mr&7)!=mapReg) continue;
        int32_t d=0; std::memcpy(&d,b.data()+i+4,4);
        if (d<0 || d>0x3000) continue;
        out.ok=true; out.outer=outer; out.inner=d; return out;
    }
    return out;
}

UiMapWrapperDecodedV13 DecodeMinimumAmbientGetterV13(uintptr_t fn) {
    UiMapWrapperDecodedV13 out;
    int outer=-1, mapReg=-1; size_t after=0;
    if (!DecodeOuterMapViewLoadV13(fn, outer, mapReg, after)) return out;
    std::array<uint8_t, 48> b{};
    if (!SafeCopy(fn, b.data(), b.size())) return out;
    for (size_t i=after; i+6<=b.size() && i<36; ++i) {
        if (b[i]==0xD9) {
            const uint8_t mr=b[i+1];
            if (((mr>>6)&3)==2 && ((mr>>3)&7)==0 && (mr&7)==mapReg) {
                int32_t d=0; std::memcpy(&d,b.data()+i+2,4);
                if (d>=0 && d<=0x3000) { out.ok=true; out.outer=outer; out.inner=d; return out; }
            }
        }
        if (i+8<=b.size() && b[i]==0xF3 && b[i+1]==0x0F && b[i+2]==0x10) {
            const uint8_t mr=b[i+3];
            if (((mr>>6)&3)==2 && (mr&7)==mapReg) {
                int32_t d=0; std::memcpy(&d,b.data()+i+4,4);
                if (d>=0 && d<=0x3000) { out.ok=true; out.outer=outer; out.inner=d; return out; }
            }
        }
    }
    return out;
}

UiMapWrapperDecodedV13 DecodeDrawLightsSetterV13(uintptr_t fn) {
    UiMapWrapperDecodedV13 out;
    int outer=-1, mapReg=-1; size_t after=0;
    if (!DecodeOuterMapViewLoadV13(fn, outer, mapReg, after)) return out;
    std::array<uint8_t, 64> b{};
    if (!SafeCopy(fn, b.data(), b.size())) return out;
    int valueReg=-1;
    for (size_t i=0;i+3<b.size() && i<40;++i) {
        if (b[i]!=0x8A) continue;
        const uint8_t mr=b[i+1];
        if (((mr>>6)&3)==1 && (mr&7)==5 && b[i+2]==0x08) {
            valueReg=(mr>>3)&7;
            break;
        }
    }
    if (valueReg<0 || valueReg>3) return out; // AL/CL/DL/BL only
    for (size_t i=after;i+6<=b.size() && i<48;++i) {
        if (b[i]!=0x88) continue;
        const uint8_t mr=b[i+1];
        if (((mr>>6)&3)!=2 || (mr&7)!=mapReg || ((mr>>3)&7)!=valueReg) continue;
        int32_t d=0; std::memcpy(&d,b.data()+i+2,4);
        if (d<0 || d>0x3000) continue;
        out.ok=true; out.outer=outer; out.inner=d; return out;
    }
    return out;
}

UiMapWrapperDecodedV13 DecodeDrawLightsGetterV13(uintptr_t fn) {
    UiMapWrapperDecodedV13 out;
    int outer=-1, mapReg=-1; size_t after=0;
    if (!DecodeOuterMapViewLoadV13(fn, outer, mapReg, after)) return out;
    std::array<uint8_t, 48> b{};
    if (!SafeCopy(fn, b.data(), b.size())) return out;
    for (size_t i=after;i+6<=b.size() && i<36;++i) {
        if (b[i]!=0x8A) continue;
        const uint8_t mr=b[i+1];
        if (((mr>>6)&3)!=2 || (mr&7)!=mapReg) continue;
        int32_t d=0; std::memcpy(&d,b.data()+i+2,4);
        if (d<0 || d>0x3000) continue;
        out.ok=true; out.outer=outer; out.inner=d; return out;
    }
    return out;
}

struct UiMapPatternMethodV13 {
    uint32_t rva=0;
    int outer=-1;
    int inner=-1;
};

// Structural fallback: scan current .text for the tiny wrapper shapes and pair
// byte + float getter/setter contracts. No historical offset is used.
void ScanUiMapLightWrapperShapesV13(const ModuleView& m,
                                    std::vector<UiMapPatternMethodV13>& floatSet,
                                    std::vector<UiMapPatternMethodV13>& floatGet,
                                    std::vector<UiMapPatternMethodV13>& byteSet,
                                    std::vector<UiMapPatternMethodV13>& byteGet) {
    const SectionInfo* t=m.text();
    if(!t || t->end<=t->start) return;
    const size_t n=static_cast<size_t>(t->end-t->start);
    std::vector<uint8_t> b(n);
    if(!SafeCopy(t->start,b.data(),b.size())) return;

    for(size_t i=0;i+40<n;++i) {
        if(b[i]!=0x8B || (b[i+1]&0xC7)!=0x81) continue; // mov reg,[ecx+disp32]
        const int mapReg=(b[i+1]>>3)&7;
        int32_t outer=0; std::memcpy(&outer,b.data()+i+2,4);
        if(outer<0 || outer>0x2000) continue;
        size_t fnStart=i;
        if(i>=3 && b[i-3]==0x55 && b[i-2]==0x8B && b[i-1]==0xEC) fnStart=i-3;
        const uint32_t rva=t->rva+static_cast<uint32_t>(fnStart);
        size_t end=std::min(n,i+34);
        for(size_t k=i+6;k<end;++k) {
            if(b[k]==0xC3 || b[k]==0xC2) { end=k; break; }
        }
        for(size_t j=i+6;j<end;++j) {
            if(j+8<=end && b[j]==0xF3 && b[j+1]==0x0F && b[j+2]==0x11) {
                const uint8_t mr=b[j+3];
                if(((mr>>6)&3)==2 && (mr&7)==mapReg) {
                    int32_t inner=0; std::memcpy(&inner,b.data()+j+4,4);
                    if(inner>=0 && inner<=0x3000) floatSet.push_back({rva,outer,inner});
                }
            }
            if(j+6<=end && b[j]==0xD9) {
                const uint8_t mr=b[j+1];
                if(((mr>>6)&3)==2 && ((mr>>3)&7)==0 && (mr&7)==mapReg) {
                    int32_t inner=0; std::memcpy(&inner,b.data()+j+2,4);
                    if(inner>=0 && inner<=0x3000) floatGet.push_back({rva,outer,inner});
                }
            }
            if(j+6<=end && b[j]==0x88) {
                const uint8_t mr=b[j+1];
                if(((mr>>6)&3)==2 && (mr&7)==mapReg) {
                    int32_t inner=0; std::memcpy(&inner,b.data()+j+2,4);
                    if(inner>=0 && inner<=0x3000) byteSet.push_back({rva,outer,inner});
                }
            }
            if(j+6<=end && b[j]==0x8A) {
                const uint8_t mr=b[j+1];
                if(((mr>>6)&3)==2 && (mr&7)==mapReg) {
                    int32_t inner=0; std::memcpy(&inner,b.data()+j+2,4);
                    if(inner>=0 && inner<=0x3000) byteGet.push_back({rva,outer,inner});
                }
            }
        }
    }
}

bool SameWrapperFieldV13(const UiMapPatternMethodV13& a,const UiMapPatternMethodV13& b) {
    return a.outer==b.outer && a.inner==b.inner;
}

bool ResolveUiMapContractByShapesV13(const ModuleView& m,
                                     UiMapPatternMethodV13& fs,UiMapPatternMethodV13& fg,
                                     UiMapPatternMethodV13& bs,UiMapPatternMethodV13& bg) {
    std::vector<UiMapPatternMethodV13> fsets,fgets,bsets,bgets;
    ScanUiMapLightWrapperShapesV13(m,fsets,fgets,bsets,bgets);
    struct Combo { UiMapPatternMethodV13 fs,fg,bs,bg; int score=0; };
    std::vector<Combo> combos;
    for(const auto& a:fsets) for(const auto& b:fgets) {
        if(!SameWrapperFieldV13(a,b)) continue;
        for(const auto& c:bsets) for(const auto& d:bgets) {
            if(!SameWrapperFieldV13(c,d)) continue;
            if(a.outer!=c.outer) continue;
            int score=100;
            if(a.inner==c.inner+4) score+=100;
            const uint32_t lo=std::min(std::min(a.rva,b.rva),std::min(c.rva,d.rva));
            const uint32_t hi=std::max(std::max(a.rva,b.rva),std::max(c.rva,d.rva));
            if(hi-lo<=0x1000) score+=80;
            else if(hi-lo<=0x4000) score+=20;
            combos.push_back({a,b,c,d,score});
        }
    }
    if(combos.empty()) return false;
    std::sort(combos.begin(),combos.end(),[](const Combo& x,const Combo& y){
        if(x.score!=y.score) return x.score>y.score;
        if(x.fs.outer!=y.fs.outer) return x.fs.outer<y.fs.outer;
        return x.fs.inner<y.fs.inner;
    });
    if(combos.size()>1 && combos[0].score==combos[1].score &&
       (combos[0].fs.outer!=combos[1].fs.outer || combos[0].fs.inner!=combos[1].fs.inner || combos[0].bs.inner!=combos[1].bs.inner))
        return false;
    fs=combos[0].fs; fg=combos[0].fg; bs=combos[0].bs; bg=combos[0].bg;
    return true;
}

void RunUiMapLightStaticDiscoveryV13(Report& r) {
    std::cout << "\n============================================================\n"
              << "          UIMAP -> MAPVIEW LIGHT CONTRACT V30\n"
              << "============================================================\n"
              << "[RULE] offsets antigos NAO sao copiados; wrappers da build atual sao decodificados.\n";

    auto method=[&](const char* n)->uint32_t {
        auto it=r.binds.find(n);
        return (it!=r.binds.end() && it->second.selectedOk)?it->second.selected.methodRva:0u;
    };
    uint32_t setMin=method("setMinimumAmbientLight");
    uint32_t getMin=method("getMinimumAmbientLight");
    uint32_t setDraw=method("setDrawLights");
    uint32_t getDraw=method("isDrawingLights");

    UiMapWrapperDecodedV13 sm,gm,sd,gd;
    if(setMin) sm=DecodeMinimumAmbientSetterV13(r.module.base+setMin);
    if(getMin) gm=DecodeMinimumAmbientGetterV13(r.module.base+getMin);
    if(setDraw) sd=DecodeDrawLightsSetterV13(r.module.base+setDraw);
    if(getDraw) gd=DecodeDrawLightsGetterV13(r.module.base+getDraw);

    bool minAgree=sm.ok && gm.ok && sm.outer==gm.outer && sm.inner==gm.inner;
    bool drawAgree=sd.ok && gd.ok && sd.outer==gd.outer && sd.inner==gd.inner;

    if(!(minAgree && drawAgree && sm.outer==sd.outer)) {
        UiMapPatternMethodV13 fs,fg,bs,bg;
        if(ResolveUiMapContractByShapesV13(r.module,fs,fg,bs,bg)) {
            std::cout << "[LIGHT STATIC FALLBACK] wrapper-shape quartet unico encontrado.\n";
            if(!setMin) setMin=fs.rva;
            if(!getMin) getMin=fg.rva;
            if(!setDraw) setDraw=bs.rva;
            if(!getDraw) getDraw=bg.rva;
            sm={true,fs.outer,fs.inner}; gm={true,fg.outer,fg.inner};
            sd={true,bs.outer,bs.inner}; gd={true,bg.outer,bg.inner};
            minAgree=true; drawAgree=true;
        }
    }

    std::cout << "[LIGHT METHODS]\n"
              << "  setDrawLights            : " << (setDraw?HexU32(setDraw):std::string("<pending>")) << "\n"
              << "  isDrawingLights          : " << (getDraw?HexU32(getDraw):std::string("<pending>")) << "\n"
              << "  setMinimumAmbientLight   : " << (setMin?HexU32(setMin):std::string("<pending>")) << "\n"
              << "  getMinimumAmbientLight   : " << (getMin?HexU32(getMin):std::string("<pending>")) << "\n";

    if(minAgree) {
        std::cout << "[MINIMUM AMBIENT] setter/getter agree: UIMap +" << HexU32(sm.outer)
                  << " -> MapView*, MapView +" << HexU32(sm.inner) << " float\n";
    }
    if(drawAgree) {
        std::cout << "[DRAW LIGHTS] setter/getter agree: UIMap +" << HexU32(sd.outer)
                  << " -> MapView*, MapView +" << HexU32(sd.inner) << " u8\n";
    }

    if(minAgree && drawAgree && sm.outer==sd.outer) {
        r.worldLight.setMinimumAmbientLightRva=setMin;
        r.worldLight.getMinimumAmbientLightRva=getMin;
        r.worldLight.setDrawLightsRva=setDraw;
        r.worldLight.isDrawingLightsRva=getDraw;
        r.worldLight.uiMapToMapView=sm.outer;
        r.worldLight.minimumAmbient.type="float";
        r.worldLight.minimumAmbient.offset=sm.inner;
        r.worldLight.minimumAmbient.status="semantic_uimap_mapview_setter_getter_minimum_ambient";
        r.worldLight.drawLights.type="u8";
        r.worldLight.drawLights.offset=sd.inner;
        r.worldLight.drawLights.status="semantic_uimap_mapview_setter_getter_draw_lights";
        r.worldLight.staticContractStatus="semantic_uimap_mapview_setter_getter_contract";
        r.worldLight.status="semantic_uimap_mapview_setter_getter_contract";
        r.worldLight.candidateA=sd.inner;
        r.worldLight.candidateB=sm.inner;
        std::cout << "[LIGHT CONTRACT] CONFIRMADO pela build atual";
        if(sm.inner==sd.inner+4) std::cout << " (drawLights imediatamente antes do float ambient)";
        std::cout << ".\n";
        SaveCheckpoint(r,"uimap_mapview_light_contract");
        return;
    }

    if(minAgree) {
        r.worldLight.setMinimumAmbientLightRva=setMin;
        r.worldLight.getMinimumAmbientLightRva=getMin;
        r.worldLight.uiMapToMapView=sm.outer;
        r.worldLight.minimumAmbient.type="float";
        r.worldLight.minimumAmbient.offset=sm.inner;
        r.worldLight.minimumAmbient.status="semantic_uimap_mapview_partial_minimum_ambient";
        r.worldLight.staticContractStatus="semantic_uimap_mapview_partial";
        r.worldLight.status="semantic_uimap_mapview_partial";
        SaveCheckpoint(r,"uimap_mapview_light_partial");
    } else {
        std::cout << "[LIGHT CONTRACT] ainda nao foi possivel fechar setter/getter do minimumAmbientLight.\n";
    }
}

void RunWorldAmbientLightDiscovery(Report& r) {
    std::cout << "\n============================================================\n"
              << "               WORLD / AMBIENT LIGHT V25\n"
              << "============================================================\n";
    if(EvidenceScore(r.worldLight.staticContractStatus)>=80 &&
       r.worldLight.uiMapToMapView>=0 && r.worldLight.minimumAmbient.offset>=0) {
        std::cout << "[WORLD LIGHT] contrato estatico ja resolvido; nenhum teste manual escuro/claro necessario.\n"
                  << "  UIMap -> MapView : +" << HexU32(static_cast<uint32_t>(r.worldLight.uiMapToMapView)) << "\n"
                  << "  drawLights       : " << (r.worldLight.drawLights.offset>=0?("+"+HexU32(static_cast<uint32_t>(r.worldLight.drawLights.offset))):std::string("<pending>")) << "\n"
                  << "  minimumAmbient   : +" << HexU32(static_cast<uint32_t>(r.worldLight.minimumAmbient.offset)) << " (float)\n"
                  << "[MODE] READ-ONLY: nenhum valor sera escrito nesta etapa.\n";
        return;
    }
    std::cout << "[WORLD LIGHT] contrato ainda parcial. V18 manteve somente evidencia semantica/estrutural segura.\n";
}


void RunCreatureLightDiscovery(Report& r) {
    std::cout << "\n============================================================\n"
              << "             CREATURE / LOCALPLAYER LIGHT V11\n"
              << "============================================================\n";
    if (!r.localPlayerPtr) {
        std::cout << "[LIGHT] LocalPlayer ausente; discovery adiado.\n";
        return;
    }

    if (r.creatureLayout.lightLevel.offset >= 0 &&
        EvidenceScore(r.creatureLayout.lightLevel.status) >= 80) {
        uint8_t v=0;
        const bool ok = SafeRead(r.localPlayerPtr + r.creatureLayout.lightLevel.offset, v);
        std::cout << "[LIGHT QUICK-CHECK] level +" << HexU32(r.creatureLayout.lightLevel.offset)
                  << " u8 -> " << (ok?std::to_string(static_cast<unsigned>(v)):std::string("<read failed>"))
                  << " [" << r.creatureLayout.lightLevel.status << "]\n";
        if (r.creatureLayout.lightColor.offset >= 0) {
            uint8_t c=0; const bool cok=SafeRead(r.localPlayerPtr+r.creatureLayout.lightColor.offset,c);
            std::cout << "[LIGHT QUICK-CHECK] color +" << HexU32(r.creatureLayout.lightColor.offset)
                      << " u8 -> " << (cok?std::to_string(static_cast<unsigned>(c)):std::string("<read failed>"))
                      << " [" << r.creatureLayout.lightColor.status << "]\n";
        }
        std::cout << "[SKIP] Creature light ja confirmado no profile desta fingerprint.\n";
        return;
    }

    if (AskYesNo("Mapear Creature/LocalPlayer Light por duas transicoes reversiveis agora?") != 's') {
        std::cout << "[LIGHT] teste pulado; profile preservado.\n";
        return;
    }

    size_t window = 0x200;
    std::vector<ReversibleByteHit> c1,c2;
    if (!CaptureCreatureLightCycle(r.localPlayerPtr,window,1,c1)) return;
    if (c1.empty() && AskYesNo("Nenhum byte reversivel em 0x200. Expandir uma unica vez para 0x400?")=='s') {
        window=0x400;
        if (!CaptureCreatureLightCycle(r.localPlayerPtr,window,1,c1)) return;
    }
    if (c1.empty()) {
        std::cout << "[LIGHT] nenhuma transicao reversivel encontrada; mantendo pending.\n";
        return;
    }

    if (!CaptureCreatureLightCycle(r.localPlayerPtr,window,2,c2)) return;
    const auto common = IntersectByteHitOffsets(c1,c2);
    std::cout << "[LIGHT TWO-CYCLE] common offsets=" << common.size() << "\n";
    for (int off : common) {
        const auto* a=FindByteHitAt(c1,off); const auto* b=FindByteHitAt(c2,off);
        std::cout << "  +" << HexU32(static_cast<uint32_t>(off));
        if(a) std::cout << " C1=" << static_cast<unsigned>(a->base) << "->" << static_cast<unsigned>(a->active) << "->" << static_cast<unsigned>(a->restored);
        if(b) std::cout << " C2=" << static_cast<unsigned>(b->base) << "->" << static_cast<unsigned>(b->active) << "->" << static_cast<unsigned>(b->restored);
        std::cout << "\n";
    }
    PrintGetLightCodeUse(r,common);

    if (common.size()==1) {
        r.creatureLayout.lightLevel = {"u8",common[0],"runtime_reversible_light_two_cycles"};
        r.creatureLayout.lightCandidateA = common[0];
        r.creatureLayout.lightCandidateB = -1;
        r.creatureLayout.lightStatus = "runtime_reversible_light_two_cycles_unique";
        std::cout << "[LIGHT LEVEL] promoted u8 +" << HexU32(common[0])
                  << " por duas transicoes reversiveis.\n";
        SaveCheckpoint(r,"creature_light_level");
        return;
    }

    if (common.size()==2 && std::abs(common[0]-common[1])==1) {
        r.creatureLayout.lightCandidateA=common[0];
        r.creatureLayout.lightCandidateB=common[1];
        r.creatureLayout.lightStatus="runtime_reversible_light_pair_two_cycles_unlabeled";
        std::cout << "[LIGHT PAIR] dois bytes adjacentes confirmados +" << HexU32(common[0])
                  << " / +" << HexU32(common[1])
                  << ". Nao rotulados como level/color sem evidencia adicional.\n";
        SaveCheckpoint(r,"creature_light_pair");
        return;
    }

    r.creatureLayout.lightStatus="runtime_reversible_light_ambiguous";
    std::cout << "[LIGHT] transicao real detectada, mas ainda ambigua. Nenhum offset foi inventado.\n";
    SaveCheckpoint(r,"creature_light_pending");
}


struct V18Accessor {
    int slot = -1;
    uint32_t functionRva = 0;
    std::string type;
    int offset = -1;
    bool getter = false;
    bool setter = false;
};

bool DecodeSimpleGetterV18(uintptr_t fn, V18Accessor& out) {
    uint8_t b[16]{};
    for (size_t i=0;i<sizeof(b);++i) if(!SafeRead(fn+i,b[i])) return false;

    // mov eax,[ecx+disp8] ; ret
    if (b[0]==0x8B && b[1]==0x41 && b[3]==0xC3) {
        out.type="u32"; out.offset=static_cast<int>(b[2]); out.getter=true; return true;
    }
    // mov eax,[ecx+disp32] ; ret
    if (b[0]==0x8B && b[1]==0x81 && b[6]==0xC3) {
        int32_t d=0; std::memcpy(&d,b+2,4);
        if(d>=0 && d<=0x2000) { out.type="u32"; out.offset=d; out.getter=true; return true; }
    }
    // movzx eax,byte ptr [ecx+disp8] ; ret
    if (b[0]==0x0F && b[1]==0xB6 && b[2]==0x41 && b[4]==0xC3) {
        out.type="u8"; out.offset=static_cast<int>(b[3]); out.getter=true; return true;
    }
    // movzx eax,byte ptr [ecx+disp32] ; ret
    if (b[0]==0x0F && b[1]==0xB6 && b[2]==0x81 && b[7]==0xC3) {
        int32_t d=0; std::memcpy(&d,b+3,4);
        if(d>=0 && d<=0x2000) { out.type="u8"; out.offset=d; out.getter=true; return true; }
    }
    // movzx eax,word ptr [ecx+disp8] ; ret
    if (b[0]==0x0F && b[1]==0xB7 && b[2]==0x41 && b[4]==0xC3) {
        out.type="u16"; out.offset=static_cast<int>(b[3]); out.getter=true; return true;
    }
    // movzx eax,word ptr [ecx+disp32] ; ret
    if (b[0]==0x0F && b[1]==0xB7 && b[2]==0x81 && b[7]==0xC3) {
        int32_t d=0; std::memcpy(&d,b+3,4);
        if(d>=0 && d<=0x2000) { out.type="u16"; out.offset=d; out.getter=true; return true; }
    }
    // mov al,[ecx+disp8] ; ret
    if (b[0]==0x8A && b[1]==0x41 && b[3]==0xC3) {
        out.type="u8"; out.offset=static_cast<int>(b[2]); out.getter=true; return true;
    }
    // mov al,[ecx+disp32] ; ret
    if (b[0]==0x8A && b[1]==0x81 && b[6]==0xC3) {
        int32_t d=0; std::memcpy(&d,b+2,4);
        if(d>=0 && d<=0x2000) { out.type="u8"; out.offset=d; out.getter=true; return true; }
    }
    return false;
}

bool DecodeSimpleSetterU32V18(uintptr_t fn, int& offset) {
    uint8_t b[32]{};
    for (size_t i=0;i<sizeof(b);++i) if(!SafeRead(fn+i,b[i])) return false;

    bool argLoaded=false;
    for(size_t i=0;i+3<sizeof(b);++i) {
        if(b[i]==0x8B && b[i+1]==0x45 && b[i+2]==0x08) argLoaded=true; // mov eax,[ebp+8]
        if(!argLoaded) continue;
        if(b[i]==0x89 && b[i+1]==0x41) {
            offset=static_cast<int>(b[i+2]); return true;
        }
        if(i+5<sizeof(b) && b[i]==0x89 && b[i+1]==0x81) {
            int32_t d=0; std::memcpy(&d,b+i+2,4);
            if(d>=0 && d<=0x2000) { offset=d; return true; }
        }
    }
    return false;
}

std::vector<V18Accessor> EnumerateSimpleVtableAccessorsV18(const Report& r,
                                                           const std::string& cls,
                                                           int maxSlots=64) {
    std::vector<V18Accessor> out;
    const auto it=r.rtti.find(cls);
    if(it==r.rtti.end() || it->second.vtableRvas.empty()) return out;
    const uintptr_t vt=r.module.base + it->second.vtableRvas.front();

    for(int slot=0; slot<maxSlots; ++slot) {
        uint32_t fn=0;
        if(!SafeRead(vt + static_cast<uintptr_t>(slot)*4,fn) || !r.module.inText(fn)) continue;
        V18Accessor a; a.slot=slot; a.functionRva=r.module.rva(fn);
        if(DecodeSimpleGetterV18(fn,a)) out.push_back(a);
    }
    return out;
}

const V18Accessor* FindAccessorSlotV18(const std::vector<V18Accessor>& v, int slot) {
    for(const auto& a:v) if(a.slot==slot) return &a;
    return nullptr;
}

struct V18IdCandidate {
    int slot=-1;
    int offset=-1;
    uint32_t getterRva=0;
    int score=0;
    bool commonFour=false;
    bool setterPair=false;
    bool adjacentName=false;
};

std::vector<V18IdCandidate> FindCreatureIdCandidatesV18(Report& r) {
    const auto lp=EnumerateSimpleVtableAccessorsV18(r,"LocalPlayer");
    const auto pl=EnumerateSimpleVtableAccessorsV18(r,"Player");
    const auto mo=EnumerateSimpleVtableAccessorsV18(r,"Monster");
    const auto np=EnumerateSimpleVtableAccessorsV18(r,"Npc");

    std::vector<V18IdCandidate> out;
    const auto itMon=r.rtti.find("Monster");
    if(itMon==r.rtti.end() || itMon->second.vtableRvas.empty()) return out;
    const uintptr_t monVt=r.module.base+itMon->second.vtableRvas.front();

    for(const auto& m:mo) {
        if(!m.getter || m.type!="u32" || m.offset<0 || m.offset>0x400) continue;
        const auto* a=FindAccessorSlotV18(lp,m.slot);
        const auto* b=FindAccessorSlotV18(pl,m.slot);
        const auto* c=FindAccessorSlotV18(np,m.slot);
        if(!a || !b || !c) continue;
        if(a->type!="u32" || b->type!="u32" || c->type!="u32") continue;
        if(a->offset!=m.offset || b->offset!=m.offset || c->offset!=m.offset) continue;

        V18IdCandidate k;
        k.slot=m.slot; k.offset=m.offset; k.getterRva=m.functionRva;
        k.commonFour=true; k.score+=70;

        if(m.slot>0) {
            uint32_t prevFn=0;
            if(SafeRead(monVt+static_cast<uintptr_t>(m.slot-1)*4,prevFn) && r.module.inText(prevFn)) {
                int setOff=-1;
                if(DecodeSimpleSetterU32V18(prevFn,setOff) && setOff==m.offset) {
                    k.setterPair=true; k.score+=70;
                }
            }
        }
        if(r.creatureLayout.nameOffset>=0 && m.offset+4==r.creatureLayout.nameOffset) {
            k.adjacentName=true; k.score+=30;
        }

        // Known fields are not ID candidates.
        if(m.offset==r.creatureLayout.x || m.offset==r.creatureLayout.y ||
           m.offset==r.creatureLayout.z || m.offset==r.creatureLayout.direction ||
           m.offset==r.creatureLayout.hpPercent.offset) {
            continue;
        }

        // IDs are scalar values; do not treat the vtable pointer itself as a field.
        if(m.offset==0) continue;
        out.push_back(k);
    }

    std::sort(out.begin(),out.end(),[](const V18IdCandidate& x,const V18IdCandidate& y){
        if(x.score!=y.score) return x.score>y.score;
        if(x.setterPair!=y.setterPair) return x.setterPair>y.setterPair;
        return x.offset<y.offset;
    });
    return out;
}

bool ReadStableU32V18(uintptr_t object, int offset, uint32_t& value) {
    uint32_t a=0,b=0;
    if(!object || offset<0 || !SafeRead(object+offset,a)) return false;
    Sleep(120);
    if(!SafeRead(object+offset,b)) return false;
    if(a!=b) return false;
    value=a;
    return true;
}

void PrintCreatureAccessorReconV18(Report& r,
                                   const std::vector<V18IdCandidate>& candidates) {
    std::cout << "\n============================================================\n"
              << "          CREATURE ACCESSOR / ID CONTRACT V30\n"
              << "============================================================\n"
              << "[METHOD] RTTI Creature-family vtables -> simple field getters/setters.\n"
              << "[NOTE] Map::getCreatureById confirma a semantica uint32 ID, mas o Map pode indexar por chave;\n"
              << "       por isso o offset da Creature e promovido somente com accessor + runtime em dois alvos.\n";
    const auto bit=r.binds.find("getCreatureById");
    if(bit!=r.binds.end() && bit->second.selectedOk)
        std::cout << "[MAP::getCreatureById] RVA " << HexU32(bit->second.selected.methodRva) << "\n";

    std::cout << "[COMMON U32 ACCESSOR CANDIDATES] " << candidates.size() << "\n";
    for(const auto& c:candidates) {
        std::cout << "  slot=" << c.slot
                  << " getter=" << HexU32(c.getterRva)
                  << " field=+" << HexU32(static_cast<uint32_t>(c.offset))
                  << " score=" << c.score
                  << " four_classes=" << (c.commonFour?"YES":"NO")
                  << " setter_pair=" << (c.setterPair?"YES":"NO")
                  << " name_adjacent=" << (c.adjacentName?"YES":"NO")
                  << "\n";
    }
}

void RunCreatureIdDiscoveryV18(Report& r) {
    const auto candidates=FindCreatureIdCandidatesV18(r);
    PrintCreatureAccessorReconV18(r,candidates);

    if(r.creatureLayout.id.offset>=0 &&
       EvidenceScore(r.creatureLayout.id.status)>=80) {
        uintptr_t t=AcquireCreatureFromAttackTarget(r,false);
        if(t) {
            uint32_t id=0;
            const bool ok=ReadStableU32V18(t,r.creatureLayout.id.offset,id);
            std::cout << "[CREATURE ID QUICK-CHECK] +" << HexU32(static_cast<uint32_t>(r.creatureLayout.id.offset))
                      << " u32 -> " << (ok?std::to_string(id):std::string("<unstable/read failed>"))
                      << " [" << r.creatureLayout.id.status << "]\n";
        } else {
            std::cout << "[CREATURE ID] ja confirmado no profile; sem target ativo para quick-check.\n";
        }
        return;
    }

    if(candidates.empty()) {
        std::cout << "[CREATURE ID] nenhum accessor u32 comum seguro; mantendo pending.\n";
        return;
    }

    // Require a strong semantic shape before asking the user for two targets.
    std::vector<V18IdCandidate> strong;
    for(const auto& c:candidates) if(c.setterPair && c.commonFour) strong.push_back(c);
    if(strong.empty()) {
        std::cout << "[CREATURE ID] candidatos existem, mas nenhum tem getter/setter pareado nas classes Creature. Mantendo pending.\n";
        return;
    }

    if(AskYesNo("Validar Creature.ID automaticamente com DOIS monstros diferentes agora?")!='s') {
        std::cout << "[CREATURE ID] runtime pulado; candidatos semanticos preservados apenas no console.\n";
        return;
    }

    uintptr_t targetA=AcquireCreatureFromAttackTarget(r,true);
    if(!targetA) {
        std::cout << "[CREATURE ID] Monster A nao disponivel.\n";
        return;
    }

    struct RuntimeIdHit { V18IdCandidate c; uint32_t a=0; uint32_t b=0; };
    std::vector<RuntimeIdHit> hits;
    for(const auto& c:strong) {
        uint32_t v=0;
        if(ReadStableU32V18(targetA,c.offset,v) && v!=0 && v!=0xFFFFFFFFu)
            hits.push_back({c,v,0});
    }

    std::cout << "[ID TARGET A] ptr=" << HexU32(static_cast<uint32_t>(targetA))
              << " candidates=" << hits.size() << "\n";
    for(const auto& h:hits)
        std::cout << "  +" << HexU32(static_cast<uint32_t>(h.c.offset))
                  << " u32=" << h.a
                  << " slot=" << h.c.slot
                  << " score=" << h.c.score << "\n";

    if(hits.empty()) {
        std::cout << "[CREATURE ID] nenhum candidato estavel no primeiro monstro.\n";
        return;
    }

    WaitEnter("Agora ataque OUTRO monstro diferente e mantenha o target. Depois ENTER... ");
    uintptr_t targetB=AcquireCreatureFromAttackTarget(r,false);
    if(!targetB || targetB==targetA) {
        std::cout << "[CREATURE ID] o segundo target nao mudou; validacao descartada.\n";
        return;
    }

    std::vector<RuntimeIdHit> survived;
    for(auto h:hits) {
        uint32_t v=0;
        if(!ReadStableU32V18(targetB,h.c.offset,v)) continue;
        if(v==0 || v==0xFFFFFFFFu || v==h.a) continue;
        h.b=v;
        survived.push_back(h);
    }

    std::cout << "[ID TARGET B] ptr=" << HexU32(static_cast<uint32_t>(targetB))
              << " surviving=" << survived.size() << "\n";
    for(const auto& h:survived)
        std::cout << "  +" << HexU32(static_cast<uint32_t>(h.c.offset))
                  << " A=" << h.a << " B=" << h.b
                  << " getterSlot=" << h.c.slot
                  << " setter_pair=" << (h.c.setterPair?"YES":"NO")
                  << "\n";

    if(survived.size()==1) {
        const auto& h=survived.front();
        r.creatureLayout.id={"u32",h.c.offset,"runtime_creature_id_semantic_two_targets"};
        std::cout << "[CREATURE ID] CONFIRMADO +" << HexU32(static_cast<uint32_t>(h.c.offset))
                  << " u32; getter/setter comum a LocalPlayer/Player/Monster/Npc e valores diferentes em dois monstros.\n";
        SaveCheckpoint(r,"creature_id_semantic_two_targets");
    } else if(survived.empty()) {
        std::cout << "[CREATURE ID] nenhum candidato sobreviveu aos dois targets; mantendo pending.\n";
    } else {
        std::cout << "[CREATURE ID] mais de um campo sobreviveu; nenhum offset foi promovido.\n";
    }
}


struct V18SemanticFieldResult {
    std::string getToken;
    std::string setToken;
    std::string type;
    int offset = -1;
    uint32_t getterRva = 0;
    uint32_t setterRva = 0;
    bool getterUnique = false;
    bool setterAgree = false;
    std::string status = "pending";
};

bool DecodeDirectScalarGetterV18(uintptr_t fn,
                                 const std::string& expectedType,
                                 int& outOffset) {
    outOffset = -1;
    std::array<uint8_t, 80> b{};
    if (!SafeCopy(fn, b.data(), b.size())) return false;

    // Tiny OTC scalar getters are aligned back-to-back. V15 scanned 80 bytes
    // and therefore read neighboring getters too. The semantic getters we are
    // decoding here are direct accessors, so only inspect the first 16 bytes.
    // This is enough for mov/movzx/fld + displacement + ret, while never
    // crossing into the next 16-byte-aligned accessor.
    const size_t scanLen=16;

    std::set<int> hits;
    const auto add = [&](int d) {
        if (d >= 0 && d <= 0x1000) hits.insert(d);
    };

    for (size_t i=0; i<scanLen && i+8<b.size(); ++i) {
        if (expectedType=="u8") {
            // mov al,[ecx+disp8/disp32]
            if (b[i]==0x8A && b[i+1]==0x41) add(static_cast<uint8_t>(b[i+2]));
            if (b[i]==0x8A && b[i+1]==0x81) {
                int32_t d=0; std::memcpy(&d,b.data()+i+2,4); add(d);
            }
            // movzx eax,byte ptr [ecx+disp8/disp32]
            if (b[i]==0x0F && b[i+1]==0xB6 && b[i+2]==0x41) add(static_cast<uint8_t>(b[i+3]));
            if (b[i]==0x0F && b[i+1]==0xB6 && b[i+2]==0x81) {
                int32_t d=0; std::memcpy(&d,b.data()+i+3,4); add(d);
            }
        } else if (expectedType=="u16") {
            // mov ax,[ecx+disp8/disp32]
            if (b[i]==0x66 && b[i+1]==0x8B && b[i+2]==0x41) add(static_cast<uint8_t>(b[i+3]));
            if (b[i]==0x66 && b[i+1]==0x8B && b[i+2]==0x81) {
                int32_t d=0; std::memcpy(&d,b.data()+i+3,4); add(d);
            }
            // movzx eax,word ptr [ecx+disp8/disp32]
            if (b[i]==0x0F && b[i+1]==0xB7 && b[i+2]==0x41) add(static_cast<uint8_t>(b[i+3]));
            if (b[i]==0x0F && b[i+1]==0xB7 && b[i+2]==0x81) {
                int32_t d=0; std::memcpy(&d,b.data()+i+3,4); add(d);
            }
        } else if (expectedType=="u32") {
            if (b[i]==0x8B && b[i+1]==0x41) add(static_cast<uint8_t>(b[i+2]));
            if (b[i]==0x8B && b[i+1]==0x81) {
                int32_t d=0; std::memcpy(&d,b.data()+i+2,4); add(d);
            }
        } else if (expectedType=="double") {
            // fld qword ptr [ecx+disp8/disp32]
            if (b[i]==0xDD && b[i+1]==0x41) add(static_cast<uint8_t>(b[i+2]));
            if (b[i]==0xDD && b[i+1]==0x81) {
                int32_t d=0; std::memcpy(&d,b.data()+i+2,4); add(d);
            }
        } else if (expectedType=="float") {
            // fld dword ptr [ecx+disp8/disp32]
            if (b[i]==0xD9 && b[i+1]==0x41) add(static_cast<uint8_t>(b[i+2]));
            if (b[i]==0xD9 && b[i+1]==0x81) {
                int32_t d=0; std::memcpy(&d,b.data()+i+2,4); add(d);
            }
            // movss xmm?, [ecx+disp32]
            if (b[i]==0xF3 && b[i+1]==0x0F && b[i+2]==0x10) {
                const uint8_t mr=b[i+3];
                const int mod=(mr>>6)&3, rm=mr&7;
                if (rm==1 && mod==1) add(static_cast<int8_t>(b[i+4]));
                if (rm==1 && mod==2) { int32_t d=0; std::memcpy(&d,b.data()+i+4,4); add(d); }
            }
        }
    }

    if (hits.size()!=1) return false;
    outOffset=*hits.begin();
    return true;
}

bool MethodReferencesThisOffsetV18(uintptr_t fn, int wantedOffset) {
    if (!fn || wantedOffset < 0) return false;
    std::array<uint8_t, 160> b{};
    if (!SafeCopy(fn,b.data(),b.size())) return false;

    // Track simple register aliases derived from ECX (this). This handles
    // wrappers that begin with mov edi,ecx / mov esi,ecx / mov eax,ecx.
    std::set<int> thisRegs{1}; // ECX
    for(size_t i=0;i+1<b.size();++i) {
        if(b[i]!=0x8B) continue;
        const uint8_t mr=b[i+1];
        if(((mr>>6)&3)!=3) continue;
        const int dst=(mr>>3)&7, src=mr&7;
        if(thisRegs.count(src)) thisRegs.insert(dst);
    }

    const auto matchModrm=[&](size_t modrmPos)->bool {
        if (modrmPos>=b.size()) return false;
        const uint8_t mr=b[modrmPos];
        const int mod=(mr>>6)&3;
        const int rm=mr&7;
        if (!thisRegs.count(rm) || mod==3) return false;
        int d=0;
        if (mod==0) {
            if (rm==5) return false;
            d=0;
        } else if (mod==1) {
            if (modrmPos+1>=b.size()) return false;
            d=static_cast<int8_t>(b[modrmPos+1]);
        } else if (mod==2) {
            if (modrmPos+4>=b.size()) return false;
            int32_t dd=0; std::memcpy(&dd,b.data()+modrmPos+1,4); d=dd;
        }
        return d==wantedOffset;
    };

    for(size_t i=0;i+7<b.size();++i) {
        if (b[i]==0x88 || b[i]==0x89 || b[i]==0x8A || b[i]==0x8B ||
            b[i]==0x8D || b[i]==0xC6 || b[i]==0xC7 || b[i]==0xD9 || b[i]==0xDD) {
            if (matchModrm(i+1)) return true;
        }
        if (b[i]==0x66 && (b[i+1]==0x89 || b[i+1]==0x8B)) {
            if (matchModrm(i+2)) return true;
        }
        if (b[i]==0x0F && (b[i+1]==0xB6 || b[i+1]==0xB7)) {
            if (matchModrm(i+2)) return true;
        }
        if ((b[i]==0xF3 || b[i]==0xF2) && b[i+1]==0x0F &&
            (b[i+2]==0x10 || b[i+2]==0x11)) {
            // F3 = MOVSS (float), F2 = MOVSD (double). Complex OTC setters
            // frequently alias ECX into ESI/EDI and then write the scalar with
            // MOVSD, so both prefixes must participate in this+offset matching.
            if (matchModrm(i+3)) return true;
        }
    }
    return false;
}

std::vector<std::pair<int,uint32_t>> DecodeGetterCandidatesV18(const Report& r,
                                                                const std::string& token,
                                                                const std::string& type) {
    std::vector<std::pair<int,uint32_t>> out;
    const auto it=r.binds.find(token);
    if (it==r.binds.end()) return out;
    std::set<std::pair<int,uint32_t>> seen;
    for(const auto& c:it->second.candidates) {
        const uintptr_t fn=r.module.base+c.methodRva;
        int off=-1;
        if(!DecodeDirectScalarGetterV18(fn,type,off)) continue;
        if(seen.insert({off,c.methodRva}).second) out.push_back({off,c.methodRva});
    }
    return out;
}

V18SemanticFieldResult ResolveSemanticScalarV18(const Report& r,
                                                 const std::string& getToken,
                                                 const std::string& setToken,
                                                 const std::string& type,
                                                 int expectedOffset = -1) {
    V18SemanticFieldResult out;
    out.getToken=getToken; out.setToken=setToken; out.type=type;

    const auto git=r.binds.find(getToken);
    if(git==r.binds.end()) return out;
    const auto sit=r.binds.find(setToken);

    struct Hit {
        int offset=-1;
        uint32_t getterRva=0;
        uint32_t setterRva=0;
        int score=0;
        bool selectedGetter=false;
        bool setterAgree=false;
    };
    std::vector<Hit> hits;

    for(const auto& c:git->second.candidates) {
        int off=-1;
        if(!DecodeDirectScalarGetterV18(r.module.base+c.methodRva,type,off)) continue;

        Hit h;
        h.offset=off;
        h.getterRva=c.methodRva;
        h.score=c.score;
        h.selectedGetter=git->second.selectedOk && git->second.selected.methodRva==c.methodRva;
        if(h.selectedGetter) h.score += 140;
        if(expectedOffset>=0) h.score += (off==expectedOffset ? 500 : -500);

        if(sit!=r.binds.end()) {
            for(const auto& sc:sit->second.candidates) {
                if(MethodReferencesThisOffsetV18(r.module.base+sc.methodRva,off)) {
                    h.setterAgree=true;
                    h.setterRva=sc.methodRva;
                    h.score += 260;
                    if(sit->second.selectedOk && sit->second.selected.methodRva==sc.methodRva)
                        h.score += 80;
                    break;
                }
            }
        }
        hits.push_back(h);
    }
    if(hits.empty()) return out;

    std::map<int,Hit> bestByOffset;
    for(const auto& h:hits) {
        auto it=bestByOffset.find(h.offset);
        if(it==bestByOffset.end() || h.score>it->second.score)
            bestByOffset[h.offset]=h;
    }

    Hit chosen;
    bool have=false;

    // Known fields are used only as anchors to disambiguate noisy registration
    // candidates. Unknown fields below never inherit these offsets.
    if(expectedOffset>=0) {
        auto it=bestByOffset.find(expectedOffset);
        if(it!=bestByOffset.end()) {
            chosen=it->second;
            have=true;
        }
    } else {
        // Unknown fields require getter/setter agreement on the same this+offset.
        std::vector<Hit> paired;
        for(const auto& kv:bestByOffset)
            if(kv.second.setterAgree) paired.push_back(kv.second);

        if(paired.size()==1) {
            chosen=paired.front();
            have=true;
        } else if(paired.size()>1) {
            std::sort(paired.begin(),paired.end(),[](const Hit& a,const Hit& b){return a.score>b.score;});
            if(paired.front().selectedGetter && paired.front().score >= paired[1].score + 120) {
                chosen=paired.front();
                have=true;
            }
        }
    }

    if(!have) return out;

    out.offset=chosen.offset;
    out.getterRva=chosen.getterRva;
    out.getterUnique=true;
    out.setterAgree=chosen.setterAgree;
    out.setterRva=chosen.setterRva;
    if(expectedOffset>=0) {
        out.status=chosen.setterAgree
            ? "semantic_current_build_anchor_getter_setter"
            : "semantic_current_build_anchor_getter";
    } else {
        out.status=chosen.setterAgree
            ? "semantic_current_build_getter_setter_disambiguated"
            : "semantic_current_build_getter_disambiguated";
    }
    return out;
}


V18SemanticFieldResult ResolveSemanticAutoScalarV18(const Report& r,
                                                     const std::string& getToken,
                                                     const std::string& setToken) {
    const std::array<const char*,5> types = {"u8","u16","u32","float","double"};
    std::vector<V18SemanticFieldResult> hits;
    for(const char* t:types) {
        auto h=ResolveSemanticScalarV18(r,getToken,setToken,t);
        if(h.offset>=0 && h.getterUnique && h.setterAgree) hits.push_back(h);
    }
    if(hits.empty()) return {};

    // Collapse duplicate observations of the exact same offset/type and require
    // one semantic interpretation. We intentionally do not prefer an offline
    // type or offset.
    std::map<std::pair<int,std::string>,V18SemanticFieldResult> uniq;
    for(const auto& h:hits) uniq[{h.offset,h.type}]=h;
    if(uniq.size()!=1) {
        V18SemanticFieldResult pending;
        pending.getToken=getToken;
        pending.setToken=setToken;
        pending.status="pending_semantic_type_ambiguous";
        return pending;
    }
    auto out=uniq.begin()->second;
    out.status="semantic_current_build_getter_setter_autotype";
    return out;
}

void PrintSemanticScalarDiagnosticsV18(const Report& r,
                                      const std::string& getToken,
                                      const std::string& setToken) {
    std::cout << "  [DIAG " << getToken << "]";
    const auto git=r.binds.find(getToken);
    const auto sit=r.binds.find(setToken);
    if(git==r.binds.end()) {
        std::cout << " getter token ausente\n";
        return;
    }
    bool any=false;
    const std::array<const char*,5> types={"u8","u16","u32","float","double"};
    std::set<std::string> printed;
    for(const auto& c:git->second.candidates) {
        for(const char* t:types) {
            int off=-1;
            if(!DecodeDirectScalarGetterV18(r.module.base+c.methodRva,t,off)) continue;
            bool pair=false; uint32_t setter=0;
            if(sit!=r.binds.end()) {
                for(const auto& sc:sit->second.candidates) {
                    if(MethodReferencesThisOffsetV18(r.module.base+sc.methodRva,off)) {
                        pair=true; setter=sc.methodRva; break;
                    }
                }
            }
            std::ostringstream key;
            key << t << ':' << off << ':' << c.methodRva << ':' << setter;
            if(!printed.insert(key.str()).second) continue;
            if(!any) std::cout << "\n";
            any=true;
            std::cout << "    " << t << " +" << HexU32(static_cast<uint32_t>(off))
                      << " getter=" << HexU32(c.methodRva)
                      << " setter=" << (pair?HexU32(setter):std::string("<no-match>"))
                      << "\n";
        }
    }
    if(!any) std::cout << " nenhum getter direto decodificado\n";
}

std::vector<std::pair<int,uint32_t>> DecodeOutfitCandidatesV18(const Report& r,
                                                                const std::string& token) {
    std::vector<std::pair<int,uint32_t>> out;
    const auto it=r.binds.find(token);
    if(it==r.binds.end()) return out;
    std::set<std::pair<int,uint32_t>> seen;

    for(const auto& c:it->second.candidates) {
        const uintptr_t fn=r.module.base+c.methodRva;
        std::array<uint8_t, 112> b{};
        if(!SafeCopy(fn,b.data(),b.size())) continue;
        const int thisReg=DetectThisRegister(fn);
        std::set<int> offsets;
        for(size_t i=0;i+7<b.size();++i) {
            if(b[i]!=0x8D) continue;
            const uint8_t mr=b[i+1];
            const int mod=(mr>>6)&3, rm=mr&7;
            if(rm!=thisReg || mod==3) continue;
            int d=-1;
            if(mod==1) d=static_cast<int8_t>(b[i+2]);
            else if(mod==2) { int32_t dd=0; std::memcpy(&dd,b.data()+i+2,4); d=dd; }
            else if(mod==0) d=0;
            if(d>=0x20 && d<=0x400) offsets.insert(d);
        }
        if(offsets.size()==1) {
            const int off=*offsets.begin();
            if(seen.insert({off,c.methodRva}).second) out.push_back({off,c.methodRva});
        }
    }
    return out;
}

void RunCreatureSemanticFieldsV18(Report& r) {
    std::cout << "\n============================================================\n"
              << "       CREATURE SPEED / SKULL / SHIELD / OUTFIT V30\n"
              << "============================================================\n"
              << "[METHOD] exact current-build tokens -> all instance bind candidates -> getter/setter field agreement.\n"
              << "[RULE] noisy Lua registration candidates are disambiguated by setter agreement; offline offsets are NOT copied.\n";

    const auto hp=ResolveSemanticScalarV18(r,"getHealthPercent","setHealthPercent","u8",r.creatureLayout.hpPercent.offset);
    const auto dir=ResolveSemanticScalarV18(r,"getDirection","setDirection","u32",r.creatureLayout.direction);
    const bool hpAnchor = hp.offset>=0 && r.creatureLayout.hpPercent.offset>=0 && hp.offset==r.creatureLayout.hpPercent.offset;
    const bool dirAnchor = dir.offset>=0 && r.creatureLayout.direction>=0 && dir.offset==r.creatureLayout.direction;

    std::cout << "[ANCHOR getHealthPercent] "
              << (hp.offset>=0?("+"+HexU32(static_cast<uint32_t>(hp.offset))):std::string("<unresolved>"))
              << " expected=" << (r.creatureLayout.hpPercent.offset>=0?("+"+HexU32(static_cast<uint32_t>(r.creatureLayout.hpPercent.offset))):std::string("<pending>"))
              << " -> " << (hpAnchor?"MATCH":"NO MATCH") << "\n";
    std::cout << "[ANCHOR getDirection]     "
              << (dir.offset>=0?("+"+HexU32(static_cast<uint32_t>(dir.offset))):std::string("<unresolved>"))
              << " expected=" << (r.creatureLayout.direction>=0?("+"+HexU32(static_cast<uint32_t>(r.creatureLayout.direction))):std::string("<pending>"))
              << " -> " << (dirAnchor?"MATCH":"NO MATCH") << "\n";

    const bool anchorsOk = hpAnchor && dirAnchor;
    if(!anchorsOk) {
        std::cout << "[STOP PROMOTION] semantic resolver nao reproduziu os dois campos Creature ja confirmados; novos campos permanecem pending.\n";
        return;
    }

    const auto speed=ResolveSemanticScalarV18(r,"getSpeed","setSpeed","u16");
    const auto baseSpeed=ResolveSemanticAutoScalarV18(r,"getBaseSpeed","setBaseSpeed");
    const auto skull=ResolveSemanticScalarV18(r,"getSkull","setSkull","u8");
    const auto shield=ResolveSemanticScalarV18(r,"getShield","setShield","u8");

    const auto printField=[&](const char* label,const V18SemanticFieldResult& f) {
        std::cout << std::left << std::setw(14) << label << std::right;
        if(f.offset<0) {
            std::cout << " <pending>\n";
            return;
        }
        std::cout << " " << f.type << " +" << HexU32(static_cast<uint32_t>(f.offset))
                  << " getter=" << HexU32(f.getterRva)
                  << " setter=" << (f.setterAgree?HexU32(f.setterRva):std::string("<not-confirmed>"))
                  << " [" << f.status << "]\n";
    };
    printField("Speed",speed);
    printField("BaseSpeed",baseSpeed);
    if(baseSpeed.offset<0) PrintSemanticScalarDiagnosticsV18(r,"getBaseSpeed","setBaseSpeed");
    printField("Skull",skull);
    printField("Shield/Party",shield);

    const auto promote=[&](const V18SemanticFieldResult& f, SelectedField& dst) {
        if(f.offset<0 || !f.getterUnique || !f.setterAgree) return;
        if(dst.offset<0 || EvidenceScore(f.status)>=EvidenceScore(dst.status))
            dst={f.type,f.offset,f.status};
    };
    promote(speed,r.creatureLayout.speed);
    promote(baseSpeed,r.creatureLayout.baseSpeed);
    promote(skull,r.creatureLayout.skull);
    promote(shield,r.creatureLayout.shield);

    // Outfit is a non-scalar structure. getOutfit/setOutfit normally expose it
    // by taking the address of this+offset. Require a unique getter offset and,
    // when possible, the setter to reference exactly the same field.
    const auto outfitGets=DecodeOutfitCandidatesV18(r,"getOutfit");
    struct OutfitHit { int off=-1; uint32_t getter=0; uint32_t setter=0; int score=0; bool setterAgree=false; bool selectedGetter=false; };
    std::map<int,OutfitHit> outfitBest;
    const auto ogit=r.binds.find("getOutfit");
    const auto osit=r.binds.find("setOutfit");
    for(const auto& h:outfitGets) {
        OutfitHit oh; oh.off=h.first; oh.getter=h.second;
        if(ogit!=r.binds.end()) {
            for(const auto& c:ogit->second.candidates) if(c.methodRva==h.second) {
                oh.score=c.score;
                oh.selectedGetter=ogit->second.selectedOk && ogit->second.selected.methodRva==c.methodRva;
                if(oh.selectedGetter) oh.score+=140;
                break;
            }
        }
        if(osit!=r.binds.end()) {
            for(const auto& c:osit->second.candidates) {
                if(MethodReferencesThisOffsetV18(r.module.base+c.methodRva,oh.off)) {
                    oh.setterAgree=true; oh.setter=c.methodRva; oh.score+=260;
                    if(osit->second.selectedOk && osit->second.selected.methodRva==c.methodRva) oh.score+=80;
                    break;
                }
            }
        }
        auto it=outfitBest.find(oh.off);
        if(it==outfitBest.end() || oh.score>it->second.score) outfitBest[oh.off]=oh;
    }

    std::vector<OutfitHit> outfitPaired;
    for(const auto& kv:outfitBest) if(kv.second.setterAgree) outfitPaired.push_back(kv.second);
    std::sort(outfitPaired.begin(),outfitPaired.end(),[](const OutfitHit&a,const OutfitHit&b){return a.score>b.score;});

    int outfitOff=-1; uint32_t outfitGetter=0, outfitSetter=0; bool outfitSetterAgree=false;
    if(outfitPaired.size()==1 ||
       (outfitPaired.size()>1 && outfitPaired.front().selectedGetter &&
        outfitPaired.front().score >= outfitPaired[1].score + 120)) {
        const auto& w=outfitPaired.front();
        outfitOff=w.off; outfitGetter=w.getter; outfitSetter=w.setter; outfitSetterAgree=w.setterAgree;
        r.creatureLayout.outfitOffset=outfitOff;
        r.creatureLayout.outfitStatus="semantic_current_build_outfit_getter_setter_disambiguated";
    }
    std::cout << std::left << std::setw(14) << "Outfit" << std::right;
    if(outfitOff>=0) {
        std::cout << " struct +" << HexU32(static_cast<uint32_t>(outfitOff))
                  << " getter=" << HexU32(outfitGetter)
                  << " setter=" << (outfitSetterAgree?HexU32(outfitSetter):std::string("<not-confirmed>"))
                  << " [" << r.creatureLayout.outfitStatus << "]\n";
    } else {
        std::cout << " <pending>";
        if(!outfitBest.empty()) {
            std::cout << " candidates=";
            bool first=true;
            for(const auto& kv:outfitBest) {
                if(!first) std::cout << ",";
                first=false;
                std::cout << "+" << HexU32(static_cast<uint32_t>(kv.first))
                          << (kv.second.setterAgree?"[pair]":"[getter]");
            }
        }
        std::cout << "\n";
    }

    // Read-only sanity values from the LocalPlayer and, when available, the current target.
    const auto printRuntime=[&](uintptr_t obj,const char* who) {
        if(!obj) return;
        std::cout << "[" << who << " QUICK]";
        if(r.creatureLayout.speed.offset>=0) { uint16_t v=0; if(SafeRead(obj+r.creatureLayout.speed.offset,v)) std::cout << " speed=" << v; }
        if(r.creatureLayout.baseSpeed.offset>=0) { double v=0; if(ReadSelectedValue(obj,r.creatureLayout.baseSpeed,v)) std::cout << " baseSpeed=" << v; }
        if(r.creatureLayout.skull.offset>=0) { uint8_t v=0; if(SafeRead(obj+r.creatureLayout.skull.offset,v)) std::cout << " skull=" << static_cast<unsigned>(v); }
        if(r.creatureLayout.shield.offset>=0) { uint8_t v=0; if(SafeRead(obj+r.creatureLayout.shield.offset,v)) std::cout << " shield=" << static_cast<unsigned>(v); }
        std::cout << "\n";
    };
    printRuntime(r.localPlayerPtr,"LOCALPLAYER");
    uintptr_t target=AcquireCreatureFromAttackTarget(r,false);
    if(target) printRuntime(target,"TARGET");

    SaveCheckpoint(r,"creature_speed_skull_shield_outfit_semantic");
}



void RunLocalPlayerSemanticFieldsV18(Report& r) {
    std::cout << "\n============================================================\n"
              << "       LOCALPLAYER MAGIC / STATES / SOUL / STAMINA V30\n"
              << "============================================================\n"
              << "[METHOD] exact current-build get/set tokens -> direct this+field agreement with automatic scalar type.\n"
              << "[RULE] promotion requires the already-confirmed Creature HP% and Direction anchors to decode correctly first.\n";

    const auto hp=ResolveSemanticScalarV18(r,"getHealthPercent","setHealthPercent","u8",r.creatureLayout.hpPercent.offset);
    const auto dir=ResolveSemanticScalarV18(r,"getDirection","setDirection","u32",r.creatureLayout.direction);
    const bool anchorsOk = hp.offset==r.creatureLayout.hpPercent.offset &&
                           dir.offset==r.creatureLayout.direction &&
                           hp.offset>=0 && dir.offset>=0;
    std::cout << "[ANCHORS] HP%=" << (hp.offset>=0?("+"+HexU32(static_cast<uint32_t>(hp.offset))):std::string("<unresolved>"))
              << " Direction=" << (dir.offset>=0?("+"+HexU32(static_cast<uint32_t>(dir.offset))):std::string("<unresolved>"))
              << " -> " << (anchorsOk?"MATCH":"NO MATCH") << "\n";
    if(!anchorsOk) {
        std::cout << "[STOP PROMOTION] semantic accessor decoder perdeu os anchors; LocalPlayer fields permanecem pending.\n";
        return;
    }

    const auto magic=ResolveSemanticAutoScalarV18(r,"getMagicLevel","setMagicLevel");
    const auto states=ResolveSemanticAutoScalarV18(r,"getStates","setStates");
    const auto soul=ResolveSemanticAutoScalarV18(r,"getSoul","setSoul");
    const auto stamina=ResolveSemanticAutoScalarV18(r,"getStamina","setStamina");

    const auto printField=[&](const char* label,const V18SemanticFieldResult& f) {
        std::cout << std::left << std::setw(14) << label << std::right;
        if(f.offset<0) {
            std::cout << " <pending> [" << f.status << "]\n";
            return;
        }
        std::cout << " " << f.type << " +" << HexU32(static_cast<uint32_t>(f.offset))
                  << " getter=" << HexU32(f.getterRva)
                  << " setter=" << (f.setterAgree?HexU32(f.setterRva):std::string("<not-confirmed>"))
                  << " [" << f.status << "]\n";
    };
    printField("MagicLevel",magic);
    if(magic.offset<0) PrintSemanticScalarDiagnosticsV18(r,"getMagicLevel","setMagicLevel");
    printField("States",states);
    printField("Soul",soul);
    if(soul.offset<0) PrintSemanticScalarDiagnosticsV18(r,"getSoul","setSoul");
    printField("Stamina",stamina);
    if(stamina.offset<0) PrintSemanticScalarDiagnosticsV18(r,"getStamina","setStamina");

    const auto promote=[&](const char* name,const V18SemanticFieldResult& f) {
        if(f.offset<0 || !f.getterUnique || !f.setterAgree) return;
        SelectedField sf{f.type,f.offset,f.status};
        auto it=r.selectedFields.find(name);
        if(it==r.selectedFields.end() || it->second.offset<0 ||
           EvidenceScore(sf.status)>=EvidenceScore(it->second.status))
            r.selectedFields[name]=sf;
    };
    promote("magicLevel",magic);
    promote("states",states);
    promote("soul",soul);
    promote("stamina",stamina);

    std::cout << "[LOCALPLAYER QUICK]";
    for(const char* name:{"magicLevel","states","soul","stamina"}) {
        auto it=r.selectedFields.find(name);
        if(it==r.selectedFields.end() || it->second.offset<0) continue;
        double v=0;
        if(ReadSelectedValue(r.localPlayerPtr,it->second,v))
            std::cout << " " << name << "=" << v;
    }
    std::cout << "\n";
    SaveCheckpoint(r,"localplayer_magic_states_soul_stamina_semantic");
}


void RunCreatureLayoutDiscovery(Report& r) {
    std::cout << "\n============================================================\n"
              << "              PERSISTENT CREATURE LAYOUT V30\n"
              << "============================================================\n";

    const bool positionKnown = r.creatureLayout.x >= 0 && r.creatureLayout.y >= 0 && r.creatureLayout.z >= 0 &&
                               EvidenceScore(r.creatureLayout.positionStatus) >= 80;
    const bool nameKnown = r.creatureLayout.nameOffset >= 0 && EvidenceScore(r.creatureLayout.nameStatus) >= 80;
    const bool hpKnown = r.creatureLayout.hpPercent.offset >= 0 && EvidenceScore(r.creatureLayout.hpPercent.status) >= 80;

    uintptr_t target = AcquireCreatureFromAttackTarget(r, !(positionKnown && nameKnown && hpKnown));
    if (!target) {
        std::cout << "[CREATURE] sem AttackTarget valido nesta execucao; mantendo profile e seguindo.\n";
        return;
    }
    r.creatureTargetPtr = target;

    uint32_t cx=0,cy=0; uint8_t cz=0;
    if(positionKnown && QuickCheckCreaturePosition(target,r.localPlayerPtr,r.positionRuntime,r.creatureLayout,&cx,&cy,&cz)) {
        std::cout << "[CREATURE POSITION QUICK-CHECK] OK -> " << cx << "," << cy << "," << static_cast<unsigned>(cz) << "\n";
    } else if(PromoteCreaturePositionFromPlayerLayout(r,target)) {
        SaveCheckpoint(r,"creature_position");
    }

    std::string currentName;
    if(nameKnown && ReadCreatureName(target,r.creatureLayout,currentName)) {
        std::cout << "[CREATURE NAME QUICK-CHECK] +" << HexU32(r.creatureLayout.nameOffset)
                  << " " << r.creatureLayout.nameKind << " -> \"" << currentName << "\" [OK]\n";
        if(r.creatureLayout.nameMirrorOffset >= 0) {
            std::string mirror;
            const bool ok = ReadCreatureNameAt(target,r.creatureLayout.nameMirrorOffset,r.creatureLayout.nameMirrorKind,mirror);
            std::cout << "[CREATURE NAME MIRROR QUICK-CHECK] +" << HexU32(r.creatureLayout.nameMirrorOffset)
                      << " " << r.creatureLayout.nameMirrorKind << " -> " << (ok?("\""+mirror+"\""):std::string("<read failed>")) << "\n";
        }
    } else {
        const std::string expectedA=AskText("Nome EXATO visivel da criatura atacada (ENTER para pular)");
        if(!expectedA.empty()) {
            auto a=FindCreatureNameCandidates(target,expectedA,0x300);
            std::cout << "[CREATURE NAME A] candidates=" << a.size() << "\n";
            for(const auto& c:a) std::cout << "  +" << HexU32(c.offset) << " " << c.kind << " -> \"" << c.value << "\"\n";
            if(a.size()==1) {
                r.creatureLayout.nameOffset=a[0].offset; r.creatureLayout.nameKind=a[0].kind;
                r.creatureLayout.nameStatus="runtime_creature_name_exact";
                SaveCheckpoint(r,"creature_name");
            } else if(a.size()>1 && AskYesNo("Validar os offsets de Name com uma SEGUNDA criatura de nome diferente?")=='s') {
                WaitEnter("Ataque uma criatura de NOME DIFERENTE e mantenha o target. Depois ENTER... ");
                uintptr_t targetB=AcquireCreatureFromAttackTarget(r,false);
                if(targetB && targetB!=target) {
                    const std::string expectedB=AskText("Nome EXATO da segunda criatura");
                    auto b=FindCreatureNameCandidates(targetB,expectedB,0x300);
                    std::vector<CreatureNameCandidate> common;
                    for(const auto& x:a) for(const auto& y:b)
                        if(x.offset==y.offset && x.kind==y.kind) { common.push_back(x); break; }
                    std::cout << "[CREATURE NAME TWO-TARGET] common offsets=" << common.size() << "\n";
                    for(const auto& c:common) std::cout << "  +" << HexU32(c.offset) << " " << c.kind << "\n";
                    if(common.size()==1) {
                        r.creatureLayout.nameOffset=common[0].offset; r.creatureLayout.nameKind=common[0].kind;
                        r.creatureLayout.nameStatus="runtime_creature_name_two_targets";
                        SaveCheckpoint(r,"creature_name");
                    } else if(common.size()==2) {
                        std::sort(common.begin(),common.end(),[](const CreatureNameCandidate& x,const CreatureNameCandidate& y){return x.offset<y.offset;});
                        r.creatureLayout.nameOffset=common[0].offset; r.creatureLayout.nameKind=common[0].kind;
                        r.creatureLayout.nameStatus="runtime_creature_name_mirrored_two_targets";
                        r.creatureLayout.nameMirrorOffset=common[1].offset; r.creatureLayout.nameMirrorKind=common[1].kind;
                        r.creatureLayout.nameMirrorStatus="runtime_creature_name_mirrored_two_targets";
                        std::cout << "[CREATURE NAME MIRROR] confirmed +" << HexU32(common[0].offset)
                                  << " / +" << HexU32(common[1].offset) << " across two names.\n";
                        SaveCheckpoint(r,"creature_name_mirror");
                    } else {
                        std::cout << "[CREATURE NAME] continua ambiguo; nenhum offset foi inventado.\n";
                    }
                    target=targetB; r.creatureTargetPtr=targetB;
                } else {
                    std::cout << "[CREATURE NAME] segunda criatura nao ficou ativa ou reutilizou o mesmo objeto; mantendo pending.\n";
                }
            }
        }
    }

    double hpQuick=0;
    if(hpKnown && ReadCreatureHpDisplay(target,r.creatureLayout.hpPercent,hpQuick)) {
        std::cout << "[CREATURE HP% QUICK-CHECK] +" << HexU32(r.creatureLayout.hpPercent.offset)
                  << " " << r.creatureLayout.hpPercent.type << " -> " << hpQuick << "% [OK]\n";
    } else if(AskYesNo("Mapear Creature.HP% automaticamente com DOIS monstros?")=='s') {
        std::cout << "\n[HP% AUTO V24]\n"
                  << "Nao sera pedido o HP depois do golpe.\n"
                  << "Fluxo: Monster A cheio -> dano -> matar -> Monster B cheio -> dano.\n";

        target=AcquireCreatureFromAttackTarget(r,true);
        if(target) {
            const uintptr_t targetA=target;
            WaitEnter("Garanta que o MONSTRO A esta com HP CHEIO (100%) e mantenha o target. ENTER... ");
            uintptr_t checkA=AcquireCreatureFromAttackTarget(r,false);
            if(checkA!=targetA) {
                std::cout << "[HP% AUTO] target A mudou antes do snapshot; teste adiado.\n";
            } else {
                auto candidates=FindCreatureHpCandidates(targetA,100.0,0x400);
                PrintHpCandidatesWithCurrent(targetA,candidates,"HP A FULL");

                WaitEnter("De UM ou mais golpes SEM MATAR o MONSTRO A. Quando ele estiver ferido, ENTER... ");
                uintptr_t hurtA=AcquireCreatureFromAttackTarget(r,false);
                if(hurtA!=targetA) {
                    std::cout << "[HP% AUTO] Monster A mudou/morreu antes do snapshot de dano. Nenhum dado promovido.\n";
                } else {
                    candidates=FilterHpDamaged(targetA,candidates);
                    PrintHpCandidatesWithCurrent(targetA,candidates,"HP A DAMAGED");
                    if(candidates.empty()) {
                        std::cout << "[HP% AUTO] nenhum candidato 100->menor no Monster A.\n";
                    } else {
                        WaitEnter("Agora MATE o MONSTRO A. Espere o target desaparecer/zerar e ENTER... ");
                        const uintptr_t game = r.module.base + r.gameOwnerRva;
                        uint32_t afterKill=0;
                        SafeRead(game + r.gameLayout.attackTarget, afterKill);
                        bool sawClear = afterKill==0;
                        if(!sawClear) {
                            probe_console::ui() << "AttackTarget ainda nao zerou. Espere/cancele o target e ENTER... " << std::flush;
                            std::string dummy; std::getline(std::cin,dummy);
                            afterKill=0; SafeRead(game + r.gameLayout.attackTarget,afterKill);
                            sawClear = afterKill==0;
                        }
                        std::cout << "[HP% AUTO] ciclo A -> null: " << (sawClear?"CONFIRMADO":"NAO CONFIRMADO") << "\n";

                        WaitEnter("Ataque OUTRO monstro (MONSTRO B) com HP CHEIO e mantenha o target. ENTER... ");
                        uintptr_t targetB=AcquireCreatureFromAttackTarget(r,false);
                        if(!targetB || (!sawClear && targetB==targetA)) {
                            std::cout << "[HP% AUTO] segundo monstro nao validado. Profile preservado.\n";
                        } else {
                            candidates=FilterHpFull(targetB,candidates);
                            PrintHpCandidatesWithCurrent(targetB,candidates,"HP B RESET FULL");
                            if(candidates.empty()) {
                                std::cout << "[HP% AUTO] nenhum offset do Monster A voltou ao estado cheio no Monster B.\n";
                            } else {
                                WaitEnter("De UM ou mais golpes SEM MATAR o MONSTRO B. Quando ele estiver ferido, ENTER... ");
                                uintptr_t hurtB=AcquireCreatureFromAttackTarget(r,false);
                                if(hurtB!=targetB) {
                                    std::cout << "[HP% AUTO] Monster B mudou/morreu antes do snapshot de dano. Profile preservado.\n";
                                } else {
                                    candidates=FilterHpDamaged(targetB,candidates);
                                    PrintHpCandidatesWithCurrent(targetB,candidates,"HP B DAMAGED");
                                    CreatureHpCandidate chosen;
                                    if(SelectHpCandidateUniqueOffset(candidates,chosen)) {
                                        r.creatureLayout.hpPercent.type=chosen.type;
                                        r.creatureLayout.hpPercent.offset=chosen.offset;
                                        r.creatureLayout.hpPercent.status="runtime_hppercent_two_creatures_damage_reset_"+chosen.encoding;
                                        std::cout << "[CREATURE HP%] CONFIRMADO +" << HexU32(static_cast<uint32_t>(chosen.offset))
                                                  << " type=" << chosen.type << " encoding=" << chosen.encoding
                                                  << " por dois monstros (full->damage / reset->damage).\n";
                                        SaveCheckpoint(r,"creature_hp_percent_two_creatures");
                                        target=targetB; r.creatureTargetPtr=targetB;
                                    } else if(candidates.empty()) {
                                        std::cout << "[HP% AUTO] nenhum candidato sobreviveu aos dois monstros.\n";
                                    } else {
                                        std::cout << "[HP% AUTO] ainda existem " << candidates.size()
                                                  << " representacoes em mais de um offset; mantendo pending sem chute.\n";
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    std::cout << "\n[CREATURE LAYOUT RESULT]\n"
              << "RTTI target : " << (r.creatureTargetClass.empty()?"<none>":r.creatureTargetClass) << "\n"
              << "Position    : " << (r.creatureLayout.x>=0?("+"+HexU32(r.creatureLayout.x)+"/+"+HexU32(r.creatureLayout.y)+"/+"+HexU32(r.creatureLayout.z)):std::string("PENDING")) << " [" << r.creatureLayout.positionStatus << "]\n"
              << "Direction   : " << (r.creatureLayout.direction>=0?("+"+HexU32(r.creatureLayout.direction)):std::string("PENDING")) << "\n"
              << "Name        : " << (r.creatureLayout.nameOffset>=0?("+"+HexU32(r.creatureLayout.nameOffset)+" "+r.creatureLayout.nameKind):std::string("PENDING")) << " [" << r.creatureLayout.nameStatus << "]\n";
    if(r.creatureLayout.nameMirrorOffset>=0)
        std::cout << "Name mirror : +" << HexU32(r.creatureLayout.nameMirrorOffset) << " " << r.creatureLayout.nameMirrorKind << " [" << r.creatureLayout.nameMirrorStatus << "]\n";
    std::cout << "HP%         : " << (r.creatureLayout.hpPercent.offset>=0?(r.creatureLayout.hpPercent.type+" +"+HexU32(r.creatureLayout.hpPercent.offset)):std::string("PENDING")) << " [" << r.creatureLayout.hpPercent.status << "]\n"
              << "ID          : " << (r.creatureLayout.id.offset>=0?(r.creatureLayout.id.type+" +"+HexU32(r.creatureLayout.id.offset)):std::string("PENDING")) << " [" << r.creatureLayout.id.status << "]\n"
              << "Light       : " << (r.creatureLayout.lightLevel.offset>=0?(r.creatureLayout.lightLevel.type+" +"+HexU32(r.creatureLayout.lightLevel.offset)):std::string("PENDING")) << " [" << r.creatureLayout.lightStatus << "]\n";
    SaveCheckpoint(r,"creature_layout");
}


bool TopMoveContractMatches(const std::string& raw,uint32_t methodRva) {
    const auto self=FindJsonObject(raw,"this");
    const auto ret=FindJsonObject(raw,"return");
    const auto hidden=FindJsonObject(ret,"hiddenStorage");
    const auto refs=FindJsonObject(ret,"refCount");
    return methodRva && HexU32Value(JsonStringValue(raw,"methodRva"))==methodRva &&
        JsonStringValue(raw,"status")=="runtime_confirmed" &&
        JsonStringValue(raw,"callingConvention")=="x86_thiscall_sret" &&
        JsonStringValue(self,"semantic")=="Tile" &&
        JsonStringValue(self,"location")=="ecx" &&
        HexU32Value(JsonStringValue(raw,"stackCleanupBytes"))==4 &&
        JsonStringValue(ret,"abiType")=="shared_object_ptr<Thing>" &&
        JsonStringValue(ret,"status")=="runtime_confirmed" &&
        JsonStringValue(hidden,"entryLocation")=="[esp+4]" &&
        JsonStringValue(hidden,"frameLocation")=="[ebp+8]" &&
        HexU32Value(JsonStringValue(refs,"offset"))==4 &&
        JsonStringValue(refs,"status")=="increment_and_release_runtime_confirmed" &&
        JsonStringValue(ret,"tileMembership")=="runtime_confirmed";
}

std::string CurrentPipeBaselineJson(const Report& r) {
    std::ostringstream o;
    o << "{\"id\":\"" << ramonia_baseline::id
      << "\",\"policy\":\"current_build_discovery_then_runtime_abi\","
      << "\"binds\":{";
    bool first=true;
    for(const char* token:ramonia_baseline::requiredBinds) {
        if(!first) o << ",";
        first=false;
        const auto it=r.binds.find(token);
        o << "\"" << token << "\":\""
          << (it!=r.binds.end() && it->second.selectedOk ? "discovered" : "pending") << "\"";
    }
    o << "},\"requiredAbi\":[";
    first=true;
    for(const char* token:ramonia_baseline::abiMethods) {
        if(std::string(token)=="talk") continue; // talkChannel is the live UI route.
        if(!first) o << ",";
        first=false;
        o << "\"" << token << "\"";
    }
    o << "],\"requiredFields\":[\"tileThings\",\"tileThingsElem\","
      << "\"tileVtableRva\",\"itemId\",\"itemCount\",\"inventoryArray\",\"inventoryStride\","
      << "\"containerItems\",\"openContainersMap\"],"
      << "\"tileAccessPolicy\":\"runtime_getTopMoveThing_same_tile_membership\","
      << "\"uncoverPolicy\":\"one_blocker_then_reread\","
      << "\"toolUsePolicy\":\"require_exposed_target_then_useWith\","
      << "\"abiPromotion\":\"static_or_entry_only_evidence_is_not_confirmation\"}";
    return o.str();
}

std::string SaveJson(const Report& r) {
    const std::string path = PersistentProfilePath(r);
    std::ofstream o(path, std::ios::binary);
    if (!o) return {};

    o << "{\n";
    o << "  \"schema\": \"otclab-ramonia-x86-research-v40\",\n";
    o << "  \"mode\": \"discovery_and_natural_call_capture_no_internal_calls\",\n";
    o << "  \"policy\": \"discovery_rules_only_no_cross_build_rva_oracle\",\n";
    o << "  \"persistence\": {\"profileVersion\": 40, \"checkpoint\": \"" << JsonEscape(r.checkpoint)
      << "\", \"resumeEnabled\": true},\n";
    o << "  \"build\": {\n";
    o << "    \"exe\": \"" << JsonEscape(r.module.path) << "\",\n";
    o << "    \"architecture\": \"x86\",\n";
    o << "    \"timestamp\": \"" << HexU32(r.module.timestamp) << "\",\n";
    o << "    \"sizeOfImage\": \"" << HexU32(r.module.sizeOfImage) << "\",\n";
    o << "    \"entryPointRva\": \"" << HexU32(r.module.entryRva) << "\",\n";
    o << "    \"textHashFNV1A64\": \"" << (r.fp.ok ? HexU64(r.fp.textHash) : "0x0") << "\",\n";
    o << "    \"textHashMethod\": \"disk_raw_text\",\n";
    o << "    \"mappedTextHashDiagnostic\": \"" << HexU64(r.mappedTextHash) << "\",\n";
    o << "    \"rendererHint\": \"" << JsonEscape(r.renderer) << "\"\n";
    o << "  },\n";

    o << "  \"rtti\": {\n";
    size_t ri=0;
    for (const auto& kv : r.rtti) {
        o << "    \"" << JsonEscape(kv.first) << "\": {\"typeDescriptorRva\": \""
          << HexU32(kv.second.typeDescriptorRva) << "\", \"vtables\": [";
        for (size_t i=0;i<kv.second.vtableRvas.size();++i) {
            if (i) o << ", ";
            o << "\"" << HexU32(kv.second.vtableRvas[i]) << "\"";
        }
        o << "]}" << (++ri<r.rtti.size()?",":"") << "\n";
    }
    o << "  },\n";

    o << "  \"semanticBinds\": {\n";
    size_t bi=0;
    for (const auto& kv : r.binds) {
        o << "    \"" << JsonEscape(kv.first) << "\": {";
        if (kv.second.selectedOk) {
            const auto& c = kv.second.selected;
            o << "\"methodRva\": \"" << HexU32(c.methodRva) << "\", "
              << "\"ownerRva\": \"" << HexU32(c.ownerRva) << "\", "
              << "\"ownerSection\": \"" << JsonEscape(c.ownerSection) << "\", "
              << "\"status\": \"offline_semantic\"";
        } else {
            o << "\"methodRva\": \"0x0\", \"ownerRva\": \"0x0\", \"status\": \"not_found\"";
        }
        o << ",\"candidates\":[";
        for(size_t i=0;i<kv.second.candidates.size();++i) {
            const auto& c=kv.second.candidates[i];
            if(i) o << ",";
            o << "{\"methodRva\":\"" << HexU32(c.methodRva)
              << "\",\"ownerRva\":\"" << HexU32(c.ownerRva)
              << "\",\"ownerSection\":\"" << JsonEscape(c.ownerSection)
              << "\",\"tokenRva\":\"" << HexU32(c.tokenRva)
              << "\",\"xrefRva\":\"" << HexU32(c.xrefRva)
              << "\",\"score\":" << c.score << "}";
        }
        o << "]}" << (++bi<r.binds.size()?",":"") << "\n";
    }
    o << "  },\n";

    o << "  \"owners\": {\"gameRva\": \"" << HexU32(r.gameOwnerRva)
      << "\", \"mapRva\": \"" << HexU32(r.mapOwnerRva) << "\"},\n";

    o << "  \"pipeBaseline\": " << CurrentPipeBaselineJson(r) << ",\n";
    o << "  \"guidedValidation\": {";
    size_t gi=0;
    for(const auto& kv:r.guidedEvidence) {
        if(gi++) o << ",";
        o << "\"" << JsonEscape(kv.first) << "\":" << kv.second;
    }
    o << "},\n";

    o << "  \"actions\": {\n";
    const std::vector<std::string> actionOrder(
        ramonia_baseline::abiMethods.begin(),ramonia_baseline::abiMethods.end());
    std::vector<std::string> actionPresent;
    for (const auto& name : actionOrder) {
        if (r.actionAbi.count(name) || r.actionAbiRaw.count(name)) actionPresent.push_back(name);
    }
    for (size_t ai=0; ai<actionPresent.size(); ++ai) {
        const std::string& name = actionPresent[ai];
        auto it = r.actionAbi.find(name);
        if (it == r.actionAbi.end()) {
            o << "    \"" << JsonEscape(name) << "\": " << r.actionAbiRaw.at(name)
              << (ai+1<actionPresent.size()?",":"") << "\n";
            continue;
        }
        const ActionAbiV34& a = it->second;
        o << "    \"" << JsonEscape(name) << "\": {\n"
          << "      \"methodRva\": \"" << HexU32(a.methodRva) << "\",\n"
          << "      \"ownerRva\": \"" << HexU32(a.ownerRva) << "\",\n"
          << "      \"callingConvention\": \"" << JsonEscape(a.callingConvention) << "\",\n";
        if (name=="autoWalk")
            o << "      \"stackCleanupBytes\": 16,\n";
        o << "      \"this\": {\"semantic\": \"" << JsonEscape(a.thisSemantic)
          << "\", \"location\": \"" << JsonEscape(a.thisLocation) << "\"},\n"
          << "      \"status\": \"" << JsonEscape(a.status) << "\",\n"
          << "      \"args\": [";
        for (size_t i=0;i<a.args.size();++i) {
            const ActionAbiArgV34& arg=a.args[i];
            if (i) o << ", ";
            o << "{\"index\":" << arg.index
              << ",\"semantic\":\"" << JsonEscape(arg.semantic)
              << "\",\"abiType\":\"" << JsonEscape(arg.abiType)
              << "\",\"status\":\"" << JsonEscape(arg.status) << "\"";
            if (!arg.rtti.empty()) o << ",\"rtti\":\"" << JsonEscape(arg.rtti) << "\"";
            if (arg.hasObservedBool)
                o << ",\"observedRequiredValue\":" << (arg.observedBool?"true":"false");
            if (arg.hasRefCountOffset)
                o << ",\"refCountOffset\":\"" << HexU32(arg.refCountOffset) << "\"";
            if (!arg.ownership.empty())
                o << ",\"ownership\":\"" << JsonEscape(arg.ownership) << "\"";
            o << "}";
        }
        o << "]\n"
          << "    }" << (ai+1<actionPresent.size()?",":"") << "\n";
    }
    o << "  },\n";
    o << "  \"gameLayout\": {\n";
    auto joff=[&](int v){ return v>=0 ? HexU32(static_cast<uint32_t>(v)) : std::string("0x0"); };
    o << "    \"localPlayerOffset\": \"" << joff(r.gameLayout.localPlayer) << "\",\n";
    o << "    \"attackTargetOffset\": \"" << joff(r.gameLayout.attackTarget) << "\",\n";
    o << "    \"followTargetOffset\": \"" << joff(r.gameLayout.followTarget) << "\",\n";
    o << "    \"protocolCandidateOffset\": \"" << joff(r.gameLayout.protocolCandidate) << "\",\n";
    o << "    \"attackTargetSource\": \"" << JsonEscape(r.attackTargetSource) << "\",\n";
    o << "    \"followTargetSource\": \"" << JsonEscape(r.followTargetSource) << "\",\n";
    o << "    \"status\": \"" << (r.gameLayout.inferred?"runtime_or_static_consistent":"partial") << "\"\n";
    o << "  },\n";

    o << "  \"runtime\": {\n";
    o << "    \"localPlayerPtr\": \"" << HexU64(r.localPlayerPtr) << "\",\n";
    o << "    \"localPlayerVtableRva\": \"" << HexU32(r.localPlayerVtableRva) << "\",\n";
    o << "    \"localPlayerVtableMatch\": " << (r.localPlayerVtableMatch?"true":"false") << ",\n";
    o << "    \"position\": {\"x\": \"" << joff(r.positionRuntime.x)
      << "\", \"y\": \"" << joff(r.positionRuntime.y)
      << "\", \"z\": \"" << joff(r.positionRuntime.z)
      << "\", \"direction\": \"" << joff(r.positionRuntime.direction) << "\"},\n";
    o << "    \"mapCenterCandidates\": [";
    for (size_t i=0;i<r.mapCenters.size();++i) {
        if (i) o << ", ";
        o << "\"" << HexU32(r.mapCenters[i].offset) << "\"";
    }
    o << "],\n";
    o << "    \"mapCenterOffset\": \"" << joff(r.mapCenterOffset) << "\",\n";
    o << "    \"mapCenterStatus\": \"" << JsonEscape(r.mapCenterStatus) << "\",\n";
    o << "    \"positionSource\": \"" << JsonEscape(r.positionSource) << "\",\n";
    o << "    \"creatureLayout\": {\n";
    o << "      \"position\": {\"x\": \"" << joff(r.creatureLayout.x)
      << "\", \"y\": \"" << joff(r.creatureLayout.y)
      << "\", \"z\": \"" << joff(r.creatureLayout.z)
      << "\", \"direction\": \"" << joff(r.creatureLayout.direction) << "\"},\n";
    o << "      \"positionStatus\": \"" << JsonEscape(r.creatureLayout.positionStatus) << "\",\n";
    o << "      \"name\": {\"kind\": \"" << JsonEscape(r.creatureLayout.nameKind)
      << "\", \"offset\": \"" << joff(r.creatureLayout.nameOffset)
      << "\", \"status\": \"" << JsonEscape(r.creatureLayout.nameStatus)
      << "\", \"mirrorKind\": \"" << JsonEscape(r.creatureLayout.nameMirrorKind)
      << "\", \"mirrorOffset\": \"" << joff(r.creatureLayout.nameMirrorOffset)
      << "\", \"mirrorStatus\": \"" << JsonEscape(r.creatureLayout.nameMirrorStatus) << "\"},\n";
    o << "      \"hpPercent\": {\"type\": \"" << JsonEscape(r.creatureLayout.hpPercent.type)
      << "\", \"offset\": \"" << joff(r.creatureLayout.hpPercent.offset)
      << "\", \"status\": \"" << JsonEscape(r.creatureLayout.hpPercent.status) << "\"},\n";
    o << "      \"id\": {\"type\": \"" << JsonEscape(r.creatureLayout.id.type)
      << "\", \"offset\": \"" << joff(r.creatureLayout.id.offset)
      << "\", \"status\": \"" << JsonEscape(r.creatureLayout.id.status) << "\"},\n";
    const auto writeCreatureScalar = [&](const char* key, const SelectedField& f) {
        o << "      \"" << key << "\": {\"type\": \"" << JsonEscape(f.type)
          << "\", \"offset\": \"" << joff(f.offset)
          << "\", \"status\": \"" << JsonEscape(f.status) << "\"},\n";
    };
    writeCreatureScalar("speed", r.creatureLayout.speed);
    writeCreatureScalar("baseSpeed", r.creatureLayout.baseSpeed);
    writeCreatureScalar("skull", r.creatureLayout.skull);
    writeCreatureScalar("shield", r.creatureLayout.shield);
    o << "      \"outfit\": {\"offset\": \"" << joff(r.creatureLayout.outfitOffset)
      << "\", \"status\": \"" << JsonEscape(r.creatureLayout.outfitStatus) << "\"},\n";
    o << "      \"light\": {\n";
    o << "        \"level\": {\"type\": \"" << JsonEscape(r.creatureLayout.lightLevel.type)
      << "\", \"offset\": \"" << joff(r.creatureLayout.lightLevel.offset)
      << "\", \"status\": \"" << JsonEscape(r.creatureLayout.lightLevel.status) << "\"},\n";
    o << "        \"color\": {\"type\": \"" << JsonEscape(r.creatureLayout.lightColor.type)
      << "\", \"offset\": \"" << joff(r.creatureLayout.lightColor.offset)
      << "\", \"status\": \"" << JsonEscape(r.creatureLayout.lightColor.status) << "\"},\n";
    o << "        \"candidateA\": \"" << joff(r.creatureLayout.lightCandidateA)
      << "\", \"candidateB\": \"" << joff(r.creatureLayout.lightCandidateB)
      << "\", \"status\": \"" << JsonEscape(r.creatureLayout.lightStatus) << "\"\n";
    o << "      }\n";
    o << "    },\n";
    o << "    \"worldLight\": {\n";
    o << "      \"ownerRva\": \"" << HexU32(r.worldLight.ownerRva) << "\",\n";
    o << "      \"setDrawLightsRva\": \"" << HexU32(r.worldLight.setDrawLightsRva) << "\",\n";
    o << "      \"isDrawingLightsRva\": \"" << HexU32(r.worldLight.isDrawingLightsRva) << "\",\n";
    o << "      \"setMinimumAmbientLightRva\": \"" << HexU32(r.worldLight.setMinimumAmbientLightRva) << "\",\n";
    o << "      \"getMinimumAmbientLightRva\": \"" << HexU32(r.worldLight.getMinimumAmbientLightRva) << "\",\n";
    o << "      \"uiMapToMapView\": \"" << joff(r.worldLight.uiMapToMapView) << "\",\n";
    o << "      \"drawLights\": {\"type\": \"" << JsonEscape(r.worldLight.drawLights.type)
      << "\", \"offset\": \"" << joff(r.worldLight.drawLights.offset)
      << "\", \"status\": \"" << JsonEscape(r.worldLight.drawLights.status) << "\"},\n";
    o << "      \"minimumAmbient\": {\"type\": \"" << JsonEscape(r.worldLight.minimumAmbient.type)
      << "\", \"offset\": \"" << joff(r.worldLight.minimumAmbient.offset)
      << "\", \"status\": \"" << JsonEscape(r.worldLight.minimumAmbient.status) << "\"},\n";
    o << "      \"candidateA\": \"" << joff(r.worldLight.candidateA)
      << "\", \"candidateB\": \"" << joff(r.worldLight.candidateB) << "\",\n";
    o << "      \"staticContractStatus\": \"" << JsonEscape(r.worldLight.staticContractStatus) << "\",\n";
    o << "      \"status\": \"" << JsonEscape(r.worldLight.status) << "\"\n";
    o << "    },\n";
    o << "    \"selectedFields\": {\n";
    size_t sfi=0;
    for (const auto& kv : r.selectedFields) {
        o << "      \"" << JsonEscape(kv.first) << "\": {\"type\": \"" << JsonEscape(kv.second.type)
          << "\", \"offset\": \"" << joff(kv.second.offset) << "\", \"status\": \""
          << JsonEscape(kv.second.status) << "\"}" << (++sfi<r.selectedFields.size()?",":"") << "\n";
    }
    o << "    }\n";
    o << "  }\n";
    o << "}\n";
    o.flush();
    o.close();

    const std::string canonical = CanonicalProjectProfilePath(r);
    if (!canonical.empty() && Lower(canonical) != Lower(path)) {
        if (CopyFileA(path.c_str(), canonical.c_str(), FALSE))
            std::cout << "[PROFILE MIRROR] canonical -> " << canonical << "\n";
        else
            std::cout << "[PROFILE MIRROR] falhou Win32=" << GetLastError() << " -> " << canonical << "\n";
    }
    return path;
}

void SaveCheckpoint(Report& r, const std::string& checkpoint) {
    r.checkpoint = checkpoint;
    const std::string path = SaveJson(r);
    if (!path.empty()) std::cout << "[PROFILE SAVE] " << checkpoint << " -> " << path << "\n";
    else std::cout << "[PROFILE SAVE] FAILED at " << checkpoint << "\n";
}

void PrintHeader() {
    std::cout
        << "============================================================\n"
        << "        OTCLab x86 Research Probe V40 - Clear Results R4\n"
        << "[BUILD REVISION] x86-results-r4-simple-console-diagnostic-log\n"
        << "      RTTI-FIRST / SEMANTIC-BIND / RUNTIME-ABI\n"
        << "============================================================\n"
        << "[MODE] descoberta estrutural permanece READ-ONLY\n"
        << "[MODE] fase V33.1 usa 1 byte INT3 temporario/reversivel por acao\n"
        << "[MODE] byte original restaurado no primeiro hit ou cancelamento\n"
        << "[MODE] nenhuma chamada interna do jogo feita pelo probe\n"
        << "[RULE] Nenhum RVA de outra build e usado como oracle\n"
        << "[RULE] Nome do cliente nao restringe a descoberta; exige PE32/i386\n"
        << "[RULE] Cada build usa sua fingerprint e valida seus proprios binds/ABI\n"
        << "[RULE] IDs externos/server sao separados do Client Thing ID em memoria\n"
        << "============================================================\n\n";
}

void OpenConsole() {
    AllocConsole();
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$", "r", stdin);
    SetConsoleTitleA("OTCLab x86 Research Probe V40 - Clear Results R4");
}



// ============================================================
// V24 - INVENTORY / EQUIPMENT + ITEM SCALAR DISCOVERY
// Read-only. One reversible equipment-slot transition is used only when the
// inventory array is not already present in the exact-build profile.
// ============================================================
struct V19InventoryCandidate {
    int baseOffset = -1;
    int stride = 0;
    int itemObjects = 0;
    int nullSlots = 0;
    int invalidSlots = 0;
    int score = 0;
};

int AskIntRangeV19(const std::string& label, int lo, int hi) {
    for (;;) {
        probe_console::ui() << label << " [" << lo << "-" << hi << "]: " << std::flush;
        std::string x; std::getline(std::cin,x);
        try {
            int v=std::stoi(x);
            if(v>=lo && v<=hi) return v;
        } catch(...) {}
        probe_console::ui() << "Valor invalido.\n";
    }
}

bool IsExactRttiObjectV19(const Report& r, uint32_t ptr, const std::string& cls) {
    if(!ptr) return false;
    auto it=r.rtti.find(cls);
    if(it==r.rtti.end() || it->second.vtableRvas.empty()) return false;
    uint32_t vt=0;
    if(!SafeRead(static_cast<uintptr_t>(ptr),vt) || !r.module.inModule(vt)) return false;
    const uint32_t vr=r.module.rva(vt);
    return std::find(it->second.vtableRvas.begin(),it->second.vtableRvas.end(),vr)!=it->second.vtableRvas.end();
}

V19InventoryCandidate ScoreInventoryFromSnapshotV19(const Report& r,
                                                     const std::vector<uint8_t>& snap,
                                                     int baseOff,
                                                     int stride) {
    V19InventoryCandidate c;
    c.baseOffset=baseOff; c.stride=stride;
    if(baseOff<0 || stride<=0 || baseOff+(9*stride)+4>(int)snap.size()) {
        c.invalidSlots=99; return c;
    }
    for(int i=0;i<10;++i) {
        const int off=baseOff+i*stride;
        uint32_t p=0; std::memcpy(&p,snap.data()+off,4);
        if(!p) { ++c.nullSlots; continue; }
        if(IsExactRttiObjectV19(r,p,"Item")) ++c.itemObjects;
        else ++c.invalidSlots;
    }
    c.score=c.itemObjects*100 + c.nullSlots*2 - c.invalidSlots*160;
    if(c.invalidSlots==0) c.score+=80;
    return c;
}

uint32_t FirstInventoryItemV19(const Report& r,int baseOff,int stride,int* outSlot=nullptr) {
    if(!r.localPlayerPtr || baseOff<0 || stride<=0) return 0;
    for(int i=0;i<10;++i) {
        uint32_t p=0;
        if(!SafeRead(r.localPlayerPtr+baseOff+i*stride,p) || !p) continue;
        if(IsExactRttiObjectV19(r,p,"Item")) {
            if(outSlot) *outSlot=i+1;
            return p;
        }
    }
    return 0;
}

struct V19GetterObs {
    std::string type;
    int offset=-1;
    uint32_t getterRva=0;
    uint32_t setterRva=0;
    bool setterAgree=false;
    double value=0.0;
};

bool ReadScalarAtV19(uintptr_t obj,int off,const std::string& type,double& out) {
    if(!obj || off<0) return false;
    if(type=="u8") { uint8_t v=0; if(!SafeRead(obj+off,v)) return false; out=v; return true; }
    if(type=="u16") { uint16_t v=0; if(!SafeRead(obj+off,v)) return false; out=v; return true; }
    if(type=="u32") { uint32_t v=0; if(!SafeRead(obj+off,v)) return false; out=v; return true; }
    if(type=="float") { float v=0; if(!SafeRead(obj+off,v) || !std::isfinite(v)) return false; out=v; return true; }
    if(type=="double") { double v=0; if(!SafeRead(obj+off,v) || !std::isfinite(v)) return false; out=v; return true; }
    return false;
}

std::vector<V19GetterObs> ItemGetterObservationsV19(const Report& r,
                                                     const std::string& getToken,
                                                     const std::string& setToken,
                                                     uintptr_t item) {
    std::vector<V19GetterObs> out;
    const auto git=r.binds.find(getToken);
    if(git==r.binds.end()) return out;
    const auto sit=r.binds.find(setToken);
    const std::array<const char*,5> types={"u8","u16","u32","float","double"};
    std::set<std::tuple<std::string,int,uint32_t>> seen;
    for(const auto& gc:git->second.candidates) {
        for(const char* t:types) {
            int off=-1;
            if(!DecodeDirectScalarGetterV18(r.module.base+gc.methodRva,t,off)) continue;
            double v=0;
            if(!ReadScalarAtV19(item,off,t,v)) continue;
            V19GetterObs h; h.type=t; h.offset=off; h.getterRva=gc.methodRva; h.value=v;
            if(sit!=r.binds.end()) {
                for(const auto& sc:sit->second.candidates) {
                    if(MethodReferencesThisOffsetV18(r.module.base+sc.methodRva,off)) {
                        h.setterAgree=true; h.setterRva=sc.methodRva; break;
                    }
                }
            }
            if(seen.insert({h.type,h.offset,h.getterRva}).second) out.push_back(h);
        }
    }
    return out;
}


// ============================================================
// V24 - INVENTORY / EQUIPMENT + ITEM ID/COUNT
// Creature-light discovery was intentionally removed from the roadmap/output.
// V20 identifies Item.ID by tying getId candidates to the Item RTTI vtable and
// cross-checking several live equipped Item objects. Item.Count is recovered by
// getter/setter body field-use intersection, with an optional reversible stack
// transition fallback that does not require the user to type the count.
// ============================================================
struct V20MemRef {
    int offset=-1;
    int width=0;
    bool read=false;
    bool write=false;
};

bool V20MethodInClassVtable(const Report& r,
                            const std::string& cls,
                            uint32_t methodRva,
                            int* slotOut=nullptr) {
    auto it=r.rtti.find(cls);
    if(it==r.rtti.end() || it->second.vtableRvas.empty() || !methodRva) return false;
    const uint32_t methodAbs=static_cast<uint32_t>(r.module.base+methodRva);
    for(uint32_t vtRva:it->second.vtableRvas) {
        const uintptr_t vt=r.module.base+vtRva;
        for(int slot=0;slot<128;++slot) {
            uint32_t fn=0;
            if(!SafeRead(vt+slot*4,fn)) break;
            if(fn==methodAbs) {
                if(slotOut) *slotOut=slot;
                return true;
            }
            // vtables are contiguous .text pointers. Stop after a few clearly
            // non-code entries rather than wandering into neighboring data.
            if(slot>16 && (!fn || !r.module.inText(fn))) break;
        }
    }
    return false;
}

std::vector<uint32_t> V20CollectInventoryItems(const Report& r,int base,int stride) {
    std::vector<uint32_t> out;
    if(!r.localPlayerPtr || base<0 || stride<=0) return out;
    std::set<uint32_t> seen;
    for(int i=0;i<10;++i) {
        uint32_t p=0;
        if(!SafeRead(r.localPlayerPtr+base+i*stride,p) || !p) continue;
        if(!IsExactRttiObjectV19(r,p,"Item")) continue;
        if(seen.insert(p).second) out.push_back(p);
    }
    return out;
}

bool V20DecodeMemBaseDisp(const std::vector<uint8_t>& b,
                          size_t modrmPos,
                          const std::set<int>& thisRegs,
                          int& disp) {
    disp=0;
    if(modrmPos>=b.size()) return false;
    const uint8_t mr=b[modrmPos];
    const int mod=(mr>>6)&3;
    int rm=mr&7;
    if(mod==3) return false;
    size_t p=modrmPos+1;
    int baseReg=rm;
    if(rm==4) {
        if(p>=b.size()) return false;
        const uint8_t sib=b[p++];
        baseReg=sib&7;
        if(mod==0 && baseReg==5) return false;
    } else if(mod==0 && rm==5) {
        return false;
    }
    if(!thisRegs.count(baseReg)) return false;
    if(mod==1) {
        if(p>=b.size()) return false;
        disp=static_cast<int8_t>(b[p]);
    } else if(mod==2) {
        if(p+4>b.size()) return false;
        int32_t d=0; std::memcpy(&d,b.data()+p,4); disp=d;
    }
    return disp>=0 && disp<=0x400;
}

std::vector<V20MemRef> V20CollectThisFieldRefs(uintptr_t fn) {
    std::vector<V20MemRef> out;
    if(!fn) return out;
    std::vector<uint8_t> b(256);
    if(!SafeCopy(fn,b.data(),b.size())) return out;

    std::set<int> thisRegs{1}; // ECX
    // Over-approximate simple aliases used by MSVC prologues: mov reg,reg and
    // lea reg,[this+0]. Pairing with both getter and setter keeps this safe.
    for(size_t i=0;i+2<b.size();++i) {
        if(b[i]==0x8B) {
            const uint8_t mr=b[i+1];
            if(((mr>>6)&3)==3) {
                const int dst=(mr>>3)&7, src=mr&7;
                if(thisRegs.count(src)) thisRegs.insert(dst);
            }
        } else if(b[i]==0x89) {
            const uint8_t mr=b[i+1];
            if(((mr>>6)&3)==3) {
                const int src=(mr>>3)&7, dst=mr&7;
                if(thisRegs.count(src)) thisRegs.insert(dst);
            }
        }
    }

    auto add=[&](size_t modrmPos,int width,bool rd,bool wr) {
        int d=0;
        if(!V20DecodeMemBaseDisp(b,modrmPos,thisRegs,d)) return;
        V20MemRef x{d,width,rd,wr};
        for(auto& e:out) {
            if(e.offset==x.offset && e.width==x.width) {
                e.read = e.read || x.read;
                e.write = e.write || x.write;
                return;
            }
        }
        out.push_back(x);
    };

    for(size_t i=0;i+8<b.size();++i) {
        const uint8_t op=b[i];
        if(op==0x8A) add(i+1,1,true,false);      // mov r8,[mem]
        if(op==0x88) add(i+1,1,false,true);      // mov [mem],r8
        if(op==0x8B) add(i+1,4,true,false);      // mov r32,[mem]
        if(op==0x89) add(i+1,4,false,true);      // mov [mem],r32
        if(op==0xC6) add(i+1,1,false,true);      // mov byte [mem],imm8
        if(op==0xC7) add(i+1,4,false,true);      // mov dword [mem],imm32
        if(op==0x66 && i+2<b.size() && b[i+1]==0x8B) add(i+2,2,true,false);
        if(op==0x66 && i+2<b.size() && b[i+1]==0x89) add(i+2,2,false,true);
        if(op==0x0F && i+2<b.size() && b[i+1]==0xB6) add(i+2,1,true,false);
        if(op==0x0F && i+2<b.size() && b[i+1]==0xB7) add(i+2,2,true,false);
    }
    return out;
}

std::string V20TypeForWidth(int w) {
    if(w==1) return "u8";
    if(w==2) return "u16";
    if(w==4) return "u32";
    return "bytes";
}

struct V20IdCandidate {
    std::string type;
    int offset=-1;
    uint32_t getterRva=0;
    uint32_t setterRva=0;
    bool setterAgree=false;
    bool itemVtable=false;
    int vtableSlot=-1;
    int distinctValues=0;
    bool plausible=true;
    int score=0;
    std::vector<uint32_t> values;
};

std::vector<V20IdCandidate> V20BuildItemIdCandidates(const Report& r,
                                                      const std::vector<uint32_t>& items) {
    std::vector<V20IdCandidate> out;
    if(items.empty()) return out;
    const auto git=r.binds.find("getId");
    if(git==r.binds.end()) return out;
    const auto sit=r.binds.find("setId");
    std::set<std::tuple<std::string,int,uint32_t>> seen;
    for(const auto& gc:git->second.candidates) {
        for(const char* t:{"u16","u32"}) {
            int off=-1;
            if(!DecodeDirectScalarGetterV18(r.module.base+gc.methodRva,t,off)) continue;
            if(!seen.insert({std::string(t),off,gc.methodRva}).second) continue;
            V20IdCandidate c;
            c.type=t; c.offset=off; c.getterRva=gc.methodRva;
            c.itemVtable=V20MethodInClassVtable(r,"Item",gc.methodRva,&c.vtableSlot);
            if(sit!=r.binds.end()) {
                for(const auto& sc:sit->second.candidates) {
                    if(MethodReferencesThisOffsetV18(r.module.base+sc.methodRva,off)) {
                        c.setterAgree=true; c.setterRva=sc.methodRva; break;
                    }
                    // Complex Item::setId implementations may alias this and
                    // touch the field deeper in the body; use V20 refs too.
                    for(const auto& ref:V20CollectThisFieldRefs(r.module.base+sc.methodRva)) {
                        if(ref.write && ref.offset==off) {
                            c.setterAgree=true; c.setterRva=sc.methodRva; break;
                        }
                    }
                    if(c.setterAgree) break;
                }
            }
            std::set<uint32_t> distinct;
            for(uint32_t p:items) {
                double dv=0;
                if(!ReadScalarAtV19(p,off,t,dv)) { c.plausible=false; continue; }
                uint32_t v=static_cast<uint32_t>(dv);
                c.values.push_back(v);
                distinct.insert(v);
                if(v==0 || v>65535) c.plausible=false;
            }
            c.distinctValues=static_cast<int>(distinct.size());
            c.score=0;
            if(c.itemVtable) c.score+=700;
            if(c.setterAgree) c.score+=220;
            if(c.plausible) c.score+=160;
            if(c.distinctValues>=2) c.score+=160;
            c.score+=std::min(5,c.distinctValues)*25;
            if(c.distinctValues<=1) c.score-=120;
            out.push_back(c);
        }
    }
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){
        if(a.score!=b.score) return a.score>b.score;
        if(a.itemVtable!=b.itemVtable) return a.itemVtable>b.itemVtable;
        if(a.setterAgree!=b.setterAgree) return a.setterAgree>b.setterAgree;
        if(a.offset!=b.offset) return a.offset<b.offset;
        return a.getterRva<b.getterRva;
    });
    return out;
}

struct V20CountCandidate {
    int offset=-1;
    int width=0;
    uint32_t getterRva=0;
    uint32_t setterRva=0;
    int score=0;
};

std::vector<V20CountCandidate> V20BuildCountBodyCandidates(const Report& r) {
    std::vector<V20CountCandidate> out;
    const auto git=r.binds.find("getCount");
    const auto sit=r.binds.find("setCount");
    if(git==r.binds.end() || sit==r.binds.end()) return out;
    std::map<std::pair<int,int>,V20CountCandidate> best;
    for(const auto& gc:git->second.candidates) {
        const auto gr=V20CollectThisFieldRefs(r.module.base+gc.methodRva);
        for(const auto& sc:sit->second.candidates) {
            const auto sr=V20CollectThisFieldRefs(r.module.base+sc.methodRva);
            for(const auto& a:gr) {
                if(!a.read) continue;
                for(const auto& b:sr) {
                    if(!b.write || a.offset!=b.offset) continue;
                    // Width can differ because MOVZX reads byte/word while a
                    // setter uses a wider register spill. Prefer exact width,
                    // but keep same-offset evidence as a fallback.
                    const int w=(a.width==b.width ? a.width : std::min(a.width,b.width));
                    if(w!=1 && w!=2 && w!=4) continue;
                    V20CountCandidate c;
                    c.offset=a.offset; c.width=w; c.getterRva=gc.methodRva; c.setterRva=sc.methodRva;
                    c.score=gc.score+sc.score + (a.width==b.width?180:80);
                    if(git->second.selectedOk && git->second.selected.methodRva==gc.methodRva) c.score+=100;
                    if(sit->second.selectedOk && sit->second.selected.methodRva==sc.methodRva) c.score+=100;
                    int slot=-1;
                    if(V20MethodInClassVtable(r,"Item",gc.methodRva,&slot)) c.score+=180;
                    auto key=std::make_pair(c.offset,c.width);
                    auto it=best.find(key);
                    if(it==best.end() || c.score>it->second.score) best[key]=c;
                }
            }
        }
    }
    for(auto& kv:best) out.push_back(kv.second);
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){
        if(a.score!=b.score) return a.score>b.score;
        if(a.offset!=b.offset) return a.offset<b.offset;
        return a.width<b.width;
    });
    return out;
}

bool V20CaptureItemFromSlot(const Report& r,int base,int stride,int slot,
                            uint32_t& ptr,std::vector<uint8_t>& snap,size_t win=0x100) {
    ptr=0; snap.clear();
    if(slot<1 || slot>10) return false;
    if(!SafeRead(r.localPlayerPtr+base+(slot-1)*stride,ptr) || !ptr) return false;
    if(!IsExactRttiObjectV19(r,ptr,"Item")) return false;
    return CaptureWindow(ptr,snap,win);
}

struct V20TransitionHit { int offset=-1; int width=0; uint32_t a=0,b=0,c=0; int score=0; };

std::vector<V20TransitionHit> V20FindCountTransitionHits(const std::vector<uint8_t>& a,
                                                         const std::vector<uint8_t>& b,
                                                         const std::vector<uint8_t>& c,
                                                         const std::vector<V20CountCandidate>& semantic) {
    std::vector<V20TransitionHit> out;
    const size_t n=std::min({a.size(),b.size(),c.size(),size_t(0x100)});
    std::set<std::pair<int,int>> semanticKeys;
    std::set<int> semanticOffsets;
    for(const auto& s:semantic) { semanticKeys.insert({s.offset,s.width}); semanticOffsets.insert(s.offset); }
    auto add=[&](int off,int w,uint32_t va,uint32_t vb,uint32_t vc){
        if(va!=vc || va==vb) return;
        if(va>10000 || vb>10000) return;
        V20TransitionHit h{off,w,va,vb,vc,0};
        if(semanticKeys.count({off,w})) h.score+=500;
        else if(semanticOffsets.count(off)) h.score+=300;
        if(w==1 && va<=255 && vb<=255) h.score+=100;
        if(va>0 && vb>0) h.score+=50;
        out.push_back(h);
    };
    for(size_t off=0;off<n;++off) {
        add(static_cast<int>(off),1,a[off],b[off],c[off]);
        if(off+2<=n) { uint16_t x=0,y=0,z=0; std::memcpy(&x,a.data()+off,2); std::memcpy(&y,b.data()+off,2); std::memcpy(&z,c.data()+off,2); add((int)off,2,x,y,z); }
        if(off+4<=n) { uint32_t x=0,y=0,z=0; std::memcpy(&x,a.data()+off,4); std::memcpy(&y,b.data()+off,4); std::memcpy(&z,c.data()+off,4); add((int)off,4,x,y,z); }
    }
    std::sort(out.begin(),out.end(),[](const auto& x,const auto& y){
        if(x.score!=y.score) return x.score>y.score;
        if(x.offset!=y.offset) return x.offset<y.offset;
        return x.width<y.width;
    });
    return out;
}


struct V24SlotItem {
    int slot=0;
    uint32_t ptr=0;
};

std::vector<V24SlotItem> V24CollectInventorySlotItems(const Report& r,int base,int stride) {
    std::vector<V24SlotItem> out;
    for(int i=0;i<10;++i) {
        uint32_t p=0;
        if(!SafeRead(r.localPlayerPtr+base+i*stride,p) || !p) continue;
        if(!IsExactRttiObjectV19(r,p,"Item")) continue;
        out.push_back({i+1,p});
    }
    return out;
}

bool V24TryPromoteItemIdByConfirmedCount(Report& r,int base,int stride) {
    auto ic=r.selectedFields.find("itemCount");
    if(ic==r.selectedFields.end() || ic->second.offset<0 ||
       EvidenceScore(ic->second.status)<80) {
        std::cout << "[ITEM ID XCHECK] Item.Count ainda nao esta confirmado; adiando cross-check estrutural.\n";
        return false;
    }

    const int countOff=ic->second.offset;
    const std::string countType=Lower(ic->second.type);
    if(countType!="u16") {
        std::cout << "[ITEM ID XCHECK] Item.Count confirmado, mas tipo=" << ic->second.type
                  << "; regra de adjacencia u16 nao sera aplicada.\n";
        return false;
    }

    const auto slots=V24CollectInventorySlotItems(r,base,stride);
    if(slots.size()<2) {
        std::cout << "[ITEM ID XCHECK] poucos Item* vivos (" << slots.size()
                  << "); precisamos de pelo menos 2 para separar ID de campos constantes.\n";
        return false;
    }

    std::vector<uint32_t> ptrs;
    for(const auto& s:slots) ptrs.push_back(s.ptr);
    auto ids=V20BuildItemIdCandidates(r,ptrs);

    struct Hit {
        V20IdCandidate c;
        int slotMatches=0;
        int pairMatches=0;
        int distinct=0;
        int score=0;
    };
    std::vector<Hit> hits;

    for(const auto& c:ids) {
        if(Lower(c.type)!="u16") continue;
        if(c.offset+2!=countOff) continue;

        Hit h; h.c=c; h.score=c.score+900; // confirmed count adjacency is strong structure
        std::set<uint32_t> distinct;
        bool ok=true;
        for(const auto& s:slots) {
            uint16_t id=0,count=0;
            uint32_t packed=0;
            if(!SafeRead(s.ptr+c.offset,id) ||
               !SafeRead(s.ptr+countOff,count) ||
               !SafeRead(s.ptr+c.offset,packed)) {
                ok=false; break;
            }
            distinct.insert(id);
            if(id==static_cast<uint16_t>(s.slot)) ++h.slotMatches;
            if(static_cast<uint16_t>(packed & 0xFFFFu)==id &&
               static_cast<uint16_t>((packed>>16)&0xFFFFu)==count) {
                ++h.pairMatches;
            }
            if(id==0 || id==0xFFFFu) ok=false;
        }
        h.distinct=static_cast<int>(distinct.size());
        if(!ok) continue;
        if(h.pairMatches==static_cast<int>(slots.size())) h.score+=700;
        if(h.distinct>=2) h.score+=250;
        if(h.slotMatches==0) h.score+=220;
        if(h.slotMatches*4>=static_cast<int>(slots.size())*3) h.score-=1200; // looks like slot index
        if(c.setterAgree) h.score+=80;
        hits.push_back(h);
    }

    std::sort(hits.begin(),hits.end(),[](const Hit& a,const Hit& b){
        if(a.score!=b.score) return a.score>b.score;
        return a.c.offset<b.c.offset;
    });

    std::cout << "[ITEM ID COUNT-ADJACENCY XCHECK] candidates=" << hits.size() << "\n";
    for(size_t i=0;i<hits.size() && i<8;++i) {
        const auto& h=hits[i];
        std::cout << "  u16 +" << HexU32(h.c.offset)
                  << " immediately-before Count +" << HexU32(countOff)
                  << " pairMatches=" << h.pairMatches << "/" << slots.size()
                  << " slotMatches=" << h.slotMatches
                  << " distinct=" << h.distinct
                  << " getter=" << HexU32(h.c.getterRva)
                  << " score=" << h.score << "\n";
    }

    if(hits.empty()) return false;
    const auto& best=hits[0];
    const bool separated=hits.size()==1 || best.score>=hits[1].score+300;
    const bool strong=best.pairMatches==static_cast<int>(slots.size()) &&
                      best.distinct>=2 &&
                      best.slotMatches*4 < static_cast<int>(slots.size())*3;
    if(!strong || !separated) {
        std::cout << "[ITEM ID XCHECK] ainda ambiguo; nenhum offset promovido.\n";
        return false;
    }

    const std::string st="semantic_current_build_item_id_count_adjacency_multi_item";
    r.selectedFields["itemId"]={"u16",best.c.offset,st};
    std::cout << "[ITEM ID] CONFIRMADO u16 +" << HexU32(best.c.offset)
              << " por getId da build atual + adjacencia ao Item.Count u16 +"
              << HexU32(countOff)
              << " + decomposicao [ID low16 | Count high16] em "
              << slots.size() << " Item* + rejeicao de correlacao com slot.\n";
    SaveCheckpoint(r,"item_id_count_adjacency_multi_item");
    return true;
}

bool V24TryItemIdRuntimeABA(Report& r,int base,int stride) {
    auto ids=V20BuildItemIdCandidates(r,V20CollectInventoryItems(r,base,stride));
    std::vector<V20IdCandidate> cand;
    for(const auto& c:ids) {
        if(Lower(c.type)=="u16" && c.offset>=0) cand.push_back(c);
    }
    if(cand.empty()) return false;

    if(AskYesNo("Item.ID ainda pending. Validar colocando Item A -> Item B diferente -> Item A no MESMO slot? [S/N]")!='s')
        return false;

    std::cout << "Use dois itens DIFERENTES que podem ocupar o mesmo slot. Nao precisa saber os IDs.\n";
    const int slot=AskIntRangeV19("Qual slot sera usado",1,10);

    auto capture=[&](const char* prompt,uint32_t& p)->bool{
        WaitEnter(prompt);
        p=0;
        return SafeRead(r.localPlayerPtr+base+(slot-1)*stride,p) &&
               p && IsExactRttiObjectV19(r,p,"Item");
    };

    uint32_t pa=0,pb=0,pc=0;
    if(!capture("Equipe o ITEM A e ENTER... ",pa)) { std::cout << "[ITEM ID ABA] Item A invalido.\n"; return false; }
    if(!capture("Troque pelo ITEM B DIFERENTE e ENTER... ",pb)) { std::cout << "[ITEM ID ABA] Item B invalido.\n"; return false; }
    if(!capture("Volte ao MESMO TIPO de ITEM A e ENTER... ",pc)) { std::cout << "[ITEM ID ABA] Item A final invalido.\n"; return false; }

    struct H { V20IdCandidate c; uint32_t a=0,b=0,d=0; int score=0; };
    std::vector<H> hits;
    for(const auto& c:cand) {
        double da=0,db=0,dc=0;
        if(!ReadScalarAtV19(pa,c.offset,c.type,da) ||
           !ReadScalarAtV19(pb,c.offset,c.type,db) ||
           !ReadScalarAtV19(pc,c.offset,c.type,dc)) continue;
        uint32_t a=static_cast<uint32_t>(da), b=static_cast<uint32_t>(db), d=static_cast<uint32_t>(dc);
        if(a==0 || b==0 || d==0 || a>65535 || b>65535 || d>65535) continue;
        if(a!=d || a==b) continue;
        H h{c,a,b,d,c.score+800};
        auto ic=r.selectedFields.find("itemCount");
        if(ic!=r.selectedFields.end() && ic->second.offset>=0 &&
           c.offset+2==ic->second.offset) h.score+=600;
        hits.push_back(h);
    }
    std::sort(hits.begin(),hits.end(),[](const H& x,const H& y){
        if(x.score!=y.score) return x.score>y.score;
        return x.c.offset<y.c.offset;
    });

    std::cout << "[ITEM ID ABA HITS] " << hits.size() << "\n";
    for(size_t i=0;i<hits.size() && i<8;++i)
        std::cout << "  u16 +" << HexU32(hits[i].c.offset)
                  << " " << hits[i].a << " -> " << hits[i].b << " -> " << hits[i].d
                  << " score=" << hits[i].score << "\n";

    if(hits.empty()) return false;
    const bool unique=hits.size()==1 || hits[0].score>=hits[1].score+250;
    if(!unique) return false;
    r.selectedFields["itemId"]={"u16",hits[0].c.offset,"runtime_item_id_two_items_aba"};
    std::cout << "[ITEM ID] CONFIRMADO u16 +" << HexU32(hits[0].c.offset)
              << " por Item A -> Item B -> Item A no mesmo slot.\n";
    SaveCheckpoint(r,"item_id_runtime_aba");
    return true;
}


void RunInventoryItemDiscoveryV24(Report& r) {
    std::cout << "\n============================================================\n"
              << "          INVENTORY / EQUIPMENT + ITEM V30\n"
              << "============================================================\n"
              << "[METHOD] Item RTTI + inventory profile + Item-vtable semantic cross-check.\n"
              << "[MODE] READ-ONLY: nenhuma escrita e nenhuma chamada interna.\n";
    if(!r.localPlayerPtr) { std::cout << "[INVENTORY] LocalPlayer ausente.\n"; return; }

    int base=-1, stride=0;
    auto ia=r.selectedFields.find("inventoryArray");
    auto is=r.selectedFields.find("inventoryStride");
    if(ia!=r.selectedFields.end() && is!=r.selectedFields.end() &&
       ia->second.offset>=0 && is->second.offset>0 &&
       EvidenceScore(ia->second.status)>=80 && EvidenceScore(is->second.status)>=80) {
        base=ia->second.offset; stride=is->second.offset;
        int live=0;
        for(int i=0;i<10;++i) {
            uint32_t p=0;
            if(SafeRead(r.localPlayerPtr+base+i*stride,p) && p && IsExactRttiObjectV19(r,p,"Item")) ++live;
        }
        std::cout << "[INVENTORY QUICK-CHECK] array +" << HexU32(base)
                  << " stride=" << stride << " live Item*=" << live << " [profile]\n";
    } else {
        if(AskYesNo("Mapear os 10 slots de equipamento/inventario agora com UMA transicao reversivel?")!='s') {
            std::cout << "[INVENTORY] teste pulado; mantendo pending.\n"; return;
        }
        std::cout << "Slots OTC: 1=head 2=necklace 3=backpack 4=armor 5=right 6=left 7=legs 8=feet 9=ring 10=ammo\n";
        const int slot=AskIntRangeV19("Qual slot voce vai usar no teste",1,10);
        const size_t win=0x800;
        std::vector<uint8_t> a,b,c;
        WaitEnter("Deixe ESSE slot VAZIO e nao altere mais nada. ENTER... ");
        if(!CaptureWindow(r.localPlayerPtr,a,win)) return;
        WaitEnter("Equipe UM item nesse mesmo slot. ENTER... ");
        if(!CaptureWindow(r.localPlayerPtr,b,win)) return;
        WaitEnter("Remova o item e deixe o slot VAZIO novamente. ENTER... ");
        if(!CaptureWindow(r.localPlayerPtr,c,win)) return;
        std::vector<std::pair<int,uint32_t>> slotHits;
        for(int off=0;off+4<=(int)std::min({a.size(),b.size(),c.size()});off+=4) {
            uint32_t pa=0,pb=0,pc=0;
            std::memcpy(&pa,a.data()+off,4); std::memcpy(&pb,b.data()+off,4); std::memcpy(&pc,c.data()+off,4);
            if(pa!=0 || pc!=0 || !pb || !IsExactRttiObjectV19(r,pb,"Item")) continue;
            slotHits.push_back({off,pb});
        }
        std::vector<V19InventoryCandidate> candidates;
        for(const auto& h:slotHits) for(int st:{4,8}) {
            const int bo=h.first-(slot-1)*st;
            auto cc=ScoreInventoryFromSnapshotV19(r,b,bo,st);
            if(cc.baseOffset>=0 && cc.invalidSlots==0 && cc.itemObjects>=1) candidates.push_back(cc);
        }
        std::sort(candidates.begin(),candidates.end(),[](const auto& x,const auto& y){
            if(x.score!=y.score) return x.score>y.score; return x.baseOffset<y.baseOffset;
        });
        if(!candidates.empty() && (candidates.size()==1 || candidates[0].score>candidates[1].score)) {
            base=candidates[0].baseOffset; stride=candidates[0].stride;
            const std::string st="runtime_inventory_slot_transition_item_rtti";
            r.selectedFields["inventoryArray"]={"ptr_array10",base,st};
            r.selectedFields["inventoryStride"]={"bytes",stride,st};
            std::cout << "[INVENTORY] CONFIRMADO LocalPlayer +" << HexU32(base)
                      << " stride=" << stride << " 10 slots por transicao + RTTI Item.\n";
            SaveCheckpoint(r,"inventory_equipment_transition");
        }
        if(base<0) { std::cout << "[INVENTORY] ambiguo; nenhum array foi promovido.\n"; return; }
    }

    auto items=V20CollectInventoryItems(r,base,stride);
    std::cout << "[LIVE EQUIPPED ITEMS] " << items.size() << " Item* validos\n";

    // ---------------- Item.ID ----------------
    bool itemIdReady=false;
    auto iid=r.selectedFields.find("itemId");
    if(iid!=r.selectedFields.end() && iid->second.offset>=0 && EvidenceScore(iid->second.status)>=80) {
        itemIdReady=true;
        std::cout << "[ITEM ID QUICK-CHECK] +" << HexU32(iid->second.offset)
                  << " type=" << iid->second.type << " [profile]\n";
    } else {
        auto ids=V20BuildItemIdCandidates(r,items);
        std::cout << "[ITEM ID CANDIDATES] " << ids.size() << "\n";
        for(size_t i=0;i<ids.size() && i<10;++i) {
            const auto& c=ids[i];
            std::cout << "  " << c.type << " +" << HexU32(c.offset)
                      << " getter=" << HexU32(c.getterRva)
                      << " itemVTable=" << (c.itemVtable?"YES":"NO");
            if(c.itemVtable) std::cout << " slot=" << c.vtableSlot;
            std::cout << " setter=" << (c.setterAgree?HexU32(c.setterRva):std::string("<no-match>"))
                      << " distinct=" << c.distinctValues << " score=" << c.score;
            if(!c.values.empty()) { std::cout << " values="; for(size_t k=0;k<c.values.size() && k<6;++k) std::cout << (k?",":"") << c.values[k]; }
            std::cout << "\n";
        }
        if(!ids.empty()) {
            const auto& best=ids[0];
            const bool separated=ids.size()==1 || best.score>=ids[1].score+180;
            const bool strong=best.itemVtable && best.plausible &&
                              (best.distinctValues>=2 || (best.setterAgree && !items.empty()));
            if(strong && separated) {
                const std::string st="semantic_current_build_item_rtti_vtable_getid_multi_item";
                r.selectedFields["itemId"]={best.type,best.offset,st};
                itemIdReady=true;
                std::cout << "[ITEM ID] CONFIRMADO " << best.type << " +" << HexU32(best.offset)
                          << " por getId no Item vtable + runtime em " << items.size() << " Item*.\n";
                SaveCheckpoint(r,"item_id_rtti_vtable_multi_item");
            } else {
                std::cout << "[ITEM ID] ainda ambiguo; nenhum offset promovido.\n";
            }
        }
    }

    // ---------------- Item.Count ----------------
    bool itemCountReady=false;
    auto ic=r.selectedFields.find("itemCount");
    if(ic!=r.selectedFields.end() && ic->second.offset>=0 && EvidenceScore(ic->second.status)>=80) {
        itemCountReady=true;
        std::cout << "[ITEM COUNT QUICK-CHECK] +" << HexU32(ic->second.offset)
                  << " type=" << ic->second.type << " [profile]\n";
    } else {
        auto cc=V20BuildCountBodyCandidates(r);
        std::cout << "[ITEM COUNT BODY CANDIDATES] " << cc.size() << "\n";
        for(size_t i=0;i<cc.size() && i<10;++i) {
            std::cout << "  " << V20TypeForWidth(cc[i].width) << " +" << HexU32(cc[i].offset)
                      << " getter=" << HexU32(cc[i].getterRva)
                      << " setter=" << HexU32(cc[i].setterRva)
                      << " score=" << cc[i].score << "\n";
        }
        if(!cc.empty()) {
            const bool unique=cc.size()==1 || cc[0].score>=cc[1].score+140;
            if(unique) {
                r.selectedFields["itemCount"]={V20TypeForWidth(cc[0].width),cc[0].offset,
                    "semantic_current_build_item_count_body_getter_setter"};
                itemCountReady=true;
                std::cout << "[ITEM COUNT] CONFIRMADO " << V20TypeForWidth(cc[0].width)
                          << " +" << HexU32(cc[0].offset)
                          << " por field-use comum em getCount/setCount.\n";
                SaveCheckpoint(r,"item_count_body_getter_setter");
            }
        }
    }

    // Runtime fallback only if static body analysis could not close Count.
    if(!itemCountReady) {
        if(AskYesNo("Item.Count ainda pending. Validar com UMA pilha que muda quantidade e volta? [S/N]")=='s') {
            std::cout << "Use um slot com item EMPILHAVEL. Nao troque o tipo do item; apenas altere a quantidade.\n";
            const int slot=AskIntRangeV19("Qual slot contem a pilha",1,10);
            uint32_t pa=0,pb=0,pc=0; std::vector<uint8_t> a,b,c;
            WaitEnter("Deixe a pilha na quantidade inicial e ENTER... ");
            if(!V20CaptureItemFromSlot(r,base,stride,slot,pa,a)) {
                std::cout << "[COUNT TRANSITION] slot sem Item valido.\n";
            } else {
                WaitEnter("ALTERE a quantidade da MESMA pilha sem zerar/remover e ENTER... ");
                if(!V20CaptureItemFromSlot(r,base,stride,slot,pb,b)) {
                    std::cout << "[COUNT TRANSITION] item ausente no estado B.\n";
                } else {
                    WaitEnter("VOLTE a quantidade exatamente ao estado inicial e ENTER... ");
                    if(!V20CaptureItemFromSlot(r,base,stride,slot,pc,c)) {
                        std::cout << "[COUNT TRANSITION] item ausente no estado C.\n";
                    } else {
                        bool sameId=true;
                        auto fit=r.selectedFields.find("itemId");
                        if(fit!=r.selectedFields.end() && fit->second.offset>=0) {
                            double xa=0,xb=0,xc=0;
                            sameId=ReadScalarAtV19(pa,fit->second.offset,fit->second.type,xa) &&
                                   ReadScalarAtV19(pb,fit->second.offset,fit->second.type,xb) &&
                                   ReadScalarAtV19(pc,fit->second.offset,fit->second.type,xc) &&
                                   xa==xb && xa==xc;
                            std::cout << "[COUNT TRANSITION] same Item.ID across A/B/C: " << (sameId?"YES":"NO") << "\n";
                        }
                        auto semantic=V20BuildCountBodyCandidates(r);
                        auto hits=V20FindCountTransitionHits(a,b,c,semantic);
                        std::cout << "[COUNT TRANSITION HITS] " << hits.size() << "\n";
                        for(size_t i=0;i<hits.size() && i<12;++i) {
                            std::cout << "  " << V20TypeForWidth(hits[i].width) << " +" << HexU32(hits[i].offset)
                                      << " " << hits[i].a << " -> " << hits[i].b << " -> " << hits[i].c
                                      << " score=" << hits[i].score << "\n";
                        }
                        if(sameId && !hits.empty()) {
                            // Collapse identical offset observations. Prefer a
                            // semantic width when body analysis has one.
                            const auto& best=hits[0];
                            bool unique=true;
                            for(size_t i=1;i<hits.size();++i) {
                                if(hits[i].score==best.score && hits[i].offset!=best.offset) { unique=false; break; }
                            }
                            if(unique && (best.score>=300 || hits.size()==1)) {
                                r.selectedFields["itemCount"]={V20TypeForWidth(best.width),best.offset,
                                    "runtime_transition_item_count_same_item_abc"};
                                itemCountReady=true;
                                std::cout << "[ITEM COUNT] CONFIRMADO " << V20TypeForWidth(best.width)
                                          << " +" << HexU32(best.offset)
                                          << " por A->B->A na mesma pilha.\n";
                                SaveCheckpoint(r,"item_count_runtime_transition");
                            }
                        }
                    }
                }
            }
        }
    }

    // V24: Item.Count is now a structural anchor for Item.ID. In this build
    // the real ID candidate is a u16 immediately before the confirmed u16 Count.
    // We do not assume the offset; we require current-build getId evidence,
    // multi-Item runtime values, packed low16/high16 agreement, and reject any
    // candidate that simply mirrors the equipment slot index.
    if(!itemIdReady && itemCountReady)
        itemIdReady=V24TryPromoteItemIdByConfirmedCount(r,base,stride);

    // Last-resort runtime confirmation without asking the numeric IDs.
    if(!itemIdReady)
        itemIdReady=V24TryItemIdRuntimeABA(r,base,stride);

    if(!itemIdReady) std::cout << "Item.ID        : PENDING\n";
    if(!itemCountReady) std::cout << "Item.Count     : PENDING\n";
    std::cout << "[ITEM ID SEMANTICS] +0x2E representa o ID interno/client Thing ID observado pelo cliente; IDs externos/server/OTB podem ser diferentes e devem ser mapeados separadamente.\n";
    SaveCheckpoint(r,"inventory_item_v24");
}


// ============================================================
// V24 - OPEN CONTAINER / CONTAINER OBJECT DISCOVERY (x86)
// READ-ONLY. Uses a reversible CLOSED -> OPEN -> CLOSED transition.
// No foreign-build offsets are copied. The transition only identifies the
// current-build Game collection and a live RTTI Container object; fields are
// then cross-checked with current-build semantic accessors and known Item RTTI.
// ============================================================
struct V24OpenMapCandidate {
    int mapOffset=-1;          // relative to Game object
    int sizeOffset=-1;         // relative to Game object
    uint32_t head=0;
    uint32_t node=0;
    uint32_t container=0;
    int nodeContainerOffset=-1;
    bool hasKey=false;
    uint32_t key=0;
    int keyOffset=-1;
    int score=0;
};

bool V24LooksHeapPointer(uint32_t p) {
    if(p<0x10000u) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if(!VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(p)),&mbi,sizeof(mbi))) return false;
    if(mbi.State!=MEM_COMMIT || !IsReadableProtection(mbi.Protect)) return false;
    return mbi.Type==MEM_PRIVATE || mbi.Type==MEM_MAPPED;
}

bool V24SnapU32(const std::vector<uint8_t>& s,int off,uint32_t& v) {
    if(off<0 || off+4>(int)s.size()) return false;
    std::memcpy(&v,s.data()+off,4);
    return true;
}

uint32_t V24FindContainerPtrInNode(const Report& r,uint32_t node,int& ptrOff) {
    ptrOff=-1;
    if(!V24LooksHeapPointer(node)) return 0;
    for(int off=0;off<=0x80;off+=4) {
        uint32_t p=0;
        if(!SafeRead(static_cast<uintptr_t>(node)+off,p) || !p) continue;
        if(IsExactRttiObjectV19(r,p,"Container")) { ptrOff=off; return p; }
    }
    return 0;
}

std::set<int> V24DecodeLeaOffsetsForToken(const Report& r,const std::string& token) {
    std::set<int> out;
    auto it=r.binds.find(token);
    if(it==r.binds.end()) return out;
    for(const auto& c:it->second.candidates) {
        std::vector<uint8_t> b(80);
        if(!SafeCopy(r.module.base+c.methodRva,b.data(),b.size())) continue;
        for(size_t i=0;i+6<b.size();++i) {
            if(b[i]!=0x8D) continue;
            const uint8_t mr=b[i+1];
            const int mod=(mr>>6)&3, rm=mr&7;
            if(rm!=1 || mod==3) continue; // [ecx+disp]
            int off=0;
            if(mod==1) off=static_cast<int8_t>(b[i+2]);
            else if(mod==2) { int32_t d=0; std::memcpy(&d,b.data()+i+2,4); off=d; }
            else continue;
            if(off>=0 && off<=0x4000) out.insert(off);
        }
    }
    return out;
}

std::vector<V24OpenMapCandidate> V24FindOpenMapCandidates(const Report& r,
                                                           const std::vector<uint8_t>& closed,
                                                           const std::vector<uint8_t>& opened) {
    std::vector<V24OpenMapCandidate> out;
    const auto semanticMaps=V24DecodeLeaOffsetsForToken(r,"getContainers");
    const int n=static_cast<int>(std::min(closed.size(),opened.size()));
    for(int sizeOff=4;sizeOff+4<=n;sizeOff+=4) {
        uint32_t a=0,b=0;
        V24SnapU32(closed,sizeOff,a); V24SnapU32(opened,sizeOff,b);
        if(a!=0 || b==0 || b>16) continue;
        for(int back=4;back<=32;back+=4) {
            const int mapOff=sizeOff-back;
            if(mapOff<0) continue;
            uint32_t ha=0,hb=0;
            if(!V24SnapU32(closed,mapOff,ha) || !V24SnapU32(opened,mapOff,hb)) continue;
            if(!hb || ha!=hb || !V24LooksHeapPointer(hb)) continue;
            for(int childOff=0;childOff<=0x18;childOff+=4) {
                uint32_t node=0;
                if(!SafeRead(static_cast<uintptr_t>(hb)+childOff,node) || !node || node==hb || !V24LooksHeapPointer(node)) continue;
                int objOff=-1;
                const uint32_t obj=V24FindContainerPtrInNode(r,node,objOff);
                if(!obj) continue;
                V24OpenMapCandidate c;
                c.mapOffset=mapOff; c.sizeOffset=sizeOff; c.head=hb; c.node=node;
                c.container=obj; c.nodeContainerOffset=objOff;
                c.score=1200;
                if(b==1) c.score+=450;
                if(childOff<=8) c.score+=120;
                if(back==4) c.score+=180;
                if(semanticMaps.count(mapOff)) c.score+=900;
                if(objOff>=4) {
                    uint32_t k=0;
                    if(SafeRead(static_cast<uintptr_t>(node)+objOff-4,k) && k<=15) {
                        c.hasKey=true; c.key=k; c.keyOffset=objOff-4; c.score+=180;
                    }
                }
                bool merged=false;
                for(auto& e:out) {
                    if(e.mapOffset==c.mapOffset && e.sizeOffset==c.sizeOffset && e.container==c.container) {
                        if(c.score>e.score) e=c;
                        merged=true; break;
                    }
                }
                if(!merged) out.push_back(c);
            }
        }
    }
    std::sort(out.begin(),out.end(),[](const auto& x,const auto& y){
        if(x.score!=y.score) return x.score>y.score;
        if(x.sizeOffset!=y.sizeOffset) return x.sizeOffset<y.sizeOffset;
        return x.mapOffset<y.mapOffset;
    });
    return out;
}

std::set<uint32_t> V24ScanExactRttiObjects(const Report& r,const std::string& cls) {
    std::set<uint32_t> found;
    auto it=r.rtti.find(cls);
    if(it==r.rtti.end() || it->second.vtableRvas.empty()) return found;
    std::set<uint32_t> vts;
    for(uint32_t rv:it->second.vtableRvas) vts.insert(static_cast<uint32_t>(r.module.base+rv));

    uintptr_t p=0x10000;
    const uintptr_t end=0x7FFF0000u;
    const size_t chunkMax=1u<<20;
    std::vector<uint8_t> buf;
    while(p<end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if(!VirtualQuery(reinterpret_cast<LPCVOID>(p),&mbi,sizeof(mbi))) break;
        const uintptr_t base=reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t rend=base+mbi.RegionSize;
        const DWORD prot=mbi.Protect&0xFF;
        const bool writable = prot==PAGE_READWRITE || prot==PAGE_WRITECOPY ||
                              prot==PAGE_EXECUTE_READWRITE || prot==PAGE_EXECUTE_WRITECOPY;
        if(mbi.State==MEM_COMMIT && writable && !(mbi.Protect&PAGE_GUARD) &&
           (mbi.Type==MEM_PRIVATE || mbi.Type==MEM_MAPPED)) {
            for(uintptr_t q=base;q<rend;) {
                const size_t take=static_cast<size_t>(std::min<uintptr_t>(chunkMax,rend-q));
                buf.resize(take);
                if(SafeCopy(q,buf.data(),take)) {
                    for(size_t i=0;i+4<=take;i+=4) {
                        uint32_t x=0; std::memcpy(&x,buf.data()+i,4);
                        if(vts.count(x)) found.insert(static_cast<uint32_t>(q+i));
                    }
                }
                q+=take;
            }
        }
        if(rend<=p) break;
        p=rend;
    }
    return found;
}

struct V24ItemsCandidate {
    std::string kind;
    int offset=-1;
    int elemSize=0;
    int count=0;
    int score=0;
    bool semantic=false;
    std::vector<uint32_t> items;
};

bool V24ParseVectorItems(const Report& r,uint32_t obj,int off,int elem,std::vector<uint32_t>& items) {
    items.clear();
    uint32_t begin=0,end=0,cap=0;
    if(!SafeRead(static_cast<uintptr_t>(obj)+off,begin) ||
       !SafeRead(static_cast<uintptr_t>(obj)+off+4,end) ||
       !SafeRead(static_cast<uintptr_t>(obj)+off+8,cap)) return false;
    if(!begin || end<begin || cap<end || elem<4) return false;
    const uint32_t bytes=end-begin, capBytes=cap-begin;
    if(bytes%static_cast<uint32_t>(elem) || capBytes%static_cast<uint32_t>(elem)) return false;
    const int n=static_cast<int>(bytes/elem), cn=static_cast<int>(capBytes/elem);
    if(n<1 || n>60 || cn<n || cn>100 || !V24LooksHeapPointer(begin)) return false;
    for(int i=0;i<n;++i) {
        uint32_t p=0;
        if(!SafeRead(static_cast<uintptr_t>(begin)+i*elem,p) || !p || !IsExactRttiObjectV19(r,p,"Item")) return false;
        items.push_back(p);
    }
    return true;
}

bool V24ParseDequeItems(const Report& r,uint32_t obj,int off,int elem,std::vector<uint32_t>& items) {
    items.clear();
    uint32_t map=0,mapsize=0,myoff=0,mysize=0;
    if(!SafeRead(static_cast<uintptr_t>(obj)+off,map) ||
       !SafeRead(static_cast<uintptr_t>(obj)+off+4,mapsize) ||
       !SafeRead(static_cast<uintptr_t>(obj)+off+8,myoff) ||
       !SafeRead(static_cast<uintptr_t>(obj)+off+12,mysize)) return false;
    if(!map || !V24LooksHeapPointer(map) || mapsize==0 || mapsize>256 || mysize<1 || mysize>60) return false;
    const int block = elem<=1?16:(elem<=2?8:(elem<=4?4:(elem<=8?2:1)));
    for(uint32_t i=0;i<mysize;++i) {
        const uint32_t logical=myoff+i;
        uint32_t bi=logical/static_cast<uint32_t>(block);
        if((mapsize&(mapsize-1))==0) bi&=(mapsize-1); else bi%=mapsize;
        const uint32_t inner=logical%static_cast<uint32_t>(block);
        uint32_t blockPtr=0,item=0;
        if(!SafeRead(static_cast<uintptr_t>(map)+bi*4,blockPtr) || !blockPtr || !V24LooksHeapPointer(blockPtr)) return false;
        if(!SafeRead(static_cast<uintptr_t>(blockPtr)+inner*elem,item) || !item || !IsExactRttiObjectV19(r,item,"Item")) return false;
        items.push_back(item);
    }
    return true;
}

std::vector<V24ItemsCandidate> V24FindItemsCollections(const Report& r,uint32_t container) {
    std::vector<V24ItemsCandidate> out;
    const auto semantic=V24DecodeLeaOffsetsForToken(r,"getItems");
    for(int off=4;off<=0x300;off+=4) for(int elem:{4,8}) {
        for(const char* kind:{"vector","deque"}) {
            std::vector<uint32_t> items;
            const bool ok=std::string(kind)=="vector" ? V24ParseVectorItems(r,container,off,elem,items)
                                                       : V24ParseDequeItems(r,container,off,elem,items);
            if(!ok) continue;
            V24ItemsCandidate c; c.kind=kind; c.offset=off; c.elemSize=elem;
            c.count=static_cast<int>(items.size()); c.items=items;
            c.semantic=semantic.count(off)!=0;
            c.score=600+c.count*40+(elem==8?80:0)+(c.semantic?1000:0)+(c.kind=="deque"?30:0);
            bool dup=false;
            for(auto& e:out) {
                if(e.kind==c.kind && e.offset==c.offset && e.elemSize==c.elemSize) { if(c.score>e.score)e=c; dup=true; break; }
            }
            if(!dup) out.push_back(c);
        }
    }
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){
        if(a.score!=b.score) return a.score>b.score;
        if(a.offset!=b.offset) return a.offset<b.offset;
        return a.elemSize<b.elemSize;
    });
    return out;
}

bool V24Printable(const std::string& s) {
    if(s.size()<2 || s.size()>48) return false;
    for(unsigned char c:s) if(c<32 || c>126) return false;
    return true;
}

bool V24ReadMsvcString(uintptr_t addr,std::string& out) {
    out.clear();
    uint32_t len=0,cap=0;
    if(!SafeRead(addr+16,len) || !SafeRead(addr+20,cap) || len>64 || cap>4096 || len>cap) return false;
    char buf[65]{};
    if(cap<16) {
        if(len && !SafeCopy(addr,buf,len)) return false;
    } else {
        uint32_t p=0; if(!SafeRead(addr,p) || !p || !SafeCopy(p,buf,len)) return false;
    }
    out.assign(buf,buf+len);
    return V24Printable(out);
}

struct V24ScalarHit {
    std::string type;
    int offset=-1;
    uint32_t getter=0;
    uint32_t setter=0;
    uint32_t value=0;
    bool setterAgree=false;
    int score=0;
};

std::vector<V24ScalarHit> V24ContainerScalarHits(const Report& r,uint32_t obj,
                                                  const std::string& getTok,
                                                  const std::string& setTok,
                                                  uint32_t maxValue) {
    std::vector<V24ScalarHit> out;
    auto gi=r.binds.find(getTok); if(gi==r.binds.end()) return out;
    auto si=r.binds.find(setTok);
    for(const auto& gc:gi->second.candidates) for(const char* t:{"u8","u16","u32"}) {
        int off=-1; if(!DecodeDirectScalarGetterV18(r.module.base+gc.methodRva,t,off)) continue;
        double dv=0; if(!ReadScalarAtV19(obj,off,t,dv)) continue;
        const uint32_t v=static_cast<uint32_t>(dv);
        if(v>maxValue) continue;
        V24ScalarHit h; h.type=t; h.offset=off; h.getter=gc.methodRva; h.value=v;
        if(si!=r.binds.end()) for(const auto& sc:si->second.candidates) {
            if(MethodReferencesThisOffsetV18(r.module.base+sc.methodRva,off)) { h.setterAgree=true; h.setter=sc.methodRva; break; }
        }
        h.score=200+(h.setterAgree?600:0)+(t==std::string("u32")?20:0);
        bool merged=false;
        for(auto& e:out) if(e.offset==h.offset && e.type==h.type) { if(h.score>e.score)e=h; merged=true; break; }
        if(!merged) out.push_back(h);
    }
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){ if(a.score!=b.score)return a.score>b.score; return a.offset<b.offset; });
    return out;
}

void V24DiscoverContainerObject(Report& r,const V24OpenMapCandidate* mapInfo,uint32_t container) {
    if(!container || !IsExactRttiObjectV19(r,container,"Container")) {
        std::cout << "[CONTAINER OBJECT] nenhum Container RTTI valido para decodificar.\n"; return;
    }
    std::cout << "[CONTAINER OBJECT] ptr=" << HexU32(container) << " RTTI=Container\n";

    auto cols=V24FindItemsCollections(r,container);
    std::cout << "[CONTAINER ITEMS CANDIDATES] " << cols.size() << "\n";
    for(size_t i=0;i<cols.size() && i<10;++i) {
        const auto& c=cols[i];
        std::cout << "  " << c.kind << " +" << HexU32(c.offset)
                  << " elem=" << c.elemSize << " count=" << c.count
                  << " getItems=" << (c.semantic?"MATCH":"no") << " score=" << c.score << "\n";
    }
    int observedItems=0;
    if(!cols.empty()) {
        const auto& best=cols[0];
        const bool unique=cols.size()==1 || best.score>=cols[1].score+180 || best.semantic;
        if(unique) {
            r.selectedFields["containerItems"]={best.kind,best.offset,"runtime_transition_container_items_rtti_semantic"};
            r.selectedFields["containerItemStride"]={"bytes",best.elemSize,"runtime_transition_container_items_rtti_semantic"};
            observedItems=best.count;
            std::cout << "[CONTAINER ITEMS] CONFIRMADO " << best.kind << " +" << HexU32(best.offset)
                      << " elem=" << best.elemSize << " Item*=" << best.count << "\n";
            auto iid=r.selectedFields.find("itemId"), ic=r.selectedFields.find("itemCount");
            for(size_t i=0;i<best.items.size() && i<12;++i) {
                std::cout << "    slot " << i << " ptr=" << HexU32(best.items[i]);
                if(iid!=r.selectedFields.end() && ic!=r.selectedFields.end()) {
                    double id=0,ct=0;
                    if(ReadScalarAtV19(best.items[i],iid->second.offset,iid->second.type,id) &&
                       ReadScalarAtV19(best.items[i],ic->second.offset,ic->second.type,ct))
                        std::cout << " id=" << static_cast<uint32_t>(id) << " count=" << static_cast<uint32_t>(ct);
                }
                std::cout << "\n";
            }
            SaveCheckpoint(r,"container_items_rtti_semantic");
        }
    }

    // Container name: current object scan, optionally strengthened by getName LEA.
    const auto nameSemantic=V24DecodeLeaOffsetsForToken(r,"getName");
    struct NameHit{int off;std::string s;int score;}; std::vector<NameHit> names;
    for(int off=4;off<=0x200;off+=4) {
        std::string s; if(!V24ReadMsvcString(static_cast<uintptr_t>(container)+off,s)) continue;
        int score=300+(nameSemantic.count(off)?900:0);
        names.push_back({off,s,score});
    }
    std::sort(names.begin(),names.end(),[](const auto&a,const auto&b){if(a.score!=b.score)return a.score>b.score;return a.off<b.off;});
    std::cout << "[CONTAINER NAME CANDIDATES] " << names.size() << "\n";
    for(size_t i=0;i<names.size() && i<8;++i) std::cout << "  msvc_string +" << HexU32(names[i].off) << " = \"" << names[i].s << "\" score=" << names[i].score << "\n";
    if(!names.empty() && (names.size()==1 || names[0].score>=names[1].score+300)) {
        r.selectedFields["containerName"]={"msvc_string",names[0].off,"runtime_transition_container_name_string_semantic"};
        std::cout << "[CONTAINER NAME] CONFIRMADO +" << HexU32(names[0].off) << " = \"" << names[0].s << "\"\n";
        SaveCheckpoint(r,"container_name");
    }

    // Capacity: getter/setter agreement when available, runtime plausibility otherwise.
    auto cap=V24ContainerScalarHits(r,container,"getCapacity","setCapacity",1000);
    for(auto& h:cap) {
        if(h.value>=static_cast<uint32_t>(observedItems) && h.value>0 && h.value<=200) h.score+=350;
        else h.score-=250;
    }
    std::sort(cap.begin(),cap.end(),[](const auto&a,const auto&b){if(a.score!=b.score)return a.score>b.score;return a.offset<b.offset;});
    std::cout << "[CONTAINER CAPACITY CANDIDATES] " << cap.size() << "\n";
    for(size_t i=0;i<cap.size() && i<8;++i) std::cout << "  " << cap[i].type << " +" << HexU32(cap[i].offset) << " value=" << cap[i].value << " getter=" << HexU32(cap[i].getter) << " setter=" << (cap[i].setterAgree?HexU32(cap[i].setter):std::string("<no-match>")) << " score=" << cap[i].score << "\n";
    if(!cap.empty() && cap[0].value>0 && cap[0].value<=200 && cap[0].value>=static_cast<uint32_t>(observedItems) &&
       (cap.size()==1 || cap[0].score>=cap[1].score+180)) {
        r.selectedFields["containerCapacity"]={cap[0].type,cap[0].offset,"semantic_current_build_getter_setter_container_capacity"};
        std::cout << "[CONTAINER CAPACITY] CONFIRMADO " << cap[0].type << " +" << HexU32(cap[0].offset) << " value=" << cap[0].value << "\n";
        SaveCheckpoint(r,"container_capacity");
    }

    // ID: require agreement with the map key when the transition exposed one.
    auto ids=V24ContainerScalarHits(r,container,"getId","setId",15);
    if(mapInfo && mapInfo->hasKey) {
        for(auto& h:ids) h.score += (h.value==mapInfo->key ? 1100 : -900);
        std::sort(ids.begin(),ids.end(),[](const auto&a,const auto&b){if(a.score!=b.score)return a.score>b.score;return a.offset<b.offset;});
    }
    std::cout << "[CONTAINER ID CANDIDATES] " << ids.size();
    if(mapInfo && mapInfo->hasKey) std::cout << " mapKey=" << mapInfo->key;
    std::cout << "\n";
    for(size_t i=0;i<ids.size() && i<10;++i) std::cout << "  " << ids[i].type << " +" << HexU32(ids[i].offset) << " value=" << ids[i].value << " getter=" << HexU32(ids[i].getter) << " setter=" << (ids[i].setterAgree?HexU32(ids[i].setter):std::string("<no-match>")) << " score=" << ids[i].score << "\n";
    if(!ids.empty() && mapInfo && mapInfo->hasKey && ids[0].value==mapInfo->key &&
       (ids.size()==1 || ids[0].score>=ids[1].score+300)) {
        r.selectedFields["containerId"]={ids[0].type,ids[0].offset,"runtime_transition_container_id_mapkey_semantic"};
        std::cout << "[CONTAINER ID] CONFIRMADO " << ids[0].type << " +" << HexU32(ids[0].offset) << " value=" << ids[0].value << "\n";
        SaveCheckpoint(r,"container_id_mapkey");
    }
}

uint32_t V24SampleContainerFromSavedMap(const Report& r,int mapOff) {
    const uintptr_t game=r.module.base+r.gameOwnerRva;
    uint32_t head=0; if(!SafeRead(game+mapOff,head) || !head || !V24LooksHeapPointer(head)) return 0;
    for(int childOff=0;childOff<=0x18;childOff+=4) {
        uint32_t node=0; if(!SafeRead(static_cast<uintptr_t>(head)+childOff,node) || !node || node==head) continue;
        int po=-1; uint32_t c=V24FindContainerPtrInNode(r,node,po); if(c) return c;
    }
    return 0;
}

void RunOpenContainerDiscoveryV24(Report& r) {
    std::cout << "\n============================================================\n"
              << "        OPEN CONTAINERS / CONTAINER OBJECT V25\n"
              << "============================================================\n"
              << "[METHOD] CLOSED -> OPEN -> CLOSED transition + RTTI Container + current-build semantics.\n"
              << "[MODE] READ-ONLY: nenhuma escrita, hook ou chamada interna.\n";
    if(!r.gameOwnerRva || !r.localPlayerPtr) { std::cout << "[CONTAINERS] Game/LocalPlayer indisponivel.\n"; return; }
    const uintptr_t game=r.module.base+r.gameOwnerRva;

    auto om=r.selectedFields.find("openContainersMap");
    auto os=r.selectedFields.find("openContainersSize");
    if(om!=r.selectedFields.end() && os!=r.selectedFields.end() && om->second.offset>=0 && os->second.offset>=0 &&
       EvidenceScore(om->second.status)>=80 && EvidenceScore(os->second.status)>=80) {
        uint32_t n=0; SafeRead(game+os->second.offset,n);
        std::cout << "[OPEN CONTAINERS QUICK-CHECK] Game +" << HexU32(om->second.offset)
                  << " sizeField +" << HexU32(os->second.offset) << " open=" << n << " [profile]\n";
        uint32_t sample=V24SampleContainerFromSavedMap(r,om->second.offset);
        if(!sample && AskYesNo("Abra UM container com pelo menos 1 item para validar os campos Container agora? [S/N]")=='s') {
            WaitEnter("Abra exatamente UM container/backpack com item e ENTER... ");
            sample=V24SampleContainerFromSavedMap(r,om->second.offset);
        }
        if(sample) V24DiscoverContainerObject(r,nullptr,sample);
        return;
    }

    if(AskYesNo("Mapear a colecao de containers abertos com FECHADO -> ABERTO -> FECHADO agora? [S/N]")!='s') {
        std::cout << "[CONTAINERS] teste pulado.\n"; return;
    }
    const size_t win=0x4000;
    std::vector<uint8_t> a,b,c;
    WaitEnter("Feche TODOS os containers e nao altere mais nada. ENTER... ");
    if(!CaptureWindow(game,a,win)) { std::cout << "[CONTAINERS] falha snapshot fechado.\n"; return; }
    const auto closedObjects=V24ScanExactRttiObjects(r,"Container");

    WaitEnter("Abra EXATAMENTE UM container/backpack que tenha pelo menos 1 item. ENTER... ");
    if(!CaptureWindow(game,b,win)) { std::cout << "[CONTAINERS] falha snapshot aberto.\n"; return; }
    auto cand=V24FindOpenMapCandidates(r,a,b);
    std::cout << "[OPEN MAP CANDIDATES A->B] " << cand.size() << "\n";
    for(size_t i=0;i<cand.size() && i<10;++i) {
        std::cout << "  Game +" << HexU32(cand[i].mapOffset) << " size +" << HexU32(cand[i].sizeOffset)
                  << " head=" << HexU32(cand[i].head) << " node=" << HexU32(cand[i].node)
                  << " Container*=" << HexU32(cand[i].container)
                  << " nodeObjOff=+" << HexU32(cand[i].nodeContainerOffset);
        if(cand[i].hasKey) std::cout << " key=" << cand[i].key << " keyOff=+" << HexU32(cand[i].keyOffset);
        std::cout << " score=" << cand[i].score << "\n";
    }

    V24OpenMapCandidate provisional;
    bool haveProvisional=!cand.empty();
    if(haveProvisional) provisional=cand[0];
    uint32_t sample=haveProvisional?provisional.container:0;

    if(!sample) {
        const auto openedObjects=V24ScanExactRttiObjects(r,"Container");
        std::vector<uint32_t> delta;
        for(uint32_t p:openedObjects) if(!closedObjects.count(p)) delta.push_back(p);
        std::cout << "[RTTI CONTAINER HEAP DELTA] new-on-open=" << delta.size() << "\n";
        for(size_t i=0;i<delta.size() && i<12;++i) std::cout << "  " << HexU32(delta[i]) << "\n";
        if(delta.size()==1) sample=delta[0];
    }

    if(sample) V24DiscoverContainerObject(r,haveProvisional?&provisional:nullptr,sample);

    WaitEnter("Feche o MESMO container e deixe todos fechados novamente. ENTER... ");
    if(!CaptureWindow(game,c,win)) { std::cout << "[CONTAINERS] falha snapshot final.\n"; return; }
    for(auto& x:cand) {
        uint32_t av=0,bv=0,cv=0;
        V24SnapU32(a,x.sizeOffset,av); V24SnapU32(b,x.sizeOffset,bv); V24SnapU32(c,x.sizeOffset,cv);
        if(av==0 && bv>0 && bv<=16 && cv==av) x.score+=1200;
        else x.score-=1000;
    }
    std::sort(cand.begin(),cand.end(),[](const auto& x,const auto& y){if(x.score!=y.score)return x.score>y.score;return x.sizeOffset<y.sizeOffset;});
    std::cout << "[OPEN MAP A->B->A CONFIRMATION] " << cand.size() << "\n";
    for(size_t i=0;i<cand.size() && i<8;++i) {
        uint32_t av=0,bv=0,cv=0; V24SnapU32(a,cand[i].sizeOffset,av); V24SnapU32(b,cand[i].sizeOffset,bv); V24SnapU32(c,cand[i].sizeOffset,cv);
        std::cout << "  Game +" << HexU32(cand[i].mapOffset) << " size +" << HexU32(cand[i].sizeOffset)
                  << " " << av << " -> " << bv << " -> " << cv << " score=" << cand[i].score << "\n";
    }
    if(!cand.empty()) {
        const bool unique=cand.size()==1 || cand[0].score>=cand[1].score+300;
        uint32_t av=0,bv=0,cv=0; V24SnapU32(a,cand[0].sizeOffset,av); V24SnapU32(b,cand[0].sizeOffset,bv); V24SnapU32(c,cand[0].sizeOffset,cv);
        if(unique && av==0 && bv>0 && bv<=16 && cv==0) {
            const std::string st="runtime_transition_container_open_close_msvc_map_rtti";
            r.selectedFields["openContainersMap"]={"msvc_map",cand[0].mapOffset,st};
            r.selectedFields["openContainersSize"]={"u32",cand[0].sizeOffset,st};
            r.selectedFields["openContainersNodeObject"]={"node_offset",cand[0].nodeContainerOffset,st};
            if(cand[0].hasKey) r.selectedFields["openContainersNodeKey"]={"node_offset",cand[0].keyOffset,"runtime_transition_container_map_key_candidate"};
            std::cout << "[OPEN CONTAINERS MAP] CONFIRMADO Game +" << HexU32(cand[0].mapOffset)
                      << " sizeField +" << HexU32(cand[0].sizeOffset)
                      << " Container node offset +" << HexU32(cand[0].nodeContainerOffset) << "\n";
            SaveCheckpoint(r,"open_containers_map_transition");
        } else {
            std::cout << "[OPEN CONTAINERS MAP] ainda ambiguo; nenhum Game offset promovido.\n";
        }
    }
}


// ============================================================
// V24 - MULTI-TRANSITION OPEN CONTAINER DISCOVERY (x86)
// READ-ONLY. Uses CLOSED -> OPEN -> CLOSED -> OPEN -> CLOSED.
// The same container/backpack must be reopened. This lets the probe intersect
// two independent RTTI Container observations and validate a Game registry
// path without assuming a particular STL layout.
// ============================================================
struct V24XNameHit {
    int offset=-1;
    std::string value;
    int score=0;
    bool semantic=false;
};

struct V24XContainerObs {
    uint32_t ptr=0;
    bool delta=false;
    int score=0;
    bool hasItems=false;
    V24ItemsCandidate bestItems;
    std::vector<std::pair<uint32_t,uint32_t>> itemSignature;
    std::vector<V24ScalarHit> capacities;
    std::vector<V24ScalarHit> ids;
    std::vector<V24XNameHit> names;
};

bool V24XModrmEcxDisp(const std::vector<uint8_t>& b,size_t mrPos,int& out) {
    out=-1;
    if(mrPos>=b.size()) return false;
    const uint8_t mr=b[mrPos];
    const int mod=(mr>>6)&3, rm=mr&7;
    if(rm!=1 || mod==3) return false; // ECX base only
    if(mod==0) { out=0; return true; }
    if(mod==1) {
        if(mrPos+1>=b.size()) return false;
        out=static_cast<int8_t>(b[mrPos+1]);
        return true;
    }
    if(mod==2) {
        if(mrPos+4>=b.size()) return false;
        int32_t d=0; std::memcpy(&d,b.data()+mrPos+1,4); out=d;
        return true;
    }
    return false;
}

std::set<int> V24XDecodeThisRefsAt(uintptr_t fn) {
    std::set<int> out;
    std::vector<uint8_t> b(192);
    if(!SafeCopy(fn,b.data(),b.size())) return out;
    const auto add=[&](size_t mrPos){
        int d=-1;
        if(V24XModrmEcxDisp(b,mrPos,d) && d>=0 && d<=0x4000) out.insert(d);
    };
    for(size_t i=0;i+6<b.size();++i) {
        const uint8_t op=b[i];
        if(op==0x8B || op==0x89 || op==0x8D || op==0x39 || op==0x3B ||
           op==0x83 || op==0x81 || op==0x80 || op==0xC7 || op==0xFF) add(i+1);
        if(op==0x66 && i+2<b.size()) {
            const uint8_t op2=b[i+1];
            if(op2==0x8B || op2==0x89 || op2==0x8D || op2==0x39 || op2==0x3B) add(i+2);
        }
        if(op==0x0F && i+2<b.size()) {
            const uint8_t op2=b[i+1];
            if(op2==0xB6 || op2==0xB7 || op2==0xBE || op2==0xBF) add(i+2);
        }
        if((op==0xF2 || op==0xF3) && i+3<b.size() && b[i+1]==0x0F &&
           (b[i+2]==0x10 || b[i+2]==0x11)) add(i+3);
    }
    return out;
}

std::set<int> V24XThisRefsForToken(const Report& r,const std::string& token) {
    std::set<int> out;
    auto it=r.binds.find(token);
    if(it==r.binds.end()) return out;
    for(const auto& c:it->second.candidates) {
        auto s=V24XDecodeThisRefsAt(r.module.base+c.methodRva);
        out.insert(s.begin(),s.end());
    }
    return out;
}

void V24XPrintRefs(const char* label,const std::set<int>& refs) {
    std::cout << label << " refs=" << refs.size();
    size_t n=0;
    for(int x:refs) {
        if(n++>=24) { std::cout << " ..."; break; }
        std::cout << " +" << HexU32(static_cast<uint32_t>(x));
    }
    std::cout << "\n";
}

uint32_t V24XReadWidth(const std::vector<uint8_t>& s,int off,int width) {
    if(off<0 || off+width>(int)s.size()) return 0xFFFFFFFFu;
    uint32_t v=0; std::memcpy(&v,s.data()+off,width); return v;
}

struct V24XToggleHit {
    int offset=-1;
    int width=0;
    uint32_t a=0,b=0,c=0,d=0,e=0;
    int score=0;
};

std::vector<V24XToggleHit> V24XFindFiveStateToggles(const std::vector<uint8_t>& a,
                                                    const std::vector<uint8_t>& b,
                                                    const std::vector<uint8_t>& c,
                                                    const std::vector<uint8_t>& d,
                                                    const std::vector<uint8_t>& e,
                                                    const std::set<int>& semanticRefs) {
    std::vector<V24XToggleHit> out;
    const int n=static_cast<int>(std::min({a.size(),b.size(),c.size(),d.size(),e.size()}));
    for(int width:{1,2,4}) {
        const int step=width;
        for(int off=0;off+width<=n;off+=step) {
            const uint32_t av=V24XReadWidth(a,off,width), bv=V24XReadWidth(b,off,width),
                           cv=V24XReadWidth(c,off,width), dv=V24XReadWidth(d,off,width),
                           ev=V24XReadWidth(e,off,width);
            if(av!=cv || cv!=ev || bv!=dv || av==bv) continue;
            if(av>64 || bv>64) continue;
            V24XToggleHit h; h.offset=off; h.width=width; h.a=av;h.b=bv;h.c=cv;h.d=dv;h.e=ev;
            h.score=300;
            if(av==0) h.score+=350;
            if(bv==1) h.score+=950;
            else if(bv>0 && bv<=16) h.score+=450;
            if(width==4) h.score+=120;
            for(int r:semanticRefs) {
                const int dist=std::abs(r-off);
                if(dist==0) h.score+=900;
                else if(dist<=8) h.score+=650;
                else if(dist<=0x20) h.score+=350;
                else if(dist<=0x60) h.score+=150;
            }
            out.push_back(h);
        }
    }
    std::sort(out.begin(),out.end(),[](const auto& x,const auto& y){
        if(x.score!=y.score) return x.score>y.score;
        if(x.width!=y.width) return x.width>y.width;
        return x.offset<y.offset;
    });
    return out;
}

std::vector<std::pair<uint32_t,uint32_t>> V24XItemSignature(const Report& r,const V24ItemsCandidate& items) {
    std::vector<std::pair<uint32_t,uint32_t>> out;
    auto iid=r.selectedFields.find("itemId"), ic=r.selectedFields.find("itemCount");
    if(iid==r.selectedFields.end() || ic==r.selectedFields.end()) return out;
    for(uint32_t p:items.items) {
        double id=0,ct=0;
        if(ReadScalarAtV19(p,iid->second.offset,iid->second.type,id) &&
           ReadScalarAtV19(p,ic->second.offset,ic->second.type,ct))
            out.emplace_back(static_cast<uint32_t>(id),static_cast<uint32_t>(ct));
    }
    return out;
}

V24XContainerObs V24XBuildContainerObs(const Report& r,uint32_t p,bool delta) {
    V24XContainerObs o; o.ptr=p; o.delta=delta;
    if(!p || !IsExactRttiObjectV19(r,p,"Container")) return o;
    o.score=delta?500:50;
    auto cols=V24FindItemsCollections(r,p);
    if(!cols.empty()) {
        o.hasItems=true; o.bestItems=cols.front();
        o.score+=1600+o.bestItems.score;
        if(o.bestItems.semantic) o.score+=600;
        o.itemSignature=V24XItemSignature(r,o.bestItems);
        if(!o.itemSignature.empty()) o.score+=300;
    }
    o.capacities=V24ContainerScalarHits(r,p,"getCapacity","setCapacity",1000);
    for(auto& h:o.capacities) {
        if(h.setterAgree) o.score+=80;
        if(h.value>0 && h.value<=200) o.score+=60;
    }
    o.ids=V24ContainerScalarHits(r,p,"getId","setId",15);

    const auto nameSemantic=V24DecodeLeaOffsetsForToken(r,"getName");
    for(int off=4;off<=0x200;off+=4) {
        std::string s;
        if(!V24ReadMsvcString(static_cast<uintptr_t>(p)+off,s)) continue;
        V24XNameHit h; h.offset=off; h.value=s; h.semantic=nameSemantic.count(off)!=0;
        h.score=250+(h.semantic?900:0);
        o.names.push_back(h);
    }
    std::sort(o.names.begin(),o.names.end(),[](const auto& x,const auto& y){
        if(x.score!=y.score) return x.score>y.score; return x.offset<y.offset;
    });
    return o;
}

std::vector<V24XContainerObs> V24XOpenObservations(const Report& r,
                                                    const std::set<uint32_t>& closed,
                                                    const std::set<uint32_t>& opened) {
    std::vector<V24XContainerObs> out;
    std::vector<uint32_t> delta;
    for(uint32_t p:opened) if(!closed.count(p)) delta.push_back(p);
    const bool haveDelta=!delta.empty();
    if(haveDelta) {
        for(uint32_t p:delta) out.push_back(V24XBuildContainerObs(r,p,true));
    } else {
        size_t n=0;
        for(uint32_t p:opened) {
            if(n++>=64) break;
            out.push_back(V24XBuildContainerObs(r,p,false));
        }
    }
    std::sort(out.begin(),out.end(),[](const auto& x,const auto& y){
        if(x.score!=y.score) return x.score>y.score; return x.ptr<y.ptr;
    });
    return out;
}

void V24XPrintObs(const char* tag,const std::vector<V24XContainerObs>& obs) {
    std::cout << tag << " candidates=" << obs.size() << "\n";
    for(size_t i=0;i<obs.size() && i<12;++i) {
        const auto& o=obs[i];
        std::cout << "  ptr=" << HexU32(o.ptr) << " delta=" << (o.delta?"YES":"no")
                  << " score=" << o.score;
        if(o.hasItems) {
            std::cout << " items=" << o.bestItems.kind << "+" << HexU32(o.bestItems.offset)
                      << " elem=" << o.bestItems.elemSize << " count=" << o.bestItems.count
                      << " getItems=" << (o.bestItems.semantic?"MATCH":"no");
        } else std::cout << " items=<none>";
        if(!o.names.empty()) std::cout << " name=\"" << o.names[0].value << "\"@+" << HexU32(o.names[0].offset);
        std::cout << "\n";
    }
}

struct V24XReachResult {
    bool found=false;
    std::vector<int> path;
};

V24XReachResult V24XReachTargetFromAddress(uintptr_t rootAddr,uint32_t target) {
    V24XReachResult rr;
    struct Node { uint32_t ptr; int depth; std::vector<int> path; };
    std::vector<Node> q;
    std::set<uint32_t> seen;
    auto scanAddr=[&](uintptr_t addr,int bytes,int depth,const std::vector<int>& prefix,bool enqueue){
        for(int off=0;off<=bytes-4;off+=4) {
            uint32_t v=0; if(!SafeRead(addr+off,v) || !v) continue;
            auto p=prefix; p.push_back(off);
            if(v==target) { rr.found=true; rr.path=p; return true; }
            if(enqueue && depth<4 && V24LooksHeapPointer(v) && !seen.count(v) && q.size()<600) {
                seen.insert(v); q.push_back({v,depth+1,p});
            }
        }
        return false;
    };
    if(scanAddr(rootAddr,0x60,0,{},true)) return rr;
    for(size_t qi=0;qi<q.size() && qi<600 && !rr.found;++qi) {
        const auto cur=q[qi];
        if(scanAddr(static_cast<uintptr_t>(cur.ptr),0xA0,cur.depth,cur.path,true)) return rr;
    }
    return rr;
}

std::string V24XPathText(const std::vector<int>& p) {
    std::ostringstream os;
    for(size_t i=0;i<p.size();++i) {
        if(i) os << " -> ";
        os << "+" << HexU32(static_cast<uint32_t>(p[i]));
    }
    return os.str();
}

struct V24XRegistryLink {
    int rootOffset=-1;
    size_t obsIndex=0;
    V24XReachResult reach;
    int score=0;
    bool exactGetContainers=false;
    bool exactGetContainer=false;
};

std::set<int> V24XRootCandidates(const std::set<int>& a,const std::set<int>& b) {
    std::set<int> roots;
    const auto addAround=[&](int x){
        for(int back=0;back<=0x20;back+=4) if(x-back>=0) roots.insert(x-back);
    };
    for(int x:a) addAround(x);
    for(int x:b) addAround(x);
    return roots;
}

std::vector<V24XRegistryLink> V24XFindRegistryLinks(const Report& r,uintptr_t game,
                                                     const std::set<int>& getContainersRefs,
                                                     const std::set<int>& getContainerRefs,
                                                     const std::vector<V24XContainerObs>& obs) {
    std::vector<V24XRegistryLink> out;
    const auto roots=V24XRootCandidates(getContainersRefs,getContainerRefs);
    for(int root:roots) {
        for(size_t oi=0;oi<obs.size() && oi<8;++oi) {
            if(!obs[oi].ptr) continue;
            auto reach=V24XReachTargetFromAddress(game+root,obs[oi].ptr);
            if(!reach.found) continue;
            V24XRegistryLink l; l.rootOffset=root; l.obsIndex=oi; l.reach=reach;
            l.exactGetContainers=getContainersRefs.count(root)!=0;
            l.exactGetContainer=getContainerRefs.count(root)!=0;
            l.score=1200+obs[oi].score;
            if(l.exactGetContainers) l.score+=1400;
            if(l.exactGetContainer) l.score+=700;
            l.score+=std::max(0,500-static_cast<int>(reach.path.size())*90);
            out.push_back(l);
        }
    }
    std::sort(out.begin(),out.end(),[](const auto& x,const auto& y){
        if(x.score!=y.score) return x.score>y.score;
        if(x.rootOffset!=y.rootOffset) return x.rootOffset<y.rootOffset;
        return x.obsIndex<y.obsIndex;
    });
    return out;
}

void V24XPrintLinks(const char* tag,const std::vector<V24XRegistryLink>& links,
                    const std::vector<V24XContainerObs>& obs) {
    std::cout << tag << " links=" << links.size() << "\n";
    for(size_t i=0;i<links.size() && i<12;++i) {
        const auto& l=links[i];
        std::cout << "  Game +" << HexU32(l.rootOffset)
                  << " -> Container " << HexU32(obs[l.obsIndex].ptr)
                  << " path=" << V24XPathText(l.reach.path)
                  << " getContainers=" << (l.exactGetContainers?"EXACT":"near")
                  << " getContainer=" << (l.exactGetContainer?"EXACT":"near")
                  << " score=" << l.score << "\n";
    }
}

struct V24XObsPair { size_t a=0,b=0; int score=0; };

std::vector<V24XObsPair> V24XPairObservations(const std::vector<V24XContainerObs>& x,
                                               const std::vector<V24XContainerObs>& y) {
    std::vector<V24XObsPair> out;
    for(size_t i=0;i<x.size() && i<12;++i) for(size_t j=0;j<y.size() && j<12;++j) {
        int s=0;
        if(x[i].hasItems && y[j].hasItems && x[i].bestItems.kind==y[j].bestItems.kind &&
           x[i].bestItems.offset==y[j].bestItems.offset && x[i].bestItems.elemSize==y[j].bestItems.elemSize) s+=2600;
        else continue;
        if(!x[i].itemSignature.empty() && x[i].itemSignature==y[j].itemSignature) s+=1400;
        if(x[i].bestItems.count==y[j].bestItems.count) s+=300;
        for(const auto& nx:x[i].names) for(const auto& ny:y[j].names)
            if(nx.offset==ny.offset && nx.value==ny.value) { s+=nx.semantic||ny.semantic?800:250; goto names_done; }
        names_done:;
        s+=(x[i].delta?200:0)+(y[j].delta?200:0);
        out.push_back({i,j,s});
    }
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){return a.score>b.score;});
    return out;
}

void V24XPromoteContainerCrossOpen(Report& r,const V24XContainerObs& a,const V24XContainerObs& b) {
    if(a.hasItems && b.hasItems && a.bestItems.kind==b.bestItems.kind &&
       a.bestItems.offset==b.bestItems.offset && a.bestItems.elemSize==b.bestItems.elemSize) {
        r.selectedFields["containerItems"]={a.bestItems.kind,a.bestItems.offset,"runtime_same_container_reopen_items_rtti_semantic"};
        r.selectedFields["containerItemStride"]={"bytes",a.bestItems.elemSize,"runtime_same_container_reopen_items_rtti_semantic"};
        std::cout << "[CONTAINER ITEMS] CONFIRMADO em duas aberturas: " << a.bestItems.kind
                  << " +" << HexU32(a.bestItems.offset) << " elem=" << a.bestItems.elemSize
                  << " counts=" << a.bestItems.count << "/" << b.bestItems.count << "\n";
        SaveCheckpoint(r,"container_items_reopen_crosscheck");
    }

    struct StableScalar { V24ScalarHit h; int score=0; };
    std::vector<StableScalar> caps;
    for(const auto& x:a.capacities) for(const auto& y:b.capacities) {
        if(x.offset!=y.offset || x.type!=y.type || x.value!=y.value) continue;
        if(!x.setterAgree || !y.setterAgree || x.value==0 || x.value>200) continue;
        if(a.hasItems && x.value<static_cast<uint32_t>(a.bestItems.count)) continue;
        if(b.hasItems && x.value<static_cast<uint32_t>(b.bestItems.count)) continue;
        caps.push_back({x,1400+x.score+y.score});
    }
    std::sort(caps.begin(),caps.end(),[](const auto& x,const auto& y){return x.score>y.score;});
    std::cout << "[CONTAINER CAPACITY REOPEN XCHECK] " << caps.size() << "\n";
    for(size_t i=0;i<caps.size() && i<8;++i)
        std::cout << "  " << caps[i].h.type << " +" << HexU32(caps[i].h.offset)
                  << " value=" << caps[i].h.value << " score=" << caps[i].score << "\n";
    if(!caps.empty() && (caps.size()==1 || caps[0].score>=caps[1].score+250)) {
        r.selectedFields["containerCapacity"]={caps[0].h.type,caps[0].h.offset,"runtime_same_container_reopen_capacity_getset"};
        std::cout << "[CONTAINER CAPACITY] CONFIRMADO +" << HexU32(caps[0].h.offset)
                  << " value=" << caps[0].h.value << "\n";
        SaveCheckpoint(r,"container_capacity_reopen");
    }

    std::vector<V24XNameHit> names;
    for(const auto& x:a.names) for(const auto& y:b.names) {
        if(x.offset==y.offset && x.value==y.value && !x.value.empty()) {
            V24XNameHit h=x; h.score=x.score+y.score+(x.semantic||y.semantic?1000:400); names.push_back(h);
        }
    }
    std::sort(names.begin(),names.end(),[](const auto& x,const auto& y){return x.score>y.score;});
    std::cout << "[CONTAINER NAME REOPEN XCHECK] " << names.size() << "\n";
    for(size_t i=0;i<names.size() && i<8;++i)
        std::cout << "  msvc_string +" << HexU32(names[i].offset) << " = \"" << names[i].value
                  << "\" semantic=" << (names[i].semantic?"YES":"no") << " score=" << names[i].score << "\n";
    if(!names.empty() && (names.size()==1 || names[0].score>=names[1].score+350)) {
        r.selectedFields["containerName"]={"msvc_string",names[0].offset,"runtime_same_container_reopen_name_semantic"};
        std::cout << "[CONTAINER NAME] CONFIRMADO +" << HexU32(names[0].offset)
                  << " = \"" << names[0].value << "\"\n";
        SaveCheckpoint(r,"container_name_reopen");
    }

    // Container ID is intentionally conservative: two reopen observations must
    // agree on a getId/setId-backed field and leave a unique candidate.
    std::vector<StableScalar> ids;
    for(const auto& x:a.ids) for(const auto& y:b.ids) {
        if(x.offset!=y.offset || x.type!=y.type || x.value!=y.value) continue;
        if(!x.setterAgree || !y.setterAgree || x.value>15) continue;
        ids.push_back({x,1200+x.score+y.score});
    }
    std::sort(ids.begin(),ids.end(),[](const auto& x,const auto& y){return x.score>y.score;});
    std::cout << "[CONTAINER ID REOPEN XCHECK] " << ids.size() << "\n";
    for(size_t i=0;i<ids.size() && i<8;++i)
        std::cout << "  " << ids[i].h.type << " +" << HexU32(ids[i].h.offset)
                  << " value=" << ids[i].h.value << " score=" << ids[i].score << "\n";
    if(!ids.empty() && (ids.size()==1 || ids[0].score>=ids[1].score+500)) {
        r.selectedFields["containerId"]={ids[0].h.type,ids[0].h.offset,"runtime_same_container_reopen_id_getset"};
        std::cout << "[CONTAINER ID] CONFIRMADO +" << HexU32(ids[0].h.offset)
                  << " value=" << ids[0].h.value << "\n";
        SaveCheckpoint(r,"container_id_reopen");
    }
}


// ============================================================
// V24 - STRICT OPEN-CONTAINER MANAGER ROOT VALIDATION
// The V23 graph walk could start at a reserved Game field (for example
// Game+0x0 = LocalPlayer) and accidentally reach a Container through an
// unrelated object graph. V24 forbids the already-confirmed Game fields and
// requires a heap object referenced by Game to show BOTH:
//   (1) the same reachability path to the reopened semantic Container, and
//   (2) a local CLOSED->OPEN->CLOSED->OPEN->CLOSED scalar transition.
// ============================================================
struct V24ManagerState {
    uint32_t ptr=0;
    std::vector<uint8_t> bytes;
};
using V24ManagerMap = std::map<int,V24ManagerState>;

bool V24ReservedGameField(int off) {
    return off==0x0 || off==0x4 || off==0x8 || off==0xC;
}

V24ManagerMap V24CaptureManagerRoots(uintptr_t game,int maxOff=0x1000,int bytes=0x100) {
    V24ManagerMap out;
    for(int off=0x10; off<=maxOff; off+=4) {
        uint32_t p=0;
        if(!SafeRead(game+off,p) || !p || !V24LooksHeapPointer(p)) continue;
        V24ManagerState st; st.ptr=p; st.bytes.resize(bytes);
        if(!SafeCopy(static_cast<uintptr_t>(p),st.bytes.data(),st.bytes.size())) continue;
        out.emplace(off,std::move(st));
    }
    return out;
}

struct V24ManagerCandidate {
    int gameOffset=-1;
    uint32_t ptrA=0,ptrB=0,ptrC=0,ptrD=0,ptrE=0;
    V24XReachResult reachB,reachD;
    V24XToggleHit toggle;
    bool hasToggle=false;
    bool ptrStable=false;
    bool semanticNear=false;
    int score=0;
};

std::vector<V24ManagerCandidate> V24FindStrictManagerCandidates(
        const V24ManagerMap& a,const V24ManagerMap& b,const V24ManagerMap& c,
        const V24ManagerMap& d,const V24ManagerMap& e,
        uint32_t targetB,uint32_t targetD,const std::set<int>& semanticRefs) {
    std::vector<V24ManagerCandidate> out;
    if(!targetB || !targetD) return out;
    for(const auto& kv:b) {
        const int off=kv.first;
        if(V24ReservedGameField(off)) continue;
        auto ia=a.find(off), ic=c.find(off), id=d.find(off), ie=e.find(off);
        if(ia==a.end() || ic==c.end() || id==d.end() || ie==e.end()) continue;
        const auto& sb=kv.second; const auto& sa=ia->second; const auto& sc=ic->second;
        const auto& sd=id->second; const auto& se=ie->second;
        if(!sa.ptr || !sb.ptr || !sc.ptr || !sd.ptr || !se.ptr) continue;

        auto rb=V24XReachTargetFromAddress(sb.ptr,targetB);
        auto rd=V24XReachTargetFromAddress(sd.ptr,targetD);
        if(!rb.found || !rd.found) continue;
        if(rb.path!=rd.path) continue;
        if(rb.path.size()>5) continue;

        std::set<int> none;
        auto toggles=V24XFindFiveStateToggles(sa.bytes,sb.bytes,sc.bytes,sd.bytes,se.bytes,none);
        V24ManagerCandidate mc; mc.gameOffset=off;
        mc.ptrA=sa.ptr; mc.ptrB=sb.ptr; mc.ptrC=sc.ptr; mc.ptrD=sd.ptr; mc.ptrE=se.ptr;
        mc.reachB=rb; mc.reachD=rd;
        mc.ptrStable=(sa.ptr==sb.ptr && sb.ptr==sc.ptr && sc.ptr==sd.ptr && sd.ptr==se.ptr);
        mc.score=2600 + std::max(0,700-static_cast<int>(rb.path.size())*120);
        if(mc.ptrStable) mc.score+=1000;
        for(int s:semanticRefs) {
            const int dist=std::abs(s-off);
            if(dist==0) { mc.semanticNear=true; mc.score+=1200; }
            else if(dist<=8) { mc.semanticNear=true; mc.score+=700; }
            else if(dist<=0x40) { mc.semanticNear=true; mc.score+=250; }
        }
        for(const auto& t:toggles) {
            if(t.a==0 && t.b==1 && t.c==0 && t.d==1 && t.e==0) {
                mc.toggle=t; mc.hasToggle=true; mc.score+=2200+t.score; break;
            }
        }
        if(!mc.hasToggle) continue; // strict: no behavioral proof, no promotion
        out.push_back(mc);
    }
    std::sort(out.begin(),out.end(),[](const auto& x,const auto& y){
        if(x.score!=y.score) return x.score>y.score;
        return x.gameOffset<y.gameOffset;
    });
    return out;
}

void V24RejectReservedOpenContainerProfile(Report& r) {
    auto it=r.selectedFields.find("openContainersMap");
    if(it==r.selectedFields.end()) return;
    if(!V24ReservedGameField(it->second.offset)) return;
    std::cout << "[PROFILE FIX] openContainersMap Game +" << HexU32(it->second.offset)
              << " conflita com Game.LocalPlayer/Attack/Follow/Protocol; rejeitando evidencia antiga.\n";
    r.selectedFields.erase("openContainersMap");
    r.selectedFields.erase("openContainersPathDepth");
    for(int i=0;i<8;++i) r.selectedFields.erase("openContainersPath"+std::to_string(i));
    r.selectedFields.erase("openContainersSize");
    SaveCheckpoint(r,"v24_reject_reserved_opencontainers_root");
}


// ============================================================================
// V25 - INLINE OPEN-CONTAINER REGISTRY VALIDATION
// V24 proved that a semantic Container can be reached from Game+0x10 on both
// openings, but its strict validator assumed Game held a manager *pointer*.
// In classic x86 layouts the registry may instead be an inline tree/map object.
// V25 samples the candidate registry roots at EACH A/B/C/D/E state and requires:
//   A closed -> 0 reachable exact RTTI Containers
//   B open   -> target Container reachable
//   C closed -> 0
//   D open   -> reopened target reachable with the SAME path
//   E closed -> 0
// The first graph hop MUST be +0, preventing Game+0/+4/+8/+C from scanning
// forward into the real member and becoming false roots.
// ============================================================================

bool V25ExactRootPath(const V24XReachResult& rr) {
    return rr.found && !rr.path.empty() && rr.path.front()==0 && rr.path.size()<=5;
}

std::set<int> V25SemanticInlineRoots(const std::set<int>& a,const std::set<int>& b) {
    std::set<int> roots;
    const auto add=[&](int x) {
        if(x>=0x10 && x<=0x1000 && (x%4)==0 && !V24ReservedGameField(x)) roots.insert(x);
    };
    for(int x:a) add(x);
    for(int x:b) add(x);
    return roots;
}

bool V25FollowPathParent(uintptr_t rootAddr,const std::vector<int>& path,
                         uintptr_t& parent,int& finalOff) {
    if(path.empty()) return false;
    uintptr_t cur=rootAddr;
    for(size_t i=0;i+1<path.size();++i) {
        uint32_t p=0;
        if(!SafeRead(cur+path[i],p) || !p || !V24LooksHeapPointer(p)) return false;
        cur=static_cast<uintptr_t>(p);
    }
    parent=cur; finalOff=path.back();
    return true;
}

struct V25RootState {
    int reachable=0;
    bool targetFound=false;
    V24XReachResult targetReach;
    bool hasRegistryKey=false;
    uint32_t registryKey=0;
    int nodeKeyOffset=-1;
    int nodeValueOffset=-1;
};
using V25RootStates=std::map<int,V25RootState>;

V25RootStates V25CaptureRootStatesNow(uintptr_t game,const std::set<int>& roots,
                                      const std::set<uint32_t>& exactContainers,
                                      uint32_t target=0) {
    V25RootStates out;
    for(int root:roots) {
        V25RootState st;
        size_t checked=0;
        for(uint32_t p:exactContainers) {
            if(!p) continue;
            if(checked++>384) break;
            auto rr=V24XReachTargetFromAddress(game+root,p);
            if(!V25ExactRootPath(rr)) continue;
            ++st.reachable;
            if(target && p==target) {
                st.targetFound=true;
                st.targetReach=rr;
            }
        }
        if(target && !st.targetFound) {
            auto rr=V24XReachTargetFromAddress(game+root,target);
            if(V25ExactRootPath(rr)) {
                st.targetFound=true;
                st.targetReach=rr;
            }
        }
        if(st.targetFound) {
            uintptr_t node=0; int valueOff=-1;
            if(V25FollowPathParent(game+root,st.targetReach.path,node,valueOff) && valueOff>=4) {
                uint32_t key=0;
                if(SafeRead(node+valueOff-4,key) && key<=255) {
                    st.hasRegistryKey=true;
                    st.registryKey=key;
                    st.nodeKeyOffset=valueOff-4;
                    st.nodeValueOffset=valueOff;
                }
            }
        }
        out[root]=st;
    }
    return out;
}

struct V25InlineRootCandidate {
    int root=-1;
    V25RootState a,b,c,d,e;
    bool exactGetContainers=false,exactGetContainer=false;
    int score=0;
};

std::vector<V25InlineRootCandidate> V25EvaluateInlineRoots(
        const std::set<int>& roots,const std::set<int>& refsContainers,
        const std::set<int>& refsContainer,const V25RootStates& A,
        const V25RootStates& B,const V25RootStates& C,
        const V25RootStates& D,const V25RootStates& E) {
    std::vector<V25InlineRootCandidate> out;
    for(int root:roots) {
        auto ia=A.find(root),ib=B.find(root),ic=C.find(root),id=D.find(root),ie=E.find(root);
        if(ia==A.end()||ib==B.end()||ic==C.end()||id==D.end()||ie==E.end()) continue;
        const auto &a=ia->second,&b=ib->second,&c=ic->second,&d=id->second,&e=ie->second;
        if(a.reachable!=0 || c.reachable!=0 || e.reachable!=0) continue;
        if(!b.targetFound || !d.targetFound) continue;
        if(b.targetReach.path!=d.targetReach.path) continue;
        V25InlineRootCandidate x; x.root=root; x.a=a;x.b=b;x.c=c;x.d=d;x.e=e;
        x.exactGetContainers=refsContainers.count(root)!=0;
        x.exactGetContainer=refsContainer.count(root)!=0;
        x.score=5200;
        if(x.exactGetContainer) x.score+=2400;
        if(x.exactGetContainers) x.score+=1500;
        if(b.reachable==1) x.score+=700;
        if(d.reachable==1) x.score+=700;
        if(b.hasRegistryKey && d.hasRegistryKey && b.nodeKeyOffset==d.nodeKeyOffset &&
           b.nodeValueOffset==d.nodeValueOffset) x.score+=800;
        x.score+=std::max(0,900-static_cast<int>(b.targetReach.path.size())*120);
        out.push_back(x);
    }
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){
        if(a.score!=b.score) return a.score>b.score;
        return a.root<b.root;
    });
    return out;
}

void V25TryContainerIdFromRegistryKeySnapshot(Report& r,const V25RootState& b,
                                               const V25RootState& d,
                                               const V24XContainerObs& ob,
                                               const V24XContainerObs& od) {
    if(!b.hasRegistryKey || !d.hasRegistryKey || b.nodeKeyOffset!=d.nodeKeyOffset ||
       b.nodeValueOffset!=d.nodeValueOffset) return;
    const uint32_t keyB=b.registryKey,keyD=d.registryKey;
    struct Hit { V24ScalarHit h; uint32_t other=0; int score=0; };
    std::vector<Hit> hits;
    for(const auto& x:ob.ids) for(const auto& y:od.ids) {
        if(x.offset!=y.offset || x.type!=y.type) continue;
        if(!x.setterAgree || !y.setterAgree) continue;
        if(x.value!=keyB || y.value!=keyD) continue;
        auto cap=r.selectedFields.find("containerCapacity");
        if(cap!=r.selectedFields.end() && x.offset==cap->second.offset) continue;
        int sc=2500+x.score+y.score;
        if(x.type=="u32") sc+=300;
        hits.push_back({x,y.value,sc});
    }
    std::sort(hits.begin(),hits.end(),[](const auto& a,const auto& b){return a.score>b.score;});
    std::cout << "[REGISTRY NODE KEY XCHECK] node+" << HexU32(b.nodeKeyOffset)
              << " keys=" << keyB << "/" << keyD
              << " valuePtr+" << HexU32(b.nodeValueOffset)
              << " matchingContainerIdFields=" << hits.size() << "\n";
    for(size_t i=0;i<hits.size() && i<8;++i)
        std::cout << "  " << hits[i].h.type << " Container+" << HexU32(hits[i].h.offset)
                  << " values=" << hits[i].h.value << "/" << hits[i].other
                  << " score=" << hits[i].score << "\n";
    if(!hits.empty() && (hits.size()==1 || hits[0].score>=hits[1].score+400)) {
        r.selectedFields["containerId"]={hits[0].h.type,hits[0].h.offset,
            "semantic_getContainer_registry_key_match_reopen"};
        r.selectedFields["openContainersNodeKey"]={"u32",b.nodeKeyOffset,
            "semantic_inline_registry_node_key"};
        r.selectedFields["openContainersNodeValue"]={"ContainerPtr",b.nodeValueOffset,
            "semantic_inline_registry_node_value"};
        std::cout << "[CONTAINER ID] CONFIRMADO Container+" << HexU32(hits[0].h.offset)
                  << " por igualdade com a chave do registry getContainer ("
                  << keyB << "/" << keyD << ").\n";
        SaveCheckpoint(r,"container_id_registry_key_reopen_v25");
    }
}

void RunOpenContainerDiscoveryV24Multi(Report& r) {
    std::cout << "\n============================================================\n"
              << "     OPEN CONTAINERS / CONTAINER OBJECT V30 INLINE-REGISTRY\n"
              << "============================================================\n"
              << "[METHOD] CLOSED -> OPEN -> CLOSED -> OPEN -> CLOSED; reabra o MESMO container.\n"
              << "[METHOD] current-build getContainers/getContainer this+refs + RTTI Container + Item RTTI graph reachability.\n"
              << "[MODE] READ-ONLY: nenhuma escrita, hook ou chamada interna.\n";
    if(!r.gameOwnerRva || !r.localPlayerPtr) { std::cout << "[CONTAINERS] Game/LocalPlayer indisponivel.\n"; return; }
    const uintptr_t game=r.module.base+r.gameOwnerRva;
    V24RejectReservedOpenContainerProfile(r);
    const auto readyField=[&](const char* n){ auto it=r.selectedFields.find(n); return it!=r.selectedFields.end() && it->second.offset>=0 && EvidenceScore(it->second.status)>=80; };
    if(readyField("openContainersMap") && readyField("containerItems") && readyField("containerId") &&
       readyField("containerCapacity") && readyField("containerName")) {
        std::cout << "[CONTAINERS QUICK-CHECK] profile completo desta fingerprint; recalibracao A->B->C->D->E pulada.\n"
                  << "  OpenContainers : Game+" << HexU32(r.selectedFields["openContainersMap"].offset) << " [" << r.selectedFields["openContainersMap"].type << "]\n"
                  << "  Container.ID   : +" << HexU32(r.selectedFields["containerId"].offset) << "\n"
                  << "  Capacity       : +" << HexU32(r.selectedFields["containerCapacity"].offset) << "\n"
                  << "  Name           : +" << HexU32(r.selectedFields["containerName"].offset) << "\n"
                  << "  Items          : +" << HexU32(r.selectedFields["containerItems"].offset) << "\n";
        return;
    }
    const auto refsAllA=V24XThisRefsForToken(r,"getContainers");
    const auto refsAllB=V24XThisRefsForToken(r,"getContainer");
    V24XPrintRefs("[getContainers this+]",refsAllA);
    V24XPrintRefs("[getContainer  this+]",refsAllB);
    std::set<int> semanticRefs=refsAllA; semanticRefs.insert(refsAllB.begin(),refsAllB.end());
    const auto v25Roots=V25SemanticInlineRoots(refsAllA,refsAllB);
    std::cout << "[V25 INLINE ROOT SEMANTIC CANDIDATES] " << v25Roots.size();
    for(int x:v25Roots) std::cout << " +" << HexU32(x);
    std::cout << "\n";

    if(AskYesNo("Mapear containers com FECHADO -> ABERTO -> FECHADO -> ABERTO -> FECHADO agora? [S/N]")!='s') {
        std::cout << "[CONTAINERS] teste pulado.\n"; return;
    }

    const size_t win=0x4000;
    std::vector<uint8_t> A,B,C,D,E;
    WaitEnter("[A] Feche TODOS os containers. Nao altere mais nada. ENTER... ");
    if(!CaptureWindow(game,A,win)) { std::cout << "[CONTAINERS] falha snapshot A.\n"; return; }
    const auto managersA=V24CaptureManagerRoots(game);
    const auto setA=V24ScanExactRttiObjects(r,"Container");
    const auto v25A=V25CaptureRootStatesNow(game,v25Roots,setA,0);

    WaitEnter("[B] Abra EXATAMENTE UM backpack/container COM item dentro. ENTER... ");
    if(!CaptureWindow(game,B,win)) { std::cout << "[CONTAINERS] falha snapshot B.\n"; return; }
    const auto managersB=V24CaptureManagerRoots(game);
    const auto setB=V24ScanExactRttiObjects(r,"Container");
    auto obsB=V24XOpenObservations(r,setA,setB);
    V24XPrintObs("[OPEN#1 RTTI/ITEM OBS]",obsB);
    const uint32_t v25TargetB=(!obsB.empty() && obsB[0].hasItems)?obsB[0].ptr:(!obsB.empty()?obsB[0].ptr:0);
    const auto v25B=V25CaptureRootStatesNow(game,v25Roots,setB,v25TargetB);
    auto linksB=V24XFindRegistryLinks(r,game,refsAllA,refsAllB,obsB);
    V24XPrintLinks("[OPEN#1 GAME->CONTAINER REACH]",linksB,obsB);
    if(!obsB.empty() && obsB[0].hasItems) {
        std::cout << "[OPEN#1 BEST CONTAINER] semantic object decode while alive\n";
        V24DiscoverContainerObject(r,nullptr,obsB[0].ptr);
    }

    WaitEnter("[C] Feche o MESMO container; deixe todos fechados. ENTER... ");
    if(!CaptureWindow(game,C,win)) { std::cout << "[CONTAINERS] falha snapshot C.\n"; return; }
    const auto managersC=V24CaptureManagerRoots(game);
    const auto setC=V24ScanExactRttiObjects(r,"Container");
    const auto v25C=V25CaptureRootStatesNow(game,v25Roots,setC,0);

    WaitEnter("[D] Abra NOVAMENTE O MESMO backpack/container, sem mudar os itens. ENTER... ");
    if(!CaptureWindow(game,D,win)) { std::cout << "[CONTAINERS] falha snapshot D.\n"; return; }
    const auto managersD=V24CaptureManagerRoots(game);
    const auto setD=V24ScanExactRttiObjects(r,"Container");
    auto obsD=V24XOpenObservations(r,setC,setD);
    V24XPrintObs("[OPEN#2 RTTI/ITEM OBS]",obsD);
    const uint32_t v25TargetD=(!obsD.empty() && obsD[0].hasItems)?obsD[0].ptr:(!obsD.empty()?obsD[0].ptr:0);
    const auto v25D=V25CaptureRootStatesNow(game,v25Roots,setD,v25TargetD);
    auto linksD=V24XFindRegistryLinks(r,game,refsAllA,refsAllB,obsD);
    V24XPrintLinks("[OPEN#2 GAME->CONTAINER REACH]",linksD,obsD);

    auto pairs=V24XPairObservations(obsB,obsD);
    std::cout << "[SAME-CONTAINER REOPEN PAIRS] " << pairs.size() << "\n";
    for(size_t i=0;i<pairs.size() && i<10;++i)
        std::cout << "  B=" << HexU32(obsB[pairs[i].a].ptr) << " D=" << HexU32(obsD[pairs[i].b].ptr)
                  << " items+" << HexU32(obsB[pairs[i].a].bestItems.offset)
                  << " score=" << pairs[i].score << "\n";

    int bestRoot=-1;
    std::vector<int> bestPathB,bestPathD;
    size_t bestBi=0,bestDi=0;
    int bestRootScore=-1;
    for(const auto& p:pairs) {
        for(const auto& lb:linksB) if(lb.obsIndex==p.a) {
            for(const auto& ld:linksD) if(ld.obsIndex==p.b && ld.rootOffset==lb.rootOffset) {
                int s=p.score+lb.score+ld.score;
                if(lb.reach.path==ld.reach.path) s+=1600;
                if(refsAllA.count(lb.rootOffset)) s+=1200;
                if(s>bestRootScore) {
                    bestRootScore=s; bestRoot=lb.rootOffset; bestPathB=lb.reach.path; bestPathD=ld.reach.path;
                    bestBi=p.a; bestDi=p.b;
                }
            }
        }
    }

    if(!pairs.empty()) {
        const auto& bp=pairs.front();
        V24XPromoteContainerCrossOpen(r,obsB[bp.a],obsD[bp.b]);
    }

    if(bestRoot>=0 && !V24ReservedGameField(bestRoot)) {
        std::cout << "[REGISTRY ROOT REOPEN XCHECK] Game +" << HexU32(bestRoot)
                  << " pathB=" << V24XPathText(bestPathB)
                  << " pathD=" << V24XPathText(bestPathD)
                  << " score=" << bestRootScore << "\n";
        const bool pathStable=bestPathB==bestPathD;
        if(pathStable && bestRootScore>4000) {
            // V24: this graph-only path is diagnostic. Strict manager proof below/after E owns promotion.
            r.selectedFields["openContainersGraphCandidate"]={"container_registry_candidate",bestRoot,"candidate_graph_reachability_two_opens"};
            r.selectedFields["openContainersPathDepth"]={"count",static_cast<int>(bestPathB.size()),"semantic_getContainers_reachability_two_opens"};
            for(size_t i=0;i<bestPathB.size() && i<5;++i)
                r.selectedFields["openContainersPath"+std::to_string(i)]={"node_offset",bestPathB[i],"semantic_getContainers_reachability_two_opens"};
            std::cout << "[OPEN CONTAINERS GRAPH CANDIDATE] Game +" << HexU32(bestRoot)
                      << " follow " << V24XPathText(bestPathB) << "\n";
            SaveCheckpoint(r,"open_containers_graph_candidate_v24");
        }
    } else {
        if(bestRoot>=0 && V24ReservedGameField(bestRoot))
            std::cout << "[REGISTRY ROOT REJECT] Game +" << HexU32(bestRoot)
                      << " e campo Game reservado/ja confirmado; reachability permissiva rejeitada.\n";
        else
            std::cout << "[REGISTRY ROOT] nenhum mesmo Game root alcancou um Container estruturalmente igual nas duas aberturas.\n";
    }

    if(!obsD.empty() && obsD[0].hasItems) {
        std::cout << "[OPEN#2 BEST CONTAINER] semantic object decode while alive\n";
        V24DiscoverContainerObject(r,nullptr,obsD[0].ptr);
    }

    WaitEnter("[E] Feche o MESMO container novamente; deixe todos fechados. ENTER... ");
    if(!CaptureWindow(game,E,win)) { std::cout << "[CONTAINERS] falha snapshot E.\n"; return; }
    const auto managersE=V24CaptureManagerRoots(game);
    const auto setE=V24ScanExactRttiObjects(r,"Container");
    const auto v25E=V25CaptureRootStatesNow(game,v25Roots,setE,0);

    // V24 strict root: use the semantic same-container pair and require a manager
    // object referenced by a non-reserved Game field to show a 0->1->0->1->0
    // transition internally while reaching the semantic Container on both opens.
    if(!pairs.empty()) {
        const auto &pp=pairs.front();
        const uint32_t targetB=obsB[pp.a].ptr, targetD=obsD[pp.b].ptr;
        auto strict=V24FindStrictManagerCandidates(managersA,managersB,managersC,managersD,managersE,
                                                    targetB,targetD,semanticRefs);
        std::cout << "[STRICT MANAGER ROOT A->B->C->D->E] candidates=" << strict.size() << "\n";
        for(size_t i=0;i<strict.size() && i<12;++i) {
            const auto &m=strict[i];
            std::cout << "  Game +" << HexU32(m.gameOffset)
                      << " -> manager " << HexU32(m.ptrB)
                      << " reach=" << V24XPathText(m.reachB.path)
                      << " count+" << HexU32(m.toggle.offset)
                      << " " << m.toggle.a << "->" << m.toggle.b << "->" << m.toggle.c
                      << "->" << m.toggle.d << "->" << m.toggle.e
                      << " stablePtr=" << (m.ptrStable?"YES":"NO")
                      << " semanticNear=" << (m.semanticNear?"YES":"NO")
                      << " score=" << m.score << "\n";
        }
        if(!strict.empty() && (strict.size()==1 || strict[0].score>=strict[1].score+500)) {
            const auto &m=strict[0];
            r.selectedFields["openContainersMap"]={"manager_ptr",m.gameOffset,"runtime_manager_ptr_reachability_five_state"};
            r.selectedFields["openContainersSize"]={m.toggle.width==1?"u8":(m.toggle.width==2?"u16":"u32"),
                                                      m.toggle.offset,"runtime_manager_internal_count_five_state"};
            r.selectedFields["openContainersPathDepth"]={"count",static_cast<int>(m.reachB.path.size()),"runtime_manager_reach_path_two_opens"};
            for(size_t i=0;i<m.reachB.path.size() && i<8;++i)
                r.selectedFields["openContainersPath"+std::to_string(i)]={"node_offset",m.reachB.path[i],"runtime_manager_reach_path_two_opens"};
            std::cout << "[OPEN CONTAINERS MANAGER] CONFIRMADO Game +" << HexU32(m.gameOffset)
                      << " -> manager*, internalCount +" << HexU32(m.toggle.offset)
                      << " follow " << V24XPathText(m.reachB.path) << "\n";
            SaveCheckpoint(r,"open_containers_manager_strict_five_state");
        } else {
            std::cout << "[OPEN CONTAINERS MANAGER] ainda ambiguo; nenhum root promovido.\n";
        }
    }

    // V25 exact inline-member proof. Unlike V24's manager-pointer test this
    // evaluates the semantic member address itself at each historical state.
    if(!pairs.empty()) {
        auto inlineRoots=V25EvaluateInlineRoots(v25Roots,refsAllA,refsAllB,v25A,v25B,v25C,v25D,v25E);
        std::cout << "[INLINE REGISTRY ROOT A->B->C->D->E] candidates=" << inlineRoots.size() << "\n";
        for(size_t i=0;i<inlineRoots.size() && i<12;++i) {
            const auto &x=inlineRoots[i];
            std::cout << "  Game +" << HexU32(x.root)
                      << " path=" << V24XPathText(x.b.targetReach.path)
                      << " reachable=" << x.a.reachable << "->" << x.b.reachable << "->"
                      << x.c.reachable << "->" << x.d.reachable << "->" << x.e.reachable
                      << " getContainers=" << (x.exactGetContainers?"EXACT":"no")
                      << " getContainer=" << (x.exactGetContainer?"EXACT":"no")
                      << " key=" << (x.b.hasRegistryKey?std::to_string(x.b.registryKey):std::string("?"))
                      << "/" << (x.d.hasRegistryKey?std::to_string(x.d.registryKey):std::string("?"))
                      << " score=" << x.score << "\n";
        }
        if(!inlineRoots.empty() && (inlineRoots.size()==1 || inlineRoots[0].score>=inlineRoots[1].score+500)) {
            const auto &x=inlineRoots[0];
            r.selectedFields["openContainersMap"]={"inline_registry",x.root,
                "semantic_getContainer_exact_inline_root_closed_open_reopen"};
            r.selectedFields["openContainersPathDepth"]={"count",static_cast<int>(x.b.targetReach.path.size()),
                "semantic_inline_registry_reach_path"};
            for(size_t i=0;i<x.b.targetReach.path.size() && i<8;++i)
                r.selectedFields["openContainersPath"+std::to_string(i)]={"node_offset",x.b.targetReach.path[i],
                    "semantic_inline_registry_reach_path"};
            std::cout << "[OPEN CONTAINERS INLINE REGISTRY] CONFIRMADO Game +" << HexU32(x.root)
                      << " follow " << V24XPathText(x.b.targetReach.path)
                      << " com reachability " << x.a.reachable << "->" << x.b.reachable << "->"
                      << x.c.reachable << "->" << x.d.reachable << "->" << x.e.reachable << ".\n";
            SaveCheckpoint(r,"open_containers_inline_registry_v25");
            const auto &pp=pairs.front();
            V25TryContainerIdFromRegistryKeySnapshot(r,x.b,x.d,obsB[pp.a],obsD[pp.b]);
        } else {
            std::cout << "[OPEN CONTAINERS INLINE REGISTRY] ainda ambiguo; nenhum inline root promovido.\n";
        }
    }

    auto toggles=V24XFindFiveStateToggles(A,B,C,D,E,semanticRefs);
    std::cout << "[GAME FIVE-STATE TOGGLES A->B->C->D->E] " << toggles.size() << "\n";
    for(size_t i=0;i<toggles.size() && i<16;++i) {
        const auto& h=toggles[i];
        std::cout << "  Game +" << HexU32(h.offset) << " width=" << h.width
                  << " " << h.a << "->" << h.b << "->" << h.c << "->" << h.d << "->" << h.e
                  << " score=" << h.score << "\n";
    }
    if(!toggles.empty()) {
        int chosen=-1;
        for(size_t i=0;i<toggles.size();++i) {
            const auto& h=toggles[i];
            if(h.a==0 && h.b==1 && h.c==0 && h.d==1 && h.e==0) {
                if(bestRoot<0 || std::abs(h.offset-bestRoot)<=0x80 || h.score>=1400) { chosen=static_cast<int>(i); break; }
            }
        }
        if(chosen>=0) {
            const auto& h=toggles[chosen];
            const std::string ty=h.width==1?"u8":(h.width==2?"u16":"u32");
            auto rootIt=r.selectedFields.find("openContainersMap");
            const bool strictManager=(rootIt!=r.selectedFields.end() &&
                                      rootIt->second.status.find("manager_ptr_reachability_five_state")!=std::string::npos);
            if(strictManager) {
                std::cout << "[GAME COUNT DIAG] Game +" << HexU32(h.offset)
                          << " tambem alternou 0->1->0->1->0, mas o count confirmado pertence ao manager; nao sobrescrevendo.\n";
            } else {
                r.selectedFields["openContainersSize"]={ty,h.offset,"runtime_closed_open_closed_open_closed_count"};
                std::cout << "[OPEN CONTAINERS COUNT] CONFIRMADO Game +" << HexU32(h.offset)
                          << " " << ty << " 0->1->0->1->0\n";
                SaveCheckpoint(r,"open_containers_five_state_count");
            }
        }
    }

    // If the registry root was not graph-confirmed, an exact getContainers this+ ref
    // immediately adjacent to a unique five-state count is still useful as a candidate,
    // but is NOT promoted as confirmed.
    if(bestRoot<0 && !toggles.empty() && !refsAllA.empty()) {
        int bestRef=-1,bestDist=0x7fffffff;
        for(int ref:refsAllA) for(const auto& h:toggles) {
            if(!(h.a==0 && h.b==1 && h.c==0 && h.d==1 && h.e==0)) continue;
            const int dist=std::abs(ref-h.offset);
            if(dist<bestDist) { bestDist=dist; bestRef=ref; }
        }
        if(bestRef>=0)
            std::cout << "[OPEN CONTAINERS ROOT CANDIDATE] getContainers this+" << HexU32(bestRef)
                      << " nearest five-state count distance=" << bestDist << " bytes; mantendo candidate ate reachability fechar.\n";
    }

    SaveCheckpoint(r,"open_containers_v25_inline_complete");
}




// ============================================================================
// V27 - TILE / GROUND THING LAYOUT DISCOVERY (x86)
// READ-ONLY. Reversible current-SQM transition:
//   A: current tile has no Gold Coin on ground
//   B: user drops one Gold Coin (Client/Thing ID 2148) on same SQM
//   C: user picks it back up
// No internal game calls. Tile RTTI + LocalPlayer position + Thing RTTI are used
// to identify the live Tile and its Thing collection in the current build.
// ============================================================================
struct V26ThingCollection {
    std::string kind;
    int offset=-1;
    int elemSize=0;
    int count=0;
    int goldCount=0;
    int score=0;
    bool semantic=false;
    std::vector<uint32_t> things;
};

bool V26IsThingObject(const Report& r,uint32_t p) {
    if(!p) return false;
    for(const char* cls:{"Item","LocalPlayer","Player","Monster","Npc"})
        if(IsExactRttiObjectV19(r,p,cls)) return true;
    return false;
}

int V26ClientItemId(const Report& r,uint32_t p) {
    if(!p || !IsExactRttiObjectV19(r,p,"Item")) return -1;
    auto it=r.selectedFields.find("itemId");
    if(it==r.selectedFields.end() || it->second.offset<0) return -1;
    if(it->second.type=="u16") { uint16_t v=0; if(SafeRead(static_cast<uintptr_t>(p)+it->second.offset,v)) return v; }
    if(it->second.type=="u32") { uint32_t v=0; if(SafeRead(static_cast<uintptr_t>(p)+it->second.offset,v)) return static_cast<int>(v); }
    return -1;
}

bool V26ParseVectorThings(const Report& r,uint32_t obj,int off,int elem,std::vector<uint32_t>& things) {
    things.clear();
    uint32_t begin=0,end=0,cap=0;
    if(!SafeRead(static_cast<uintptr_t>(obj)+off,begin) ||
       !SafeRead(static_cast<uintptr_t>(obj)+off+4,end) ||
       !SafeRead(static_cast<uintptr_t>(obj)+off+8,cap)) return false;
    if(!begin || end<begin || cap<end || elem<4) return false;
    const uint32_t bytes=end-begin, capBytes=cap-begin;
    if(bytes%static_cast<uint32_t>(elem) || capBytes%static_cast<uint32_t>(elem)) return false;
    const int n=static_cast<int>(bytes/elem), cn=static_cast<int>(capBytes/elem);
    if(n<1 || n>80 || cn<n || cn>160 || !V24LooksHeapPointer(begin)) return false;
    for(int i=0;i<n;++i) {
        uint32_t p=0;
        if(!SafeRead(static_cast<uintptr_t>(begin)+i*elem,p) || !p || !V26IsThingObject(r,p)) return false;
        things.push_back(p);
    }
    return true;
}

bool V26ParseDequeThings(const Report& r,uint32_t obj,int off,int elem,std::vector<uint32_t>& things) {
    things.clear();
    uint32_t map=0,mapsize=0,myoff=0,mysize=0;
    if(!SafeRead(static_cast<uintptr_t>(obj)+off,map) ||
       !SafeRead(static_cast<uintptr_t>(obj)+off+4,mapsize) ||
       !SafeRead(static_cast<uintptr_t>(obj)+off+8,myoff) ||
       !SafeRead(static_cast<uintptr_t>(obj)+off+12,mysize)) return false;
    if(!map || !V24LooksHeapPointer(map) || mapsize==0 || mapsize>512 || mysize<1 || mysize>80) return false;
    const int block=elem<=1?16:(elem<=2?8:(elem<=4?4:(elem<=8?2:1)));
    for(uint32_t i=0;i<mysize;++i) {
        const uint32_t logical=myoff+i;
        uint32_t bi=logical/static_cast<uint32_t>(block);
        if((mapsize&(mapsize-1))==0) bi&=(mapsize-1); else bi%=mapsize;
        const uint32_t inner=logical%static_cast<uint32_t>(block);
        uint32_t blockPtr=0,p=0;
        if(!SafeRead(static_cast<uintptr_t>(map)+bi*4,blockPtr) || !blockPtr || !V24LooksHeapPointer(blockPtr)) return false;
        if(!SafeRead(static_cast<uintptr_t>(blockPtr)+inner*elem,p) || !p || !V26IsThingObject(r,p)) return false;
        things.push_back(p);
    }
    return true;
}

std::vector<V26ThingCollection> V26FindThingCollections(const Report& r,uint32_t tile) {
    std::vector<V26ThingCollection> out;
    const auto semantic=V24DecodeLeaOffsetsForToken(r,"getThings");
    for(int off=4;off<=0x180;off+=4) for(int elem:{4,8}) {
        for(const char* kind:{"vector","deque"}) {
            std::vector<uint32_t> things;
            const bool ok=std::string(kind)=="vector" ? V26ParseVectorThings(r,tile,off,elem,things)
                                                       : V26ParseDequeThings(r,tile,off,elem,things);
            if(!ok) continue;
            V26ThingCollection c; c.kind=kind; c.offset=off; c.elemSize=elem; c.things=things;
            c.count=static_cast<int>(things.size()); c.semantic=semantic.count(off)!=0;
            for(uint32_t p:things) if(V26ClientItemId(r,p)==2148) ++c.goldCount;
            c.score=700+c.count*30+(c.semantic?1200:0)+(elem==4?60:0);
            bool dup=false;
            for(auto& e:out) if(e.kind==c.kind && e.offset==c.offset && e.elemSize==c.elemSize) {
                if(c.score>e.score) e=c;
                dup=true;
                break;
            }
            if(!dup) out.push_back(c);
        }
    }
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){
        if(a.score!=b.score) return a.score>b.score;
        if(a.offset!=b.offset) return a.offset<b.offset;
        return a.elemSize<b.elemSize;
    });
    return out;
}

struct V26TilePosMatch { uint32_t tile=0; int offset=-1; std::string encoding; int score=0; };

// V27: the V26 exact-RTTI heap scan was too strict for this build: semantic Tile
// methods were found, but no live object whose first vptr equaled the single RTTI
// vtable matched the current LocalPlayer position. V27 therefore locates the Tile
// through its current-build getThings/getPosition accessors and the already-known
// LocalPlayer pointer contained in Tile::m_things. Exact RTTI is a bonus, not a gate.
struct V27TileCandidate {
    uint32_t tile=0;
    int posOffset=-1;
    std::string posEncoding;
    V26ThingCollection things;
    bool exactTileRtti=false;
    bool semanticThings=false;
    bool semanticPosition=false;
    int score=0;
};

std::vector<uint32_t> V27ScanDwordValue(uint32_t value,size_t limit=20000) {
    std::vector<uint32_t> out;
    uintptr_t p=0x10000;
    const uintptr_t end=0x7FFF0000u;
    const size_t chunkMax=1u<<20;
    std::vector<uint8_t> buf;
    while(p<end && out.size()<limit) {
        MEMORY_BASIC_INFORMATION mbi{};
        if(!VirtualQuery(reinterpret_cast<LPCVOID>(p),&mbi,sizeof(mbi))) break;
        const uintptr_t base=reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t rend=base+mbi.RegionSize;
        if(mbi.State==MEM_COMMIT && !(mbi.Protect&PAGE_GUARD) && IsReadableProtection(mbi.Protect) &&
           (mbi.Type==MEM_PRIVATE || mbi.Type==MEM_MAPPED)) {
            for(uintptr_t q=base;q<rend && out.size()<limit;) {
                const size_t take=static_cast<size_t>(std::min<uintptr_t>(chunkMax,rend-q));
                buf.resize(take);
                if(SafeCopy(q,buf.data(),take)) {
                    for(size_t i=0;i+4<=take && out.size()<limit;i+=4) {
                        uint32_t x=0; std::memcpy(&x,buf.data()+i,4);
                        if(x==value) out.push_back(static_cast<uint32_t>(q+i));
                    }
                }
                q+=take;
            }
        }
        if(rend<=p) break;
        p=rend;
    }
    return out;
}

struct V27SetRef { uint32_t address=0; uint32_t value=0; };
std::vector<V27SetRef> V27ScanDwordRefsToSet(const std::set<uint32_t>& values,size_t limit=50000) {
    std::vector<V27SetRef> out;
    if(values.empty()) return out;
    uintptr_t p=0x10000;
    const uintptr_t end=0x7FFF0000u;
    const size_t chunkMax=1u<<20;
    std::vector<uint8_t> buf;
    while(p<end && out.size()<limit) {
        MEMORY_BASIC_INFORMATION mbi{};
        if(!VirtualQuery(reinterpret_cast<LPCVOID>(p),&mbi,sizeof(mbi))) break;
        const uintptr_t base=reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t rend=base+mbi.RegionSize;
        if(mbi.State==MEM_COMMIT && !(mbi.Protect&PAGE_GUARD) && IsReadableProtection(mbi.Protect) &&
           (mbi.Type==MEM_PRIVATE || mbi.Type==MEM_MAPPED)) {
            for(uintptr_t q=base;q<rend && out.size()<limit;) {
                const size_t take=static_cast<size_t>(std::min<uintptr_t>(chunkMax,rend-q));
                buf.resize(take);
                if(SafeCopy(q,buf.data(),take)) {
                    for(size_t i=0;i+4<=take && out.size()<limit;i+=4) {
                        uint32_t x=0; std::memcpy(&x,buf.data()+i,4);
                        if(values.count(x)) out.push_back({static_cast<uint32_t>(q+i),x});
                    }
                }
                q+=take;
            }
        }
        if(rend<=p) break;
        p=rend;
    }
    return out;
}

bool V27DecodeThisDisp(const uint8_t* b,size_t n,size_t mrPos,int& off) {
    if(mrPos>=n) return false;
    const uint8_t mr=b[mrPos];
    const int mod=(mr>>6)&3, rm=mr&7;
    if(rm!=1 || mod==0 || mod==3) return false; // current this in ECX, require disp
    if(mod==1) {
        if(mrPos+1>=n) return false;
        off=static_cast<int8_t>(b[mrPos+1]);
        return off>=0;
    }
    if(mrPos+4>=n) return false;
    int32_t d=0; std::memcpy(&d,b+mrPos+1,4); off=d;
    return off>=0;
}

std::set<int> V27DecodeThisFieldOffsetsForToken(const Report& r,const std::string& token) {
    std::set<int> out=V24DecodeLeaOffsetsForToken(r,token);
    auto it=r.binds.find(token); if(it==r.binds.end()) return out;
    for(const auto& c:it->second.candidates) {
        std::vector<uint8_t> b(96);
        if(!SafeCopy(r.module.base+c.methodRva,b.data(),b.size())) continue;
        for(size_t i=0;i+3<b.size();++i) {
            int off=-1;
            const uint8_t op=b[i];
            if(op==0x8B || op==0x8D || op==0x8A || op==0x89 || op==0x88 || op==0x39 || op==0x3B) {
                if(V27DecodeThisDisp(b.data(),b.size(),i+1,off) && off<=0x400) out.insert(off);
            } else if(op==0x0F && i+2<b.size()) {
                const uint8_t op2=b[i+1];
                if(op2==0xB6 || op2==0xB7 || op2==0xBE || op2==0xBF) {
                    if(V27DecodeThisDisp(b.data(),b.size(),i+2,off) && off<=0x400) out.insert(off);
                }
            }
        }
    }
    return out;
}

bool V27ReadPositionAt(uint32_t obj,int off,uint32_t x,uint32_t y,uint8_t z,std::string& enc) {
    uint16_t x16=0,y16=0; uint8_t z8=0;
    if(SafeRead(static_cast<uintptr_t>(obj)+off,x16) &&
       SafeRead(static_cast<uintptr_t>(obj)+off+2,y16) &&
       SafeRead(static_cast<uintptr_t>(obj)+off+4,z8) && x16==x && y16==y && z8==z) {
        enc="u16,u16,u8"; return true;
    }
    if((off%4)==0) {
        uint32_t x32=0,y32=0,z32=0;
        if(SafeRead(static_cast<uintptr_t>(obj)+off,x32) &&
           SafeRead(static_cast<uintptr_t>(obj)+off+4,y32) &&
           SafeRead(static_cast<uintptr_t>(obj)+off+8,z32) && x32==x && y32==y && z32==z) {
            enc="u32,u32,u32"; return true;
        }
    }
    return false;
}

std::set<int> V27ThingSemanticOffsets(const Report& r) {
    std::set<int> out;
    for(const char* tok:{"getThings","getThingCount","getGround","getTopThing","getTopLookThing","getTopUseThing","getTopMoveThing","getTopCreature"}) {
        auto x=V27DecodeThisFieldOffsetsForToken(r,tok); out.insert(x.begin(),x.end());
    }
    return out;
}

std::set<int> V27PositionSemanticOffsets(const Report& r) {
    return V27DecodeThisFieldOffsetsForToken(r,"getPosition");
}

bool V27ContainsPtr(const std::vector<uint32_t>& v,uint32_t p) {
    return std::find(v.begin(),v.end(),p)!=v.end();
}

std::vector<V27TileCandidate> V27LocateCurrentTileCandidates(const Report& r,uint32_t x,uint32_t y,uint8_t z,
                                                              int forcedThings=-1,int forcedPos=-1,int forcedElem=-1) {
    std::vector<V27TileCandidate> out;
    if(!r.localPlayerPtr) return out;

    auto thingSem=V27ThingSemanticOffsets(r);
    auto posSem=V27PositionSemanticOffsets(r);
    if(forcedThings>=0) thingSem.insert(forcedThings);
    if(forcedPos>=0) posSem.insert(forcedPos);

    std::vector<int> thingOffsets;
    if(!thingSem.empty()) thingOffsets.assign(thingSem.begin(),thingSem.end());
    else for(int o=4;o<=0x100;o+=4) thingOffsets.push_back(o);

    std::vector<int> posOffsets;
    if(!posSem.empty()) posOffsets.assign(posSem.begin(),posSem.end());
    else for(int o=4;o<=0x140;++o) posOffsets.push_back(o);

    const auto lpRefs=V27ScanDwordValue(static_cast<uint32_t>(r.localPlayerPtr),24000);
    std::set<uint32_t> possibleBegins;
    for(uint32_t ref:lpRefs) {
        for(int elem:{4,8}) {
            if(forcedElem>0 && elem!=forcedElem) continue;
            for(int i=0;i<16;++i) {
                const uint32_t d=static_cast<uint32_t>(i*elem);
                if(ref>d) possibleBegins.insert(ref-d);
            }
        }
    }
    const auto headers=V27ScanDwordRefsToSet(possibleBegins,60000);

    std::set<std::tuple<uint32_t,int,int,int,std::string>> seen;
    for(const auto& hr:headers) {
        for(int toff:thingOffsets) {
            if(hr.address<=static_cast<uint32_t>(toff)) continue;
            const uint32_t owner=hr.address-static_cast<uint32_t>(toff);
            if(owner<0x10000u || !V24LooksHeapPointer(owner)) continue;
            for(int elem:{4,8}) {
                if(forcedElem>0 && elem!=forcedElem) continue;
                std::vector<uint32_t> things;
                if(!V26ParseVectorThings(r,owner,toff,elem,things) || !V27ContainsPtr(things,static_cast<uint32_t>(r.localPlayerPtr))) continue;
                V26ThingCollection col; col.kind="vector"; col.offset=toff; col.elemSize=elem; col.things=things;
                col.count=static_cast<int>(things.size()); col.semantic=thingSem.count(toff)!=0;
                for(uint32_t q:things) if(V26ClientItemId(r,q)==2148) ++col.goldCount;
                col.score=800+col.count*30+(col.semantic?1500:0)+(elem==4?80:0);

                bool gotPos=false;
                for(int poff:posOffsets) {
                    std::string enc;
                    if(!V27ReadPositionAt(owner,poff,x,y,z,enc)) continue;
                    gotPos=true;
                    V27TileCandidate c; c.tile=owner; c.posOffset=poff; c.posEncoding=enc; c.things=col;
                    c.exactTileRtti=IsExactRttiObjectV19(r,owner,"Tile");
                    c.semanticThings=thingSem.count(toff)!=0;
                    c.semanticPosition=posSem.count(poff)!=0;
                    c.score=2500+col.score+(c.semanticThings?1800:0)+(c.semanticPosition?1800:0)+(c.exactTileRtti?1200:0);
                    auto key=std::make_tuple(c.tile,c.posOffset,c.things.offset,c.things.elemSize,c.things.kind);
                    if(seen.insert(key).second) out.push_back(c);
                }
                // If semantic getPosition produced noise, brute the owner only after semantic attempts failed.
                if(!gotPos && !posSem.empty()) {
                    for(int poff=4;poff<=0x140;++poff) {
                        std::string enc;
                        if(!V27ReadPositionAt(owner,poff,x,y,z,enc)) continue;
                        V27TileCandidate c; c.tile=owner; c.posOffset=poff; c.posEncoding=enc; c.things=col;
                        c.exactTileRtti=IsExactRttiObjectV19(r,owner,"Tile");
                        c.semanticThings=thingSem.count(toff)!=0; c.semanticPosition=false;
                        c.score=2200+col.score+(c.semanticThings?1800:0)+(c.exactTileRtti?1200:0);
                        auto key=std::make_tuple(c.tile,c.posOffset,c.things.offset,c.things.elemSize,c.things.kind);
                        if(seen.insert(key).second) out.push_back(c);
                    }
                }
            }
        }
    }
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){
        if(a.score!=b.score) return a.score>b.score;
        if(a.tile!=b.tile) return a.tile<b.tile;
        if(a.things.offset!=b.things.offset) return a.things.offset<b.things.offset;
        return a.posOffset<b.posOffset;
    });
    return out;
}

const V27TileCandidate* V27FindLayoutCandidate(const std::vector<V27TileCandidate>& v,const V27TileCandidate& ref) {
    for(const auto& c:v) if(c.posOffset==ref.posOffset && c.posEncoding==ref.posEncoding &&
                             c.things.offset==ref.things.offset && c.things.elemSize==ref.things.elemSize)
        return &c;
    return nullptr;
}

void V27PrintTileCandidate(const Report& r,const V27TileCandidate& c,const char* tag) {
    std::cout << tag << " tile=" << HexU32(c.tile)
              << " pos+" << HexU32(c.posOffset) << " " << c.posEncoding
              << " things=" << c.things.kind << "+" << HexU32(c.things.offset)
              << " elem=" << c.things.elemSize << " count=" << c.things.count
              << " gold2148=" << c.things.goldCount
              << " semThings=" << (c.semanticThings?"YES":"NO")
              << " semPos=" << (c.semanticPosition?"YES":"NO")
              << " exactTileRTTI=" << (c.exactTileRtti?"YES":"NO")
              << " score=" << c.score << "\n";
}

const V26ThingCollection* V26FindSameCollection(const std::vector<V26ThingCollection>& v,const V26ThingCollection& ref) {
    for(const auto& c:v) if(c.kind==ref.kind && c.offset==ref.offset && c.elemSize==ref.elemSize) return &c;
    return nullptr;
}

void V26PrintThings(const Report& r,const V26ThingCollection& c,const char* prefix) {
    std::cout << prefix << " " << c.kind << " +" << HexU32(c.offset) << " elem=" << c.elemSize
              << " count=" << c.count << " gold2148=" << c.goldCount << " semantic=" << (c.semantic?"YES":"NO") << "\n";
    for(size_t i=0;i<c.things.size() && i<12;++i) {
        uint32_t p=c.things[i];
        std::string cls="Thing";
        for(const char* k:{"Item","LocalPlayer","Player","Monster","Npc"}) if(IsExactRttiObjectV19(r,p,k)) { cls=k; break; }
        std::cout << "    [" << i << "] ptr=" << HexU32(p) << " RTTI=" << cls;
        if(cls=="Item") {
            const int id=V26ClientItemId(r,p); uint16_t ct=0;
            auto ic=r.selectedFields.find("itemCount");
            if(ic!=r.selectedFields.end() && ic->second.offset>=0 && ic->second.type=="u16") SafeRead(static_cast<uintptr_t>(p)+ic->second.offset,ct);
            if(id>=0) std::cout << " id=" << id << " count=" << ct;
        }
        std::cout << "\n";
    }
}

void RunTileGroundDiscoveryV26(Report& r) {
    std::cout << "\n============================================================\n"
              << "          TILE / GROUND THINGS V27 SEMANTIC-PARENT A->B->A\n"
              << "============================================================\n"
              << "[METHOD] current-build getThings/getPosition + LocalPlayer membership + reversible Gold Coin 2148 transition.\n"
              << "[RULE] exact RTTI Tile is bonus evidence only; it is no longer required as the live-object locator.\n"
              << "[MODE] READ-ONLY: nenhuma escrita, hook ou chamada interna.\n";
    if(!r.localPlayerPtr || r.positionRuntime.x<0 || r.positionRuntime.y<0 || r.positionRuntime.z<0) {
        std::cout << "[TILE] LocalPlayer/Position indisponivel.\n"; return;
    }
    uint32_t x=0,y=0; uint8_t z=0;
    if(!QuickCheckPosition(r.localPlayerPtr,r.positionRuntime,&x,&y,&z)) { std::cout << "[TILE] Position quick-check falhou.\n"; return; }

    const auto thingSem=V27ThingSemanticOffsets(r);
    const auto posSem=V27PositionSemanticOffsets(r);
    std::cout << "[TILE SEMANTIC this+ THINGS]"; for(int v:thingSem) std::cout << " +" << HexU32(v); if(thingSem.empty()) std::cout << " <none>"; std::cout << "\n";
    std::cout << "[TILE SEMANTIC this+ POSITION]"; for(int v:posSem) std::cout << " +" << HexU32(v); if(posSem.empty()) std::cout << " <none>"; std::cout << "\n";

    auto tp=r.selectedFields.find("tilePosition");
    auto tt=r.selectedFields.find("tileThings");
    int forcedElem=-1;
    auto te=r.selectedFields.find("tileThingsElem");
    if(te!=r.selectedFields.end() && te->second.offset>0) forcedElem=te->second.offset;
    if(tp!=r.selectedFields.end() && tt!=r.selectedFields.end() && tp->second.offset>=0 && tt->second.offset>=0 &&
       EvidenceScore(tp->second.status)>=80 && EvidenceScore(tt->second.status)>=80) {
        auto quick=V27LocateCurrentTileCandidates(r,x,y,z,tt->second.offset,tp->second.offset,forcedElem);
        std::cout << "[TILE QUICK-CHECK] profile pos+" << HexU32(tp->second.offset)
                  << " things+" << HexU32(tt->second.offset) << " candidates=" << quick.size() << "\n";
        for(const auto& c:quick) if(c.posOffset==tp->second.offset && c.things.offset==tt->second.offset) {
            V27PrintTileCandidate(r,c,"[TILE QUICK]"); V26PrintThings(r,c.things,"[TILE THINGS QUICK]"); return;
        }
        std::cout << "[TILE QUICK-CHECK] profile nao reproduzido; mantendo profile e permitindo nova validacao.\n";
    }

    if(AskYesNo("Mapear Tile/Things no SQM atual com GOLD COIN 2148: sem coin -> drop -> pickup? [S/N]")!='s') {
        std::cout << "[TILE] teste pulado.\n"; return;
    }
    WaitEnter("[A] Fique PARADO num SQM SEM gold coin 2148 no chao. ENTER... ");
    if(!QuickCheckPosition(r.localPlayerPtr,r.positionRuntime,&x,&y,&z)) return;
    auto A=V27LocateCurrentTileCandidates(r,x,y,z);
    std::cout << "[TILE PARENT CANDIDATES A] " << A.size() << " at " << x << "," << y << "," << (int)z << "\n";
    for(size_t i=0;i<A.size() && i<12;++i) V27PrintTileCandidate(r,A[i],"  [A]");
    std::vector<V27TileCandidate> base;
    for(const auto& c:A) if(c.things.goldCount==0) base.push_back(c);
    if(base.empty()) { std::cout << "[TILE] nenhum parent semantic com LocalPlayer + Position atual e gold=0.\n"; return; }

    WaitEnter("[B] SEM ANDAR, jogue 1 Gold Coin no MESMO SQM. ENTER... ");
    uint32_t bx=0,by=0; uint8_t bz=0;
    if(!QuickCheckPosition(r.localPlayerPtr,r.positionRuntime,&bx,&by,&bz) || bx!=x || by!=y || bz!=z) {
        std::cout << "[TILE] Position mudou entre A e B; teste cancelado.\n"; return;
    }
    auto B=V27LocateCurrentTileCandidates(r,x,y,z);
    struct Hit { V27TileCandidate a,b; int score=0; };
    std::vector<Hit> hits;
    for(const auto& a:base) {
        const auto* b=V27FindLayoutCandidate(B,a); if(!b) continue;
        if(a.things.goldCount==0 && b->things.goldCount>=1) {
            Hit h{a,*b,a.score+b->score+3500};
            if(a.tile==b->tile) h.score+=700;
            hits.push_back(h);
        }
    }
    std::sort(hits.begin(),hits.end(),[](const auto& a,const auto& b){return a.score>b.score;});
    std::cout << "[TILE A->B GOLD TRANSITION HITS] " << hits.size() << "\n";
    for(size_t i=0;i<hits.size() && i<10;++i) {
        V27PrintTileCandidate(r,hits[i].b,"  [B HIT]");
        std::cout << "      gold " << hits[i].a.things.goldCount << "->" << hits[i].b.things.goldCount
                  << " count " << hits[i].a.things.count << "->" << hits[i].b.things.count
                  << " sameTile=" << (hits[i].a.tile==hits[i].b.tile?"YES":"NO") << " totalScore=" << hits[i].score << "\n";
    }
    if(hits.empty()) { std::cout << "[TILE] nenhum layout semantic apresentou Gold 0->1.\n"; return; }

    WaitEnter("[C] Pegue a Gold Coin de volta; continue SEM ANDAR. ENTER... ");
    uint32_t cx=0,cy=0; uint8_t cz=0;
    if(!QuickCheckPosition(r.localPlayerPtr,r.positionRuntime,&cx,&cy,&cz) || cx!=x || cy!=y || cz!=z) {
        std::cout << "[TILE] Position mudou entre B e C; teste cancelado.\n"; return;
    }
    auto C=V27LocateCurrentTileCandidates(r,x,y,z);
    std::vector<Hit> confirmed;
    for(auto h:hits) {
        const auto* c=V27FindLayoutCandidate(C,h.a); if(!c) continue;
        if(c->things.goldCount==0) {
            h.score+=4000;
            if(c->tile==h.a.tile || c->tile==h.b.tile) h.score+=500;
            confirmed.push_back(h);
        }
    }
    std::sort(confirmed.begin(),confirmed.end(),[](const auto& a,const auto& b){return a.score>b.score;});
    std::cout << "[TILE A->B->A CONFIRMED] " << confirmed.size() << "\n";
    for(size_t i=0;i<confirmed.size() && i<8;++i) V27PrintTileCandidate(r,confirmed[i].b,"  [CONFIRMED]");
    if(confirmed.empty()) { std::cout << "[TILE] nenhum candidato sobreviveu 0->1->0.\n"; return; }

    const auto& best=confirmed[0];
    const bool unique=confirmed.size()==1 || best.score>=confirmed[1].score+500 ||
                      (best.b.posOffset==confirmed[1].b.posOffset && best.b.things.offset==confirmed[1].b.things.offset && best.b.things.elemSize==confirmed[1].b.things.elemSize);
    const bool strong=best.b.semanticThings && (best.b.semanticPosition || best.b.exactTileRtti);
    if(!unique || !strong) {
        std::cout << "[TILE] transicao fechou, mas evidencia semantica ainda nao e unica/forte; nenhum offset promovido.\n"; return;
    }

    r.selectedFields["tilePosition"]={best.b.posEncoding,best.b.posOffset,"runtime_tile_position_semantic_parent_lp_gold_aba_v27"};
    r.selectedFields["tileThings"]={best.b.things.kind+"_thingptr",best.b.things.offset,"runtime_tile_things_semantic_parent_lp_gold2148_aba_v27"};
    r.selectedFields["tileThingsElem"]={"bytes",best.b.things.elemSize,"runtime_tile_things_semantic_parent_lp_gold2148_aba_v27"};
    std::cout << "[TILE POSITION] CONFIRMADO +" << HexU32(best.b.posOffset) << " " << best.b.posEncoding << "\n";
    std::cout << "[TILE THINGS] CONFIRMADO " << best.b.things.kind << " +" << HexU32(best.b.things.offset)
              << " elem=" << best.b.things.elemSize << " por getThings/getPosition + LocalPlayer membership + Gold 2148 0->1->0.\n";
    std::cout << "[TILE RTTI NOTE] exactTileRTTI=" << (best.b.exactTileRtti?"YES":"NO")
              << "; V27 nao depende dele para localizar o objeto vivo.\n";
    V26PrintThings(r,best.b.things,"[TILE B THINGS]");
    SaveCheckpoint(r,"tile_position_things_semantic_parent_gold2148_aba_v27");
}




// ============================================================================
// V30 - TILE / GROUND THINGS: POSITION-NEEDLE + LOOSE RTTI + A->B->A
// READ-ONLY. No internal calls. V27 failed because it required the current
// Tile parent to be reconstructed through an exact LocalPlayer pointer inside
// a strict Thing vector. V30 instead starts from the already-confirmed current
// Position bytes in live memory, derives object starts, resolves MSVC RTTI by
// name (not one exact vtable), accepts vector/deque Thing handles, and then
// requires Gold Coin ClientThingId 2148 to do 0 -> 1 -> 0 on the same layout.
// ============================================================================
struct V30ThingCollection {
    std::string kind;
    int offset=-1;
    int elemSize=0;
    int count=0;
    int goldCount=0;
    int knownThings=0;
    int unknownThings=0;
    int localPlayerHits=0;
    bool semantic=false;
    int score=0;
    std::vector<uint32_t> things;
};

struct V30TileCandidate {
    uint32_t tile=0;
    int posOffset=-1;
    std::string posEncoding;
    V30ThingCollection things;
    std::string rttiName;
    bool tileRttiName=false;
    bool exactTileRtti=false;
    bool semanticThings=false;
    bool semanticPosition=false;
    int score=0;
};

std::string V30RttiNameOfObject(const Report& r,uint32_t obj) {
    if(!obj) return {};
    uint32_t vt=0,col=0,td=0;
    if(!SafeRead(static_cast<uintptr_t>(obj),vt) || !r.module.inModule(vt) || vt<4) return {};
    if(!SafeRead(static_cast<uintptr_t>(vt)-4,col) || !r.module.inModule(col)) return {};
    // MSVC x86 CompleteObjectLocator: pTypeDescriptor at +0x0C (absolute VA).
    if(!SafeRead(static_cast<uintptr_t>(col)+0x0C,td) || !r.module.inModule(td)) return {};
    char name[128]{};
    if(!SafeCopy(static_cast<uintptr_t>(td)+8,name,sizeof(name)-1)) return {};
    name[sizeof(name)-1]=0;
    size_t n=0; while(n<sizeof(name)-1 && name[n] && static_cast<unsigned char>(name[n])>=0x20 && static_cast<unsigned char>(name[n])<0x7F) ++n;
    if(n<3) return {};
    return std::string(name,n);
}

bool V30HasModuleVptr(const Report& r,uint32_t obj) {
    uint32_t vt=0;
    return obj && SafeRead(static_cast<uintptr_t>(obj),vt) && r.module.inModule(vt);
}

bool V30NameHas(const std::string& n,const char* x) {
    return !n.empty() && n.find(x)!=std::string::npos;
}

bool V30LooksThingName(const std::string& n) {
    for(const char* k:{"Item","Thing","Creature","LocalPlayer","Player","Monster","Npc","Effect","Missile","AnimatedText"})
        if(V30NameHas(n,k)) return true;
    return false;
}

int V30ClientItemIdLoose(const Report& r,uint32_t p) {
    if(!p || !V30HasModuleVptr(r,p)) return -1;
    const std::string n=V30RttiNameOfObject(r,p);
    if(!V30NameHas(n,"Item") && !IsExactRttiObjectV19(r,p,"Item")) return -1;
    auto it=r.selectedFields.find("itemId");
    if(it==r.selectedFields.end() || it->second.offset<0) return -1;
    if(it->second.type=="u16") { uint16_t v=0; if(SafeRead(static_cast<uintptr_t>(p)+it->second.offset,v)) return static_cast<int>(v); }
    if(it->second.type=="u32") { uint32_t v=0; if(SafeRead(static_cast<uintptr_t>(p)+it->second.offset,v)) return static_cast<int>(v); }
    return -1;
}

bool V30AcceptThingPtr(const Report& r,uint32_t p,int& known,bool& isLp,int& itemId) {
    known=0; isLp=false; itemId=-1;
    if(!p || !V24LooksHeapPointer(p) || !V30HasModuleVptr(r,p)) return false;
    if(p==static_cast<uint32_t>(r.localPlayerPtr)) { known=1; isLp=true; }
    const std::string n=V30RttiNameOfObject(r,p);
    if(V30LooksThingName(n)) known=1;
    itemId=V30ClientItemIdLoose(r,p);
    if(itemId>=0) known=1;
    // Unknown module-vtable objects are tolerated in small number; this covers
    // decorative/effect Things without turning the whole collection invalid.
    return true;
}

bool V30ParseVectorThings(const Report& r,uint32_t obj,int off,int elem,V30ThingCollection& c) {
    uint32_t begin=0,end=0,cap=0;
    if(!SafeRead(static_cast<uintptr_t>(obj)+off,begin) ||
       !SafeRead(static_cast<uintptr_t>(obj)+off+4,end) ||
       !SafeRead(static_cast<uintptr_t>(obj)+off+8,cap)) return false;
    if(!begin || end<begin || cap<end || !V24LooksHeapPointer(begin) || elem<4) return false;
    const uint32_t bytes=end-begin, capBytes=cap-begin;
    if(bytes%static_cast<uint32_t>(elem) || capBytes%static_cast<uint32_t>(elem)) return false;
    const int n=static_cast<int>(bytes/elem), cn=static_cast<int>(capBytes/elem);
    if(n<1 || n>80 || cn<n || cn>160) return false;
    c={}; c.kind="vector"; c.offset=off; c.elemSize=elem; c.count=n;
    for(int i=0;i<n;++i) {
        uint32_t p=0; if(!SafeRead(static_cast<uintptr_t>(begin)+i*elem,p)) return false;
        int known=0,itemId=-1; bool isLp=false;
        if(!V30AcceptThingPtr(r,p,known,isLp,itemId)) return false;
        c.things.push_back(p); c.knownThings+=known; c.unknownThings+=(known?0:1); c.localPlayerHits+=(isLp?1:0);
        if(itemId==2148) ++c.goldCount;
    }
    if(c.knownThings<1) return false;
    if(c.unknownThings>std::max(2,n/2)) return false;
    return true;
}

bool V30ParseDequeThings(const Report& r,uint32_t obj,int off,int elem,V30ThingCollection& c) {
    uint32_t map=0,mapsize=0,myoff=0,mysize=0;
    if(!SafeRead(static_cast<uintptr_t>(obj)+off,map) ||
       !SafeRead(static_cast<uintptr_t>(obj)+off+4,mapsize) ||
       !SafeRead(static_cast<uintptr_t>(obj)+off+8,myoff) ||
       !SafeRead(static_cast<uintptr_t>(obj)+off+12,mysize)) return false;
    if(!map || !V24LooksHeapPointer(map) || mapsize==0 || mapsize>512 || mysize<1 || mysize>80 || elem<4) return false;
    const int block=elem<=1?16:(elem<=2?8:(elem<=4?4:(elem<=8?2:1)));
    c={}; c.kind="deque"; c.offset=off; c.elemSize=elem; c.count=static_cast<int>(mysize);
    for(uint32_t i=0;i<mysize;++i) {
        const uint32_t logical=myoff+i;
        uint32_t bi=logical/static_cast<uint32_t>(block);
        if((mapsize&(mapsize-1))==0) bi&=(mapsize-1); else bi%=mapsize;
        const uint32_t inner=logical%static_cast<uint32_t>(block);
        uint32_t blockPtr=0,p=0;
        if(!SafeRead(static_cast<uintptr_t>(map)+bi*4,blockPtr) || !blockPtr || !V24LooksHeapPointer(blockPtr)) return false;
        if(!SafeRead(static_cast<uintptr_t>(blockPtr)+inner*elem,p)) return false;
        int known=0,itemId=-1; bool isLp=false;
        if(!V30AcceptThingPtr(r,p,known,isLp,itemId)) return false;
        c.things.push_back(p); c.knownThings+=known; c.unknownThings+=(known?0:1); c.localPlayerHits+=(isLp?1:0);
        if(itemId==2148) ++c.goldCount;
    }
    if(c.knownThings<1) return false;
    if(c.unknownThings>std::max(2,static_cast<int>(mysize)/2)) return false;
    return true;
}

std::set<int> V30EarlyThisOffsetsForToken(const Report& r,const std::string& token) {
    std::set<int> out;
    auto it=r.binds.find(token); if(it==r.binds.end()) return out;
    for(const auto& c:it->second.candidates) {
        uint8_t b[32]{}; if(!SafeCopy(r.module.base+c.methodRva,b,sizeof(b))) continue;
        size_t lim=sizeof(b);
        for(size_t k=0;k<lim;++k) if(b[k]==0xC3 || b[k]==0xC2) { lim=k+1; break; }
        for(size_t i=0;i+2<lim;++i) {
            int off=-1;
            const uint8_t op=b[i];
            if(op==0x8B || op==0x8D || op==0x8A || op==0x89 || op==0x88 || op==0x39 || op==0x3B) {
                if(V27DecodeThisDisp(b,lim,i+1,off) && off>=0 && off<=0x300) out.insert(off);
            } else if(op==0x0F && i+2<lim && (b[i+1]==0xB6 || b[i+1]==0xB7 || b[i+1]==0xBE || b[i+1]==0xBF)) {
                if(V27DecodeThisDisp(b,lim,i+2,off) && off>=0 && off<=0x300) out.insert(off);
            }
        }
    }
    return out;
}

std::set<int> V30ThingSemanticOffsets(const Report& r) {
    std::set<int> out;
    for(const char* tok:{"getThings","getThingCount","getGround","getTopThing","getTopLookThing","getTopUseThing","getTopMoveThing","getTopCreature"}) {
        auto x=V30EarlyThisOffsetsForToken(r,tok); out.insert(x.begin(),x.end());
    }
    return out;
}

std::set<int> V30PositionSemanticOffsets(const Report& r) {
    return V30EarlyThisOffsetsForToken(r,"getPosition");
}

struct V30PosHit { uint32_t addr=0; std::string enc; };
std::vector<V30PosHit> V30FindPositionNeedles(uint32_t x,uint32_t y,uint8_t z) {
    std::vector<V30PosHit> out;
    std::set<std::pair<uint32_t,std::string>> seen;
    for(uint32_t a:V27ScanDwordValue(x,30000)) {
        uint32_t yy=0,zz=0;
        if(SafeRead(static_cast<uintptr_t>(a)+4,yy) && SafeRead(static_cast<uintptr_t>(a)+8,zz) && yy==y && zz==z) {
            if(seen.insert({a,"u32,u32,u32"}).second) out.push_back({a,"u32,u32,u32"});
        }
    }
    const uint32_t packed=(static_cast<uint32_t>(static_cast<uint16_t>(x)) |
                           (static_cast<uint32_t>(static_cast<uint16_t>(y))<<16));
    for(uint32_t a:V27ScanDwordValue(packed,30000)) {
        uint8_t zz=0;
        if(SafeRead(static_cast<uintptr_t>(a)+4,zz) && zz==z) {
            if(seen.insert({a,"u16,u16,u8"}).second) out.push_back({a,"u16,u16,u8"});
        }
    }
    return out;
}

std::vector<V30ThingCollection> V30FindThingCollections(const Report& r,uint32_t tile,const std::set<int>& sem,int forcedOff=-1,int forcedElem=-1) {
    std::vector<V30ThingCollection> out;
    std::vector<int> offs;
    if(forcedOff>=0) offs.push_back(forcedOff);
    else {
        for(int x:sem) if(x>=4 && x<=0x180 && (x%4)==0) offs.push_back(x);
        for(int x=4;x<=0x180;x+=4) if(std::find(offs.begin(),offs.end(),x)==offs.end()) offs.push_back(x);
    }
    for(int off:offs) for(int elem:{4,8}) {
        if(forcedElem>0 && elem!=forcedElem) continue;
        for(int k=0;k<2;++k) {
            V30ThingCollection c;
            const bool ok=(k==0)?V30ParseVectorThings(r,tile,off,elem,c):V30ParseDequeThings(r,tile,off,elem,c);
            if(!ok) continue;
            c.semantic=sem.count(off)!=0;
            c.score=900+c.count*35+c.knownThings*60-c.unknownThings*80+(c.semantic?1800:0)+(c.localPlayerHits?500:0)+(elem==4?80:0);
            bool dup=false;
            for(auto& e:out) if(e.kind==c.kind && e.offset==c.offset && e.elemSize==c.elemSize) { if(c.score>e.score) e=c; dup=true; break; }
            if(!dup) out.push_back(c);
        }
    }
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){ if(a.score!=b.score) return a.score>b.score; if(a.offset!=b.offset) return a.offset<b.offset; return a.elemSize<b.elemSize; });
    return out;
}

std::vector<V30TileCandidate> V30LocateTileByPositionNeedle(const Report& r,uint32_t x,uint32_t y,uint8_t z,
                                                             int forcedThings=-1,int forcedPos=-1,int forcedElem=-1) {
    std::vector<V30TileCandidate> out;
    const auto posSem=V30PositionSemanticOffsets(r);
    const auto thingSem=V30ThingSemanticOffsets(r);
    const auto hits=V30FindPositionNeedles(x,y,z);
    std::set<std::tuple<uint32_t,int,int,int,std::string,std::string>> seen;

    for(const auto& h:hits) {
        std::vector<int> poffs;
        if(forcedPos>=0) poffs.push_back(forcedPos);
        else {
            for(int o:posSem) if(o>=4 && o<=0x180) poffs.push_back(o);
            const int step=(h.enc=="u16,u16,u8")?2:4;
            for(int o=4;o<=0x180;o+=step) if(std::find(poffs.begin(),poffs.end(),o)==poffs.end()) poffs.push_back(o);
        }
        for(int poff:poffs) {
            if(h.addr<=static_cast<uint32_t>(poff)) continue;
            const uint32_t owner=h.addr-static_cast<uint32_t>(poff);
            if(owner<0x10000u || !V24LooksHeapPointer(owner) || !V30HasModuleVptr(r,owner)) continue;
            std::string enc;
            if(!V27ReadPositionAt(owner,poff,x,y,z,enc) || enc!=h.enc) continue;
            const std::string rn=V30RttiNameOfObject(r,owner);
            const bool tileName=V30NameHas(rn,"Tile") && rn.find("_Ref_count")==std::string::npos;
            const auto cols=V30FindThingCollections(r,owner,thingSem,forcedThings,forcedElem);
            for(const auto& col:cols) {
                V30TileCandidate c; c.tile=owner; c.posOffset=poff; c.posEncoding=enc; c.things=col; c.rttiName=rn;
                c.tileRttiName=tileName; c.exactTileRtti=IsExactRttiObjectV19(r,owner,"Tile");
                c.semanticThings=thingSem.count(col.offset)!=0; c.semanticPosition=posSem.count(poff)!=0;
                c.score=2500+col.score+(c.tileRttiName?3000:0)+(c.exactTileRtti?900:0)+(c.semanticThings?1500:0)+(c.semanticPosition?1500:0);
                auto key=std::make_tuple(c.tile,c.posOffset,c.things.offset,c.things.elemSize,c.things.kind,c.posEncoding);
                if(seen.insert(key).second) out.push_back(c);
            }
        }
    }
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){ if(a.score!=b.score) return a.score>b.score; if(a.tile!=b.tile) return a.tile<b.tile; if(a.things.offset!=b.things.offset) return a.things.offset<b.things.offset; return a.posOffset<b.posOffset; });
    return out;
}

const V30TileCandidate* V30FindSameLayout(const std::vector<V30TileCandidate>& v,const V30TileCandidate& ref) {
    const V30TileCandidate* best=nullptr;
    for(const auto& c:v) {
        if(c.posOffset!=ref.posOffset || c.posEncoding!=ref.posEncoding || c.things.offset!=ref.things.offset ||
           c.things.elemSize!=ref.things.elemSize || c.things.kind!=ref.things.kind) continue;
        if(c.tile==ref.tile) return &c;
        if(!best || c.score>best->score) best=&c;
    }
    return best;
}

void V30PrintTileCandidate(const V30TileCandidate& c,const char* tag) {
    std::cout << tag << " tile=" << HexU32(c.tile)
              << " rtti=" << (c.rttiName.empty()?std::string("<unknown>"):c.rttiName)
              << " pos+" << HexU32(c.posOffset) << " " << c.posEncoding
              << " things=" << c.things.kind << "+" << HexU32(c.things.offset)
              << " elem=" << c.things.elemSize << " count=" << c.things.count
              << " gold2148=" << c.things.goldCount << " known=" << c.things.knownThings
              << " unknown=" << c.things.unknownThings << " lp=" << c.things.localPlayerHits
              << " semThings=" << (c.semanticThings?"YES":"NO")
              << " semPos=" << (c.semanticPosition?"YES":"NO")
              << " tileRTTIName=" << (c.tileRttiName?"YES":"NO")
              << " exactTileVTable=" << (c.exactTileRtti?"YES":"NO")
              << " score=" << c.score << "\n";
}

void V30PrintThings(const Report& r,const V30ThingCollection& c,const char* tag) {
    std::cout << tag << " " << c.kind << "+" << HexU32(c.offset) << " elem=" << c.elemSize
              << " count=" << c.count << " gold2148=" << c.goldCount << "\n";
    for(size_t i=0;i<c.things.size() && i<16;++i) {
        const uint32_t p=c.things[i]; const std::string rn=V30RttiNameOfObject(r,p); const int id=V30ClientItemIdLoose(r,p);
        std::cout << "    [" << i << "] ptr=" << HexU32(p) << " rtti=" << (rn.empty()?std::string("<unknown>"):rn);
        if(id>=0) {
            uint16_t count=0; auto ic=r.selectedFields.find("itemCount");
            if(ic!=r.selectedFields.end() && ic->second.offset>=0 && ic->second.type=="u16") SafeRead(static_cast<uintptr_t>(p)+ic->second.offset,count);
            std::cout << " clientId=" << id << " count=" << count;
        }
        std::cout << "\n";
    }
}


// ============================================================================
// V30 - TILE::THINGS DISCOVERY PORTED FROM THE WORKING x64 DLL STRATEGY
//
// The x64 builder does NOT require a Position field inside Tile. Its strong
// bootstrap is:
//   1) exact current-build Tile RTTI/vtable,
//   2) LocalPlayer* as a stable needle inside Tile::things,
//   3) capture current Tile on SQM A,
//   4) move to a different SQM on the same floor,
//   5) require the same collection layout/vtable on a DIFFERENT Tile object,
//   6) optionally strengthen/disambiguate with Gold Coin client id 2148
//      on the current SQM: 0 -> 1 -> 0.
//
// x86 adaptation:
//   - 4-byte pointers;
//   - Thing handles may be stride 4 (raw/intrusive pointer) or stride 8
//     ([object*][companion/control*]);
//   - no offset from x64 is copied;
//   - no internal game function is called.
// ============================================================================
struct V30X64TileCandidate {
    uint32_t tile = 0;
    uint32_t vtableRva = 0;
    V30ThingCollection things;
    int controlLike = 0;
    int score = 0;
};

struct V30X64Layout {
    std::string kind;
    int offset = -1;
    int elemSize = 0;
    uint32_t vtableRva = 0;
    int pairCount = 0;
    int score = 0;
    V30X64TileCandidate a;
    V30X64TileCandidate b;
};

bool V30LooksControlLikeName(const std::string& name) {
    const std::string n = Lower(name);
    return n.find("_ref_count") != std::string::npos ||
           n.find("ref_count") != std::string::npos ||
           n.find("control") != std::string::npos;
}

int V30CountControlLike(const Report& r,const V30ThingCollection& c) {
    int n=0;
    for(uint32_t p:c.things) {
        const std::string rn=V30RttiNameOfObject(r,p);
        if(V30LooksControlLikeName(rn)) ++n;
    }
    return n;
}

std::set<uint32_t> V30ExactTileVtables(const Report& r) {
    std::set<uint32_t> out;
    auto it=r.rtti.find("Tile");
    if(it==r.rtti.end()) return out;
    for(uint32_t rv:it->second.vtableRvas) {
        const uintptr_t va=r.module.base+rv;
        if(va<=0xFFFFFFFFu) out.insert(static_cast<uint32_t>(va));
    }
    return out;
}

uint32_t V30ThingCollectionHeaderBytes(const V30ThingCollection& c) {
    return c.kind=="deque" ? 16u : 12u;
}

uint32_t V30ThingCollectionHeaderAddress(const V30X64TileCandidate& c) {
    return c.tile + static_cast<uint32_t>(c.things.offset);
}

std::vector<V30X64TileCandidate>
V30CaptureCurrentTileByExactRttiLocalNeedle(const Report& r,
                                            int forcedOff=-1,
                                            int forcedElem=-1,
                                            const std::string& forcedKind={}) {
    std::vector<V30X64TileCandidate> out;
    const auto vtables=V30ExactTileVtables(r);
    if(vtables.empty()) {
        std::cout << "[V30 TILE RTTI] nenhuma vtable Tile exata na build atual.\n";
        return out;
    }

    auto refs=V27ScanDwordRefsToSet(vtables,50000);
    std::set<uint32_t> seenObjects;
    const auto semantic=V30ThingSemanticOffsets(r);
    size_t exactObjects=0, localOwners=0;
    size_t boundaryRejected=0, aliasesCollapsed=0;

    // Build a current-snapshot index of exact Tile objects grouped by their
    // exact vtable.  V29 exposed an important x86 aliasing case: when Tile
    // objects are laid out close together, scanning +0x180 from an earlier
    // object can walk into the *same* Tile::things header that belongs to the
    // next object.  Example observed at runtime:
    //   tileA+0x24 == tilePrev+0xB4 == tilePrevPrev+0x144.
    // V30 never assumes +0x24.  Instead it uses the next exact Tile object as
    // a structural boundary when one exists nearby, and later canonicalizes
    // candidates by absolute collection-header address.
    std::map<uint32_t,std::vector<uint32_t>> exactByVtable;
    for(const auto& ref:refs) {
        const uint32_t obj=ref.address;
        if(!IsExactRttiObjectV19(r,obj,"Tile")) continue;
        exactByVtable[ref.value].push_back(obj);
    }
    for(auto& kv:exactByVtable) {
        auto& v=kv.second;
        std::sort(v.begin(),v.end());
        v.erase(std::unique(v.begin(),v.end()),v.end());
    }

    for(const auto& ref:refs) {
        const uint32_t obj=ref.address;
        if(!seenObjects.insert(obj).second) continue;
        if(!IsExactRttiObjectV19(r,obj,"Tile")) continue;
        ++exactObjects;

        uint32_t nextExactTile=0;
        auto vit=exactByVtable.find(ref.value);
        if(vit!=exactByVtable.end()) {
            const auto& objs=vit->second;
            auto it=std::upper_bound(objs.begin(),objs.end(),obj);
            if(it!=objs.end()) {
                const uint32_t gap=*it-obj;
                // Nearby equal-vtable objects are strong evidence of an
                // object/array boundary.  Far-away objects are unrelated and
                // are intentionally ignored.
                if(gap>=0x20u && gap<=0x300u) nextExactTile=*it;
            }
        }

        auto cols=V30FindThingCollections(r,obj,semantic,forcedOff,forcedElem);
        for(auto c:cols) {
            if(!forcedKind.empty() && c.kind!=forcedKind) continue;
            if(c.localPlayerHits!=1) continue;

            if(nextExactTile) {
                const uint64_t headerBegin=static_cast<uint64_t>(obj)+static_cast<uint32_t>(c.offset);
                const uint64_t headerEnd=headerBegin+V30ThingCollectionHeaderBytes(c);
                if(headerBegin>=nextExactTile || headerEnd>nextExactTile) {
                    ++boundaryRejected;
                    continue;
                }
            }

            V30X64TileCandidate t;
            t.tile=obj;
            t.vtableRva=static_cast<uint32_t>(ref.value-r.module.base);
            t.things=c;
            t.controlLike=V30CountControlLike(r,c);

            // If stride=4 is merely walking [object*][control*] pairs, it will
            // expose many _Ref_count/control objects. Penalize that shape so
            // the stride=8 interpretation wins without assuming it a priori.
            int strideScore=0;
            if(c.elemSize==4) {
                if(t.controlLike==0) strideScore+=450;
                else strideScore-=t.controlLike*450;
            } else if(c.elemSize==8) {
                strideScore+=350;
            }

            t.score=5000+c.score+strideScore;
            if(c.semantic) t.score+=900;
            if(c.knownThings==c.count) t.score+=350;
            if(c.count<=16) t.score+=250;
            ++localOwners;
            out.push_back(std::move(t));
        }
    }

    // For the same Tile/header, if a stride-8 interpretation exists and the
    // stride-4 one clearly contains control-block objects, discard the latter.
    std::vector<V30X64TileCandidate> filtered;
    for(const auto& c:out) {
        bool reject=false;
        if(c.things.elemSize==4 && c.controlLike>0) {
            for(const auto& d:out) {
                if(d.tile==c.tile && d.things.offset==c.things.offset &&
                   d.things.kind==c.things.kind && d.things.elemSize==8 &&
                   d.things.localPlayerHits==1) {
                    reject=true;
                    break;
                }
            }
        }
        if(!reject) filtered.push_back(c);
    }
    out.swap(filtered);

    // Canonicalize aliases that still resolve to the exact same physical
    // collection header.  The owner closest to the header (smallest positive
    // member offset) wins.  This is structural ownership, not an offset
    // oracle, so no x64/x86 value is hard-coded here.
    using HeaderKey=std::tuple<uint32_t,std::string,int>;
    std::map<HeaderKey,V30X64TileCandidate> byHeader;
    for(const auto& c:out) {
        const uint32_t header=V30ThingCollectionHeaderAddress(c);
        HeaderKey k{header,c.things.kind,c.things.elemSize};
        auto it=byHeader.find(k);
        if(it==byHeader.end()) {
            byHeader.emplace(k,c);
            continue;
        }
        ++aliasesCollapsed;
        const auto& old=it->second;
        if(c.things.offset<old.things.offset ||
           (c.things.offset==old.things.offset && c.score>old.score)) {
            it->second=c;
        }
    }
    out.clear();
    out.reserve(byHeader.size());
    for(auto& kv:byHeader) out.push_back(std::move(kv.second));

    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){
        if(a.score!=b.score) return a.score>b.score;
        if(a.things.offset!=b.things.offset) return a.things.offset<b.things.offset;
        if(a.things.elemSize!=b.things.elemSize) return a.things.elemSize<b.things.elemSize;
        return a.tile<b.tile;
    });

    std::cout << "[V30 TILE RTTI SCAN] vtables=" << vtables.size()
              << " rawRefs=" << refs.size()
              << " exactTileObjects=" << exactObjects
              << " boundaryRejected=" << boundaryRejected
              << " aliasesCollapsed=" << aliasesCollapsed
              << " currentTileLayouts=" << out.size() << "\n";

    return out;
}

bool V30ReadCollectionAt(const Report& r,const V30X64TileCandidate& model,V30ThingCollection& out) {
    if(!model.tile || !IsExactRttiObjectV19(r,model.tile,"Tile")) return false;
    bool ok=false;
    if(model.things.kind=="vector")
        ok=V30ParseVectorThings(r,model.tile,model.things.offset,model.things.elemSize,out);
    else if(model.things.kind=="deque")
        ok=V30ParseDequeThings(r,model.tile,model.things.offset,model.things.elemSize,out);
    if(!ok || out.localPlayerHits!=1) return false;
    out.semantic=V30ThingSemanticOffsets(r).count(out.offset)!=0;
    out.score=900+out.count*35+out.knownThings*60-out.unknownThings*80+
              (out.semantic?1800:0)+(out.localPlayerHits?500:0)+(out.elemSize==4?80:0);
    return true;
}

void V30PrintExactTile(const V30X64TileCandidate& c,const char* tag) {
    std::cout << tag
              << " tile=" << HexU32(c.tile)
              << " vtRva=" << HexU32(c.vtableRva)
              << " things=" << c.things.kind << "+" << HexU32(c.things.offset)
              << " header=" << HexU32(V30ThingCollectionHeaderAddress(c))
              << " elem=" << c.things.elemSize
              << " count=" << c.things.count
              << " gold2148=" << c.things.goldCount
              << " known=" << c.things.knownThings
              << " unknown=" << c.things.unknownThings
              << " lp=" << c.things.localPlayerHits
              << " controlLike=" << c.controlLike
              << " semantic=" << (c.things.semantic?"YES":"NO")
              << " score=" << c.score << "\n";
}

std::vector<V30X64Layout>
V30CrossValidateTwoSqm(const std::vector<V30X64TileCandidate>& a,
                       const std::vector<V30X64TileCandidate>& b) {
    using Key=std::tuple<std::string,int,int,uint32_t>;
    std::map<Key,V30X64Layout> agg;

    for(const auto& x:a) for(const auto& y:b) {
        if(x.tile==y.tile) continue;
        if(x.vtableRva!=y.vtableRva ||
           x.things.kind!=y.things.kind ||
           x.things.offset!=y.things.offset ||
           x.things.elemSize!=y.things.elemSize) continue;

        Key k{x.things.kind,x.things.offset,x.things.elemSize,x.vtableRva};
        auto& z=agg[k];
        if(z.pairCount==0) {
            z.kind=x.things.kind;
            z.offset=x.things.offset;
            z.elemSize=x.things.elemSize;
            z.vtableRva=x.vtableRva;
            z.a=x;
            z.b=y;
        }
        ++z.pairCount;
        z.score+=x.score+y.score+2500;
        if(x.things.semantic && y.things.semantic) z.score+=1200;
        if(x.controlLike==0 && y.controlLike==0) z.score+=500;
    }

    std::vector<V30X64Layout> out;
    for(auto& kv:agg) out.push_back(kv.second);
    std::sort(out.begin(),out.end(),[](const auto& x,const auto& y){
        if(x.score!=y.score) return x.score>y.score;
        if(x.pairCount!=y.pairCount) return x.pairCount>y.pairCount;
        if(x.offset!=y.offset) return x.offset<y.offset;
        return x.elemSize<y.elemSize;
    });
    return out;
}

bool V30ReadCurrentPosition(const Report& r,uint32_t& x,uint32_t& y,uint8_t& z) {
    return r.localPlayerPtr &&
           r.positionRuntime.x>=0 && r.positionRuntime.y>=0 && r.positionRuntime.z>=0 &&
           QuickCheckPosition(r.localPlayerPtr,r.positionRuntime,&x,&y,&z);
}

void RunTileGroundDiscoveryV30(Report& r) {
    std::cout << "\n============================================================\n"
              << "      TILE::THINGS V30 - x64 TWO-SQM METHOD + GOLD XCHECK\n"
              << "============================================================\n"
              << "[METHOD] exact Tile RTTI/vtable + LocalPlayer* needle em Tile::things.\n"
              << "[METHOD] V30 canonicaliza pelo header absoluto e respeita a fronteira do proximo Tile RTTI.\n"
              << "[METHOD] SQM A -> SQM B no mesmo andar: exige MESMO layout em DOIS Tile objects diferentes.\n"
              << "[GOLD]   depois usa Gold Coin clientId 2148 no SQM B como prova reversivel 0->1->0.\n"
              << "[RULE]   aliases entre Tiles vizinhos sao rejeitados; nenhum offset e escolhido por menor valor fixo.\n"
              << "[RULE]   Tile.Position NAO e requisito; nenhum offset x64 e copiado.\n"
              << "[MODE]   READ-ONLY: nenhuma escrita, hook ou chamada interna.\n";

    uint32_t x=0,y=0; uint8_t z=0;
    if(!V30ReadCurrentPosition(r,x,y,z)) {
        std::cout << "[TILE] LocalPlayer/Position indisponivel.\n";
        return;
    }

    auto tt=r.selectedFields.find("tileThings");
    auto te=r.selectedFields.find("tileThingsElem");
    auto tv=r.selectedFields.find("tileVtableRva");
    if(tt!=r.selectedFields.end() && te!=r.selectedFields.end() && tv!=r.selectedFields.end() &&
       tt->second.offset>=0 && te->second.offset>0 && tv->second.offset>=0 &&
       EvidenceScore(tt->second.status)>=80 && EvidenceScore(tv->second.status)>=80) {
        std::string kind=tt->second.type;
        if(kind.find("deque")!=std::string::npos) kind="deque";
        else kind="vector";
        auto q=V30CaptureCurrentTileByExactRttiLocalNeedle(r,tt->second.offset,te->second.offset,kind);
        std::cout << "[TILE QUICK-CHECK] stored things+" << HexU32(tt->second.offset)
                  << " elem=" << te->second.offset
                  << " vtRva=" << HexU32(tv->second.offset)
                  << " candidates=" << q.size() << "\n";
        for(const auto& c:q) {
            if(c.vtableRva==static_cast<uint32_t>(tv->second.offset)) {
                V30PrintExactTile(c,"  [TILE QUICK]");
                return;
            }
        }
        std::cout << "[TILE QUICK-CHECK] profile nao reproduzido; permitindo redescoberta.\n";
    }

    if(AskYesNo("Executar Tile::things no estilo do builder x64 (SQM A -> SQM B) e validar com Gold 2148?")!='s') {
        std::cout << "[TILE] teste pulado.\n";
        return;
    }

    // --------------------------------------------------------
    // Snapshot A
    // --------------------------------------------------------
    WaitEnter("[A] Fique PARADO em um SQM e pressione ENTER... ");
    uint32_t ax=0,ay=0; uint8_t az=0;
    if(!V30ReadCurrentPosition(r,ax,ay,az)) return;
    auto A=V30CaptureCurrentTileByExactRttiLocalNeedle(r);
    uint32_t ax2=0,ay2=0; uint8_t az2=0;
    if(!V30ReadCurrentPosition(r,ax2,ay2,az2) || ax!=ax2 || ay!=ay2 || az!=az2) {
        std::cout << "[TILE] voce se moveu durante o scan A; snapshot descartado.\n";
        return;
    }
    std::cout << "[SQM A] pos=" << ax << "," << ay << "," << static_cast<int>(az)
              << " candidates=" << A.size() << "\n";
    for(size_t i=0;i<A.size() && i<16;++i) V30PrintExactTile(A[i],"  [A]");
    if(A.empty()) {
        std::cout << "[TILE] nenhum Tile RTTI exato com LocalPlayer* dentro de uma colecao valida em A.\n";
        return;
    }

    // --------------------------------------------------------
    // Move to B, same floor.
    // --------------------------------------------------------
    uint32_t bx=0,by=0; uint8_t bz=0;
    for(;;) {
        WaitEnter("[B] Ande para OUTRO SQM no MESMO andar, pare completamente e pressione ENTER... ");
        if(!V30ReadCurrentPosition(r,bx,by,bz)) return;
        if(bz!=az) {
            std::cout << "[RETRY] mudou de andar: A z=" << static_cast<int>(az)
                      << " B z=" << static_cast<int>(bz) << ". Volte ao mesmo andar.\n";
            continue;
        }
        if(bx==ax && by==ay) {
            std::cout << "[RETRY] ainda esta no SQM A.\n";
            continue;
        }
        break;
    }

    auto B=V30CaptureCurrentTileByExactRttiLocalNeedle(r);
    uint32_t bx2=0,by2=0; uint8_t bz2=0;
    if(!V30ReadCurrentPosition(r,bx2,by2,bz2) || bx!=bx2 || by!=by2 || bz!=bz2) {
        std::cout << "[TILE] voce se moveu durante o scan B; snapshot descartado.\n";
        return;
    }
    std::cout << "[SQM B] pos=" << bx << "," << by << "," << static_cast<int>(bz)
              << " candidates=" << B.size() << "\n";
    for(size_t i=0;i<B.size() && i<16;++i) V30PrintExactTile(B[i],"  [B]");
    if(B.empty()) {
        std::cout << "[TILE] nenhum Tile RTTI exato com LocalPlayer* dentro de uma colecao valida em B.\n";
        return;
    }

    auto layouts=V30CrossValidateTwoSqm(A,B);
    std::cout << "\n[TILE LAYOUT CROSS-VALIDATION A->B] " << layouts.size() << "\n";
    for(size_t i=0;i<layouts.size() && i<16;++i) {
        const auto& l=layouts[i];
        std::cout << "  #" << i
                  << " pairs=" << l.pairCount
                  << " score=" << l.score
                  << " vtRva=" << HexU32(l.vtableRva)
                  << " " << l.kind << "+" << HexU32(l.offset)
                  << " elem=" << l.elemSize
                  << " tileA=" << HexU32(l.a.tile)
                  << " tileB=" << HexU32(l.b.tile)
                  << "\n";
    }
    if(layouts.empty()) {
        std::cout << "[TILE] nenhum layout comum em dois Tile objects distintos. Nenhum offset promovido.\n";
        return;
    }

    // --------------------------------------------------------
    // Gold 2148 secondary cross-check on the current SQM B.
    // It is used as a discriminator/extra proof, not as the initial locator.
    // --------------------------------------------------------
    std::vector<size_t> goldCandidates;
    if(AskYesNo("Fazer agora a prova extra com Gold Coin 2148 no SQM B (0->1->0)?")=='s') {
        WaitEnter("[G0] Garanta que NAO existe Gold Coin 2148 no chao deste SQM B e pressione ENTER... ");

        struct GoldState { bool ok0=false,ok1=false,ok2=false; V30ThingCollection c0,c1,c2; };
        std::vector<GoldState> gs(layouts.size());

        for(size_t i=0;i<layouts.size();++i) {
            gs[i].ok0=V30ReadCollectionAt(r,layouts[i].b,gs[i].c0);
        }

        WaitEnter("[G1] SEM ANDAR, jogue 1 Gold Coin no MESMO SQM e pressione ENTER... ");
        uint32_t gx=0,gy=0; uint8_t gz=0;
        if(!V30ReadCurrentPosition(r,gx,gy,gz) || gx!=bx || gy!=by || gz!=bz) {
            std::cout << "[TILE] Position mudou durante Gold G0->G1; validacao cancelada.\n";
            return;
        }
        for(size_t i=0;i<layouts.size();++i) {
            gs[i].ok1=V30ReadCollectionAt(r,layouts[i].b,gs[i].c1);
        }

        WaitEnter("[G2] Pegue a Gold Coin de volta, continue SEM ANDAR e pressione ENTER... ");
        if(!V30ReadCurrentPosition(r,gx,gy,gz) || gx!=bx || gy!=by || gz!=bz) {
            std::cout << "[TILE] Position mudou durante Gold G1->G2; validacao cancelada.\n";
            return;
        }
        for(size_t i=0;i<layouts.size();++i) {
            gs[i].ok2=V30ReadCollectionAt(r,layouts[i].b,gs[i].c2);
            if(gs[i].ok0 && gs[i].ok1 && gs[i].ok2 &&
               gs[i].c0.goldCount==0 &&
               gs[i].c1.goldCount>=1 &&
               gs[i].c2.goldCount==0) {
                goldCandidates.push_back(i);
                layouts[i].score+=10000;
            }
        }

        std::cout << "[GOLD 2148 XCHECK] confirmed layouts=" << goldCandidates.size() << "\n";
        for(size_t idx:goldCandidates) {
            std::cout << "  #" << idx
                      << " " << layouts[idx].kind << "+" << HexU32(layouts[idx].offset)
                      << " elem=" << layouts[idx].elemSize
                      << " gold "
                      << gs[idx].c0.goldCount << "->"
                      << gs[idx].c1.goldCount << "->"
                      << gs[idx].c2.goldCount
                      << " count "
                      << gs[idx].c0.count << "->"
                      << gs[idx].c1.count << "->"
                      << gs[idx].c2.count << "\n";
        }

        std::sort(layouts.begin(),layouts.end(),[](const auto& a,const auto& b){
            if(a.score!=b.score) return a.score>b.score;
            if(a.pairCount!=b.pairCount) return a.pairCount>b.pairCount;
            return a.offset<b.offset;
        });
    }

    // Selection rule:
    // - gold confirmation, when present, must leave one clear best layout;
    // - otherwise the two-SQM exact-RTTI layout must itself be unique/clear.
    const V30X64Layout* best=nullptr;
    if(!goldCandidates.empty()) {
        // layouts were resorted; select the highest score that now includes the
        // gold bonus. A 10k bonus makes a gold-proven layout dominate.
        best=&layouts.front();
        if(layouts.size()>1 && layouts[0].score < layouts[1].score+800) {
            std::cout << "[TILE] mais de um layout permaneceu equivalente mesmo apos Gold; nenhum offset promovido.\n";
            return;
        }
    } else {
        best=&layouts.front();
        if(layouts.size()>1 && layouts[0].score < layouts[1].score+1200) {
            std::cout << "[TILE] dois-SQM fechou, mas mais de um layout ficou equivalente; use Gold para desempatar.\n";
            return;
        }
    }

    if(!best || best->offset<0 || best->elemSize<=0) return;

    const std::string status = !goldCandidates.empty()
        ? "runtime_tile_exact_rtti_localplayer_two_sqm_gold2148_aba_canonical_v30"
        : "runtime_tile_exact_rtti_localplayer_two_sqm_canonical_v30";

    r.selectedFields.erase("tilePosition"); // x64-style discovery does not require a Tile position member.
    r.selectedFields["tileThings"]={best->kind+"_thing_handle",best->offset,status};
    r.selectedFields["tileThingsEnd"]={"offset",best->offset+4,status};
    r.selectedFields["tileThingsCap"]={"offset",best->offset+8,status};
    r.selectedFields["tileThingsElem"]={"bytes",best->elemSize,status};
    r.selectedFields["tileVtableRva"]={"rva",static_cast<int>(best->vtableRva),status};

    std::cout << "\n============================================================\n"
              << "               TILE::THINGS CONFIRMADO V30\n"
              << "============================================================\n"
              << "Tile VTable RVA     : " << HexU32(best->vtableRva) << "\n"
              << "Things begin/header : +" << HexU32(best->offset) << "\n"
              << "Things end          : +" << HexU32(best->offset+4) << "\n"
              << "Things cap          : +" << HexU32(best->offset+8) << "\n"
              << "Thing element stride: " << best->elemSize << "\n"
              << "Storage             : " << best->kind << "\n"
              << "Evidence            : exact Tile RTTI + LocalPlayer* em dois SQMs/distinct Tile objects"
              << (!goldCandidates.empty() ? " + Gold 2148 0->1->0" : "")
              << "\n"
              << "Tile Position field : NOT REQUIRED para esta etapa\n";

    SaveCheckpoint(r,"tile_things_x64style_two_sqm_gold_canonical_v30");
}



// ============================================================================
// V32 - GAME ACTION ABI STATIC PROBE (x86)
// READ-ONLY. No action is executed. This pass inspects only the machine code
// selected by the current-build semantic bind for Game actions and reports
// stack/frame references, RET cleanup, this-field references and direct CALLs.
// It is intentionally diagnostic: no calling convention is promoted merely
// from a token name or an RVA.
// ============================================================================
struct V32AbiInfo {
    bool ebpFrame=false;
    int retOffset=-1;
    int retCleanup=-1; // -1 unknown/no RET seen, 0 RET, >0 RET imm16
    std::set<int> ebpRefs;
    std::set<int> espRefs;
    std::set<int> thisOffsets;
    std::vector<uint32_t> calls;
    int pushCount=0;
};

void V32CollectBaseDisp(const uint8_t* b,size_t n,size_t modrmPos,std::set<int>& ebp,std::set<int>& esp) {
    if(modrmPos>=n) return;
    const uint8_t mr=b[modrmPos];
    const int mod=(mr>>6)&3, rm=mr&7;
    if(mod==3) return;
    size_t p=modrmPos+1;
    int base=rm;
    if(rm==4) {
        if(p>=n) return;
        const uint8_t sib=b[p++];
        base=sib&7;
    }
    int disp=0; bool have=false;
    if(mod==1) {
        if(p>=n) return;
        disp=static_cast<int8_t>(b[p]); have=true;
    } else if(mod==2 || (mod==0 && base==5)) {
        if(p+4>n) return;
        int32_t d=0; std::memcpy(&d,b+p,4); disp=d; have=true;
    }
    if(!have || disp<0 || disp>0x100) return;
    if(base==5 && mod!=0) ebp.insert(disp);
    if(base==4) esp.insert(disp);
}

V32AbiInfo V32AnalyzeAbi(const Report& r,uint32_t methodRva) {
    V32AbiInfo a;
    if(!methodRva || methodRva>=r.module.sizeOfImage) return a;
    uint8_t b[192]{};
    if(!SafeCopy(r.module.base+methodRva,b,sizeof(b))) return a;
    a.ebpFrame=(b[0]==0x55 && b[1]==0x8B && b[2]==0xEC);

    for(size_t i=0;i<sizeof(b);++i) {
        if(b[i]==0xC3) { a.retOffset=static_cast<int>(i); a.retCleanup=0; break; }
        if(b[i]==0xC2 && i+2<sizeof(b)) {
            uint16_t imm=0; std::memcpy(&imm,b+i+1,2);
            a.retOffset=static_cast<int>(i); a.retCleanup=static_cast<int>(imm); break;
        }
    }
    const size_t lim=a.retOffset>=0 ? std::min<size_t>(sizeof(b),static_cast<size_t>(a.retOffset+3)) : sizeof(b);
    for(size_t i=0;i<lim;++i) {
        const uint8_t op=b[i];
        if(op>=0x50 && op<=0x57) ++a.pushCount;
        if(op==0x68 && i+4<lim) ++a.pushCount;
        if(op==0x6A && i+1<lim) ++a.pushCount;
        if(op==0xFF && i+1<lim && ((b[i+1]>>3)&7)==6) ++a.pushCount;

        if(op==0xE8 && i+4<lim) {
            int32_t rel=0; std::memcpy(&rel,b+i+1,4);
            const int64_t dst=static_cast<int64_t>(methodRva)+static_cast<int64_t>(i)+5+rel;
            if(dst>=0 && dst<static_cast<int64_t>(r.module.sizeOfImage)) a.calls.push_back(static_cast<uint32_t>(dst));
        }

        // Common one-byte opcodes followed by ModRM. Byte-wise diagnostic scan;
        // duplicates/false positives are controlled by the small positive disp range.
        if(op==0x8B || op==0x8D || op==0x89 || op==0x8A || op==0x88 ||
           op==0x39 || op==0x3B || op==0x83 || op==0x81 || op==0xFF) {
            if(i+1<lim) V32CollectBaseDisp(b,lim,i+1,a.ebpRefs,a.espRefs);
            int off=-1;
            if(V27DecodeThisDisp(b,lim,i+1,off) && off>=0 && off<=0x400) a.thisOffsets.insert(off);
        } else if(op==0x0F && i+2<lim &&
                  (b[i+1]==0xB6 || b[i+1]==0xB7 || b[i+1]==0xBE || b[i+1]==0xBF)) {
            V32CollectBaseDisp(b,lim,i+2,a.ebpRefs,a.espRefs);
            int off=-1;
            if(V27DecodeThisDisp(b,lim,i+2,off) && off>=0 && off<=0x400) a.thisOffsets.insert(off);
        }
    }
    std::sort(a.calls.begin(),a.calls.end());
    a.calls.erase(std::unique(a.calls.begin(),a.calls.end()),a.calls.end());
    return a;
}

void V32DumpCode(const Report& r,uint32_t methodRva,int stopOffset) {
    uint8_t b[96]{};
    if(!SafeCopy(r.module.base+methodRva,b,sizeof(b))) return;
    size_t n=sizeof(b);
    if(stopOffset>=0) n=std::min(n,static_cast<size_t>(stopOffset+3));
    std::cout << "    code:";
    for(size_t i=0;i<n;++i) {
        if(i && (i%16)==0) std::cout << "\n         ";
        std::cout << " " << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(b[i]) << std::dec << std::setfill(' ');
    }
    std::cout << "\n";
}

void V32DumpActionAbi(const Report& r,const std::string& token) {
    auto it=r.binds.find(token);
    std::cout << "\n[ACTION ABI] " << token << "\n";
    if(it==r.binds.end() || !it->second.selectedOk) {
        std::cout << "    <not found>\n";
        return;
    }
    const auto& sel=it->second.selected;
    const V32AbiInfo a=V32AnalyzeAbi(r,sel.methodRva);
    std::cout << "    methodRva=" << HexU32(sel.methodRva)
              << " ownerRva=" << HexU32(sel.ownerRva)
              << " ownerSection=" << sel.ownerSection
              << " candidates=" << it->second.candidates.size() << "\n"
              << "    frame=" << (a.ebpFrame?"EBP":"no canonical EBP prologue")
              << " ret=";
    if(a.retOffset<0) std::cout << "<not seen in first 192 bytes>";
    else if(a.retCleanup==0) std::cout << "RET (caller cleanup / thiscall-like possible)";
    else std::cout << "RET " << a.retCleanup << " bytes (callee cleanup)";
    std::cout << " at +" << (a.retOffset>=0?HexU32(static_cast<uint32_t>(a.retOffset)):std::string("?")) << "\n";

    std::cout << "    EBP positive refs:";
    if(a.ebpRefs.empty()) std::cout << " <none>";
    for(int x:a.ebpRefs) std::cout << " +" << HexU32(static_cast<uint32_t>(x));
    std::cout << "\n    ESP positive refs:";
    if(a.espRefs.empty()) std::cout << " <none>";
    for(int x:a.espRefs) std::cout << " +" << HexU32(static_cast<uint32_t>(x));
    std::cout << "\n    this/ECX field refs:";
    if(a.thisOffsets.empty()) std::cout << " <none>";
    for(int x:a.thisOffsets) std::cout << " +" << HexU32(static_cast<uint32_t>(x));
    std::cout << "\n    direct rel32 CALLs:";
    if(a.calls.empty()) std::cout << " <none>";
    for(uint32_t x:a.calls) std::cout << " " << HexU32(x);
    std::cout << "\n    PUSH-like instructions observed=" << a.pushCount << "\n";
    V32DumpCode(r,sel.methodRva,a.retOffset);
}

void RunGameActionAbiProbeV32(Report& r) {
    std::cout << "\n============================================================\n"
              << "             GAME ACTION ABI STATIC PROBE V32\n"
              << "============================================================\n"
              << "[MODE] READ-ONLY / NO ACTION CALLS / NO HOOKS / NO MEMORY WRITES\n"
              << "[RULE] RVAs are current-build semantic binds; ABI is reported, not assumed.\n";
    for(const char* tok:ramonia_baseline::abiMethods)
        V32DumpActionAbi(r,tok);
    std::cout << "\n[V32 NOTE] Envie este bloco inteiro. O proximo passo so chama uma acao depois de fecharmos assinatura/calling convention por evidencia.\n";
}


// ============================================================================
// V40 - AUTOWALK DEEP ABI DIAGNOSTIC
// READ-ONLY. The old V32 pass inspected only 192 bytes, while this build's
// autoWalk has branches beyond +0x300. V40 scans a bounded deeper window and
// reports stack-frame references and plausible RET epilogues without promoting
// any ABI from byte adjacency alone.
// ============================================================================
void RunAutoWalkDeepAbiProbeV40(const Report& r) {
    auto it=r.binds.find("autoWalk");
    if(it==r.binds.end() || !it->second.selectedOk) {
        std::cout << "\n[V40 AUTOWALK DEEP] <bind unavailable>\n";
        return;
    }

    const uint32_t methodRva=it->second.selected.methodRva;
    uint32_t nextRva=r.module.sizeOfImage;
    for(const auto& kv:r.binds) {
        if(!kv.second.selectedOk) continue;
        const uint32_t candidate=kv.second.selected.methodRva;
        if(candidate>methodRva && candidate<nextRva) nextRva=candidate;
    }

    const size_t maxAvailable =
        nextRva>methodRva ? static_cast<size_t>(nextRva-methodRva) : 0x1000u;
    const size_t scanSize=std::min<size_t>(0x1000u,std::max<size_t>(0x400u,maxAvailable));
    std::vector<uint8_t> b(scanSize);
    if(!SafeCopy(r.module.base+methodRva,b.data(),b.size())) {
        std::cout << "\n[V40 AUTOWALK DEEP] read failed\n";
        return;
    }

    std::map<int,int> ebpCounts;
    std::map<int,int> espCounts;
    struct RetCandidate { size_t off{}; int cleanup{}; };
    std::vector<RetCandidate> rets;

    for(size_t i=0;i<b.size();++i) {
        const uint8_t op=b[i];

        if(op==0xC2 && i+2<b.size()) {
            uint16_t imm=0;
            std::memcpy(&imm,b.data()+i+1,2);
            bool epilogue=false;
            const size_t begin=i>16?i-16:0;
            for(size_t j=begin;j<i;++j)
                if(b[j]==0x5D || b[j]==0xC9) { epilogue=true; break; }
            if(epilogue && imm<=0x40) rets.push_back({i,static_cast<int>(imm)});
        } else if(op==0xC3) {
            bool epilogue=false;
            const size_t begin=i>16?i-16:0;
            for(size_t j=begin;j<i;++j)
                if(b[j]==0x5D || b[j]==0xC9) { epilogue=true; break; }
            if(epilogue) rets.push_back({i,0});
        }

        size_t modrmPos=0;
        if(op==0x8B || op==0x8D || op==0x89 || op==0x8A || op==0x88 ||
           op==0x39 || op==0x3B || op==0x83 || op==0x81 || op==0xFF) {
            modrmPos=i+1;
        } else if(op==0x0F && i+2<b.size() &&
                  (b[i+1]==0xB6 || b[i+1]==0xB7 || b[i+1]==0xBE || b[i+1]==0xBF)) {
            modrmPos=i+2;
        }
        if(!modrmPos || modrmPos>=b.size()) continue;

        std::set<int> ebp,esp;
        V32CollectBaseDisp(b.data(),b.size(),modrmPos,ebp,esp);
        for(int d:ebp) if(d>=0x8 && d<=0x40) ++ebpCounts[d];
        for(int d:esp) if(d>=0 && d<=0x40) ++espCounts[d];
    }

    std::cout << "\n============================================================\n"
              << "          AUTOWALK DEEP ABI STATIC PROBE V40\n"
              << "============================================================\n"
              << "[MODE] READ-ONLY / scan=" << scanSize << " bytes\n"
              << "methodRva=" << HexU32(methodRva)
              << " nextSemanticRva=" << (nextRva<r.module.sizeOfImage?HexU32(nextRva):std::string("<none>"))
              << "\n";

    std::cout << "[V40 EBP REFS]";
    if(ebpCounts.empty()) std::cout << " <none>";
    for(const auto& kv:ebpCounts)
        std::cout << " +" << HexU32(static_cast<uint32_t>(kv.first))
                  << "(x" << kv.second << ")";
    std::cout << "\n[V40 ESP REFS]";
    if(espCounts.empty()) std::cout << " <none>";
    for(const auto& kv:espCounts)
        std::cout << " +" << HexU32(static_cast<uint32_t>(kv.first))
                  << "(x" << kv.second << ")";

    std::cout << "\n[V40 RET CANDIDATES]";
    if(rets.empty()) std::cout << " <none>";
    for(const auto& rc:rets)
        std::cout << " +" << HexU32(static_cast<uint32_t>(rc.off))
                  << (rc.cleanup?(" RET "+std::to_string(rc.cleanup)):std::string(" RET"));
    std::cout << "\n";

    for(const auto& rc:rets) {
        const size_t begin=rc.off>20?rc.off-20:0;
        const size_t end=std::min(b.size(),rc.off+4);
        std::cout << "  [RET CONTEXT +" << HexU32(static_cast<uint32_t>(rc.off)) << "]";
        for(size_t i=begin;i<end;++i)
            std::cout << " " << std::uppercase << std::hex << std::setw(2)
                      << std::setfill('0') << static_cast<unsigned>(b[i])
                      << std::dec << std::setfill(' ');
        std::cout << "\n";
    }
}


// ============================================================================
// V33 - ONE-SHOT RUNTIME ACTION ABI CAPTURE (x86)
//
// Evidence model:
//   * one selected current-build action method at a time;
//   * the first byte is temporarily replaced with INT3;
//   * when the game itself naturally calls the method, VEH captures the entry
//     context and restores the original byte BEFORE the original instruction
//     executes;
//   * no game method is invoked by this probe and no breakpoint is re-armed.
//
// This is deliberately a diagnostic stage. Captured values are printed but are
// not promoted to an active-call ABI until their object/reference semantics are
// unambiguous.
// ============================================================================
struct V33MemSnap {
    uint32_t address=0;
    uint32_t valid=0;
    uint8_t bytes[48]{};
};

struct V33Capture {
    volatile LONG hit=0;
    uint32_t methodRva=0;
    uint32_t methodAddress=0;
    uint32_t threadId=0;
    uint32_t eax=0, ebx=0, ecx=0, edx=0, esi=0, edi=0, ebp=0, esp=0, eflags=0;
    uint32_t stack[8]{}; // [0]=return address, then six+ args
    V33MemSnap ecxSnap{};
    V33MemSnap argSnap[6]{};
    V33MemSnap argNested0[6]{};
    V33MemSnap argNested1[6]{};
};

struct V33BreakpointState {
    uintptr_t address=0;
    uint8_t original=0;
    volatile LONG armed=0;
    volatile LONG protectionHeld=0;
    DWORD oldProtect=0;
};

V33Capture g_v33Capture{};
V33BreakpointState g_v33Bp{};
PVOID g_v33Veh=nullptr;

bool V33ReadProcessSafe(uintptr_t address, void* out, size_t size) {
    if(!address || !out || !size) return false;
    SIZE_T got=0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), out, size, &got) && got==size;
}

bool V33PrepareWritable(uintptr_t address) {
    if(!address) return false;
    DWORD oldProtect=0;
    if(!VirtualProtect(reinterpret_cast<void*>(address),1,PAGE_EXECUTE_READWRITE,&oldProtect)) return false;
    g_v33Bp.oldProtect=oldProtect;
    InterlockedExchange(&g_v33Bp.protectionHeld,1);
    return true;
}

void V33RestoreProtection() {
    if(InterlockedExchange(&g_v33Bp.protectionHeld,0)==0) return;
    DWORD ignored=0;
    VirtualProtect(reinterpret_cast<void*>(g_v33Bp.address),1,g_v33Bp.oldProtect,&ignored);
    FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<const void*>(g_v33Bp.address),1);
}

bool V33PatchByteWhileWritable(uintptr_t address,uint8_t value) {
    if(!address || InterlockedCompareExchange(&g_v33Bp.protectionHeld,0,0)==0) return false;
    *reinterpret_cast<volatile uint8_t*>(address)=value;
    FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<const void*>(address),1);
    return true;
}

void V33SnapMemory(uint32_t address,V33MemSnap& out) {
    out.address=address; out.valid=0;
    if(!address) return;
    // ReadProcessMemory avoids raising a nested access violation from inside VEH.
    SIZE_T got=0;
    if(ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)),
                         out.bytes, sizeof(out.bytes), &got) && got>0)
        out.valid=static_cast<uint32_t>(got);
}

uint32_t V33SnapDword(const V33MemSnap& s,size_t off) {
    uint32_t v=0;
    if(off+4<=s.valid) std::memcpy(&v,s.bytes+off,4);
    return v;
}

LONG CALLBACK V33VehHandler(EXCEPTION_POINTERS* ep) {
    if(!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    if(ep->ExceptionRecord->ExceptionCode!=EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    if(InterlockedCompareExchange(&g_v33Bp.armed,0,0)==0) return EXCEPTION_CONTINUE_SEARCH;

    CONTEXT* c=ep->ContextRecord;
    const uintptr_t exceptionAt=reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    const uintptr_t eipAt=static_cast<uintptr_t>(c->Eip);
    // Windows normally reports ExceptionAddress at the INT3 byte and EIP after it.
    // Accept both forms so a different exception-dispatch path cannot make us miss our own trap.
    if(exceptionAt!=g_v33Bp.address && (eipAt==0 || eipAt-1u!=g_v33Bp.address))
        return EXCEPTION_CONTINUE_SEARCH;

    // IMPORTANT V33.1: do not call VirtualProtect from inside the VEH. The page was
    // made writable before arming; here we only restore one byte and capture POD data.
    *reinterpret_cast<volatile uint8_t*>(g_v33Bp.address)=g_v33Bp.original;
    FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<const void*>(g_v33Bp.address),1);
    InterlockedExchange(&g_v33Bp.armed,0);

    g_v33Capture.threadId=GetCurrentThreadId();
    g_v33Capture.eax=c->Eax; g_v33Capture.ebx=c->Ebx; g_v33Capture.ecx=c->Ecx;
    g_v33Capture.edx=c->Edx; g_v33Capture.esi=c->Esi; g_v33Capture.edi=c->Edi;
    g_v33Capture.ebp=c->Ebp; g_v33Capture.esp=c->Esp; g_v33Capture.eflags=c->EFlags;

    // [ESP] is the return address at function entry. Use ReadProcessMemory so an
    // invalid diagnostic pointer cannot recursively fault inside the exception handler.
    for(size_t i=0;i<8;++i) {
        uint32_t v=0;
        SIZE_T got=0;
        ReadProcessMemory(GetCurrentProcess(),
                          reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(c->Esp)+i*4u),
                          &v,sizeof(v),&got);
        g_v33Capture.stack[i]=(got==sizeof(v))?v:0;
    }

    V33SnapMemory(c->Ecx,g_v33Capture.ecxSnap);
    for(size_t i=0;i<6;++i) {
        const uint32_t arg=g_v33Capture.stack[i+1];
        V33SnapMemory(arg,g_v33Capture.argSnap[i]);
        const uint32_t p0=V33SnapDword(g_v33Capture.argSnap[i],0);
        const uint32_t p1=V33SnapDword(g_v33Capture.argSnap[i],4);
        V33SnapMemory(p0,g_v33Capture.argNested0[i]);
        V33SnapMemory(p1,g_v33Capture.argNested1[i]);
    }

    g_v33Capture.methodAddress=static_cast<uint32_t>(g_v33Bp.address);
    InterlockedExchange(&g_v33Capture.hit,1);

    // Resume at the restored first instruction. Do not single-step/re-arm.
    c->Eip=static_cast<DWORD>(g_v33Bp.address);
    return EXCEPTION_CONTINUE_EXECUTION;
}

bool V33InstallOneShot(uintptr_t address,uint32_t methodRva) {
    if(!address) return false;
    if(InterlockedCompareExchange(&g_v33Bp.armed,0,0)!=0) return false;
    V33RestoreProtection();

    uint8_t b=0;
    if(!SafeRead(address,b)) return false;
    std::memset(&g_v33Capture,0,sizeof(g_v33Capture));
    g_v33Capture.methodRva=methodRva;
    g_v33Bp.address=address;
    g_v33Bp.original=b;

    if(!g_v33Veh) {
        g_v33Veh=AddVectoredExceptionHandler(1,V33VehHandler);
        if(!g_v33Veh) return false;
    }
    if(!V33PrepareWritable(address)) return false;
    if(!V33PatchByteWhileWritable(address,0xCC)) {
        V33RestoreProtection();
        return false;
    }
    InterlockedExchange(&g_v33Bp.armed,1);
    return true;
}

void V33Disarm() {
    if(InterlockedCompareExchange(&g_v33Bp.armed,0,0)!=0) {
        if(InterlockedCompareExchange(&g_v33Bp.protectionHeld,0,0)!=0)
            V33PatchByteWhileWritable(g_v33Bp.address,g_v33Bp.original);
        InterlockedExchange(&g_v33Bp.armed,0);
    }
    V33RestoreProtection();
}

std::string V33ClassFromVtableValue(const Report& r,uint32_t vt) {
    if(!vt || !r.module.inModule(vt)) return {};
    const uint32_t vr=r.module.rva(vt);
    for(const char* cls:{"LocalPlayer","Player","Monster","Npc","Item","Tile","Container","UIMap","MapView"}) {
        auto it=r.rtti.find(cls);
        if(it==r.rtti.end()) continue;
        if(std::find(it->second.vtableRvas.begin(),it->second.vtableRvas.end(),vr)!=it->second.vtableRvas.end())
            return cls;
    }
    return {};
}

std::string V33ClassFromSnap(const Report& r,const V33MemSnap& s) {
    if(s.valid<4) return {};
    return V33ClassFromVtableValue(r,V33SnapDword(s,0));
}

bool V33LooksVector3(const V33MemSnap& s,uint32_t& begin,uint32_t& end,uint32_t& cap) {
    if(s.valid<12) return false;
    begin=V33SnapDword(s,0); end=V33SnapDword(s,4); cap=V33SnapDword(s,8);
    if(!begin && !end && !cap) return true;
    if(!begin || end<begin || cap<end) return false;
    if(static_cast<uint64_t>(cap)-begin>0x10000ULL) return false;
    return true;
}


int V33ExpectedArgs(const std::string& token);

bool V34CreatureFamily(const std::string& cls) {
    return cls=="LocalPlayer" || cls=="Player" || cls=="Monster" || cls=="Npc";
}

bool V34LooksPosition(const V33MemSnap& s,uint32_t& x,uint32_t& y,uint32_t& z) {
    if(s.valid<12) return false;
    x=V33SnapDword(s,0); y=V33SnapDword(s,4); z=V33SnapDword(s,8);
    return x>0 && x<=65535 && y>0 && y<=65535 && z<=15;
}


bool V35IsCallerStackRef(const V33Capture& c,uint32_t address) {
    if(!address || !c.esp || address<c.esp) return false;
    return static_cast<uint64_t>(address)-c.esp <= 0x1000ULL;
}

bool V35LooksPackedPosition(const V33MemSnap& s,uint16_t& x,uint16_t& y,uint8_t& z,bool& special) {
    if(s.valid<5) return false;
    std::memcpy(&x,s.bytes+0,2);
    std::memcpy(&y,s.bytes+2,2);
    z=s.bytes[4];
    special = (x==0xFFFFu);
    if(special) return true; // inventory/container encoded Position used by OTC clients
    return x>0 && y>0 && z<=15;
}


struct V36PositionMatch {
    bool ok=false;
    bool confirmed=false;
    std::string abiType;
    std::string source;
    size_t offset=0;
    uint32_t x=0,y=0,z=0;
};

bool V36ReadLocalPosition(const Report& r,uint32_t& x,uint32_t& y,uint32_t& z) {
    x=y=z=0;
    if(!r.localPlayerPtr || r.positionRuntime.x<0 || r.positionRuntime.y<0 || r.positionRuntime.z<0) return false;
    uint8_t zz=0;
    if(!SafeRead(r.localPlayerPtr+r.positionRuntime.x,x) ||
       !SafeRead(r.localPlayerPtr+r.positionRuntime.y,y) ||
       !SafeRead(r.localPlayerPtr+r.positionRuntime.z,zz)) return false;
    z=zz;
    return x>0 && x<=65535 && y>0 && y<=65535 && z<=15;
}

bool V36NearLocal(const Report& r,uint32_t x,uint32_t y,uint32_t z) {
    uint32_t lx=0,ly=0,lz=0;
    if(!V36ReadLocalPosition(r,lx,ly,lz)) return false;
    const uint32_t dx=(x>lx)?(x-lx):(lx-x);
    const uint32_t dy=(y>ly)?(y-ly):(ly-y);
    const uint32_t dz=(z>lz)?(z-lz):(lz-z);
    return dx<=32 && dy<=32 && dz<=1;
}

bool V36ScanPackedPosition(const Report& r,const V33MemSnap& snap,const std::string& source,
                           const std::string& abiType,V36PositionMatch& out) {
    if(snap.valid<5) return false;
    const size_t maxOff=std::min<size_t>(snap.valid-5,24);
    for(size_t off=0;off<=maxOff;++off) {
        uint16_t x16=0,y16=0; uint8_t z8=0;
        std::memcpy(&x16,snap.bytes+off,2);
        std::memcpy(&y16,snap.bytes+off+2,2);
        z8=snap.bytes[off+4];
        if(!x16 || !y16 || z8>15) continue;
        if(!V36NearLocal(r,x16,y16,z8)) continue;
        out.ok=true; out.confirmed=true; out.source=source; out.abiType=abiType;
        out.offset=off; out.x=x16; out.y=y16; out.z=z8;
        return true;
    }
    return false;
}

bool V36ScanU32Position(const Report& r,const V33MemSnap& snap,const std::string& source,
                        const std::string& abiType,V36PositionMatch& out) {
    if(snap.valid<12) return false;
    const size_t maxOff=std::min<size_t>(snap.valid-12,24);
    for(size_t off=0;off<=maxOff;off+=4) {
        uint32_t x=0,y=0,z=0;
        std::memcpy(&x,snap.bytes+off,4);
        std::memcpy(&y,snap.bytes+off+4,4);
        std::memcpy(&z,snap.bytes+off+8,4);
        if(!x || !y || x>65535 || y>65535 || z>15) continue;
        if(!V36NearLocal(r,x,y,z)) continue;
        out.ok=true; out.confirmed=true; out.source=source; out.abiType=abiType;
        out.offset=off; out.x=x; out.y=y; out.z=z;
        return true;
    }
    return false;
}


bool V37ScanU32U32U8Position(const Report& r,const V33MemSnap& snap,const std::string& source,
                              const std::string& abiType,V36PositionMatch& out) {
    if(snap.valid<9) return false;
    const size_t maxOff=std::min<size_t>(snap.valid-9,24);
    for(size_t off=0;off<=maxOff;++off) {
        uint32_t x=0,y=0; uint8_t z=0;
        std::memcpy(&x,snap.bytes+off,4);
        std::memcpy(&y,snap.bytes+off+4,4);
        z=snap.bytes[off+8];
        if(!x || !y || x>65535 || y>65535 || z>15) continue;
        if(!V36NearLocal(r,x,y,z)) continue;
        out.ok=true; out.confirmed=true; out.source=source; out.abiType=abiType;
        out.offset=off; out.x=x; out.y=y; out.z=z;
        return true;
    }
    return false;
}

V36PositionMatch V36FindMovePosition(const Report& r,const V33Capture& c,int idx) {
    V36PositionMatch out;
    if(idx<0 || idx>=6) return out;
    const uint32_t raw=c.stack[idx+1];
    if(!V35IsCallerStackRef(c,raw)) return out;

    // V36 runtime evidence on this exact Ramonia build showed:
    //   32 03 00 00 43 07 00 00 07 ...
    // => x=818 (u32), y=1859 (u32), z=7 (u8), one SQM from LocalPlayer.
    // Keep the detector generic by validating the decoded xyz against live LP position.
    if(V37ScanU32U32U8Position(r,c.argSnap[idx],"direct","position_u32_u32_u8_ref",out)) return out;
    if(V36ScanPackedPosition(r,c.argSnap[idx],"direct","position_u16_u16_u8_ref",out)) return out;
    if(V36ScanU32Position(r,c.argSnap[idx],"direct","position_u32x3_ref",out)) return out;

    if(V37ScanU32U32U8Position(r,c.argNested0[idx],"p0","position_ptr_u32_u32_u8_ref",out)) return out;
    if(V36ScanPackedPosition(r,c.argNested0[idx],"p0","position_ptr_u16_u16_u8_ref",out)) return out;
    if(V36ScanU32Position(r,c.argNested0[idx],"p0","position_ptr_u32x3_ref",out)) return out;

    if(V37ScanU32U32U8Position(r,c.argNested1[idx],"p1","position_second_ptr_u32_u32_u8_ref",out)) return out;
    if(V36ScanPackedPosition(r,c.argNested1[idx],"p1","position_second_ptr_u16_u16_u8_ref",out)) return out;
    if(V36ScanU32Position(r,c.argNested1[idx],"p1","position_second_ptr_u32x3_ref",out)) return out;
    return out;
}

std::string V36HexBytes(const V33MemSnap& s,size_t maxBytes=32) {
    std::ostringstream os;
    const size_t n=std::min<size_t>(s.valid,maxBytes);
    for(size_t i=0;i<n;++i) {
        if(i) os << ' ';
        os << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<unsigned>(s.bytes[i]);
    }
    return os.str();
}

void V36PrintMoveArg2Evidence(const Report& r,const V33Capture& c) {
    const int idx=1;
    const V36PositionMatch m=V36FindMovePosition(r,c,idx);
    uint32_t lx=0,ly=0,lz=0;
    const bool lp=V36ReadLocalPosition(r,lx,ly,lz);
    std::cout << "    [V37 MOVE ARG2] raw=" << HexU32(c.stack[idx+1]);
    if(lp) std::cout << " localPos={" << lx << "," << ly << "," << lz << "}";
    std::cout << "\n"
              << "      direct@" << HexU32(c.argSnap[idx].address)
              << " bytes=" << V36HexBytes(c.argSnap[idx]) << "\n"
              << "      p0@" << HexU32(c.argNested0[idx].address)
              << " bytes=" << V36HexBytes(c.argNested0[idx]) << "\n"
              << "      p1@" << HexU32(c.argNested1[idx].address)
              << " bytes=" << V36HexBytes(c.argNested1[idx]) << "\n";
    if(m.ok) {
        std::cout << "      POSITION MATCH source=" << m.source
                  << " offset=+" << HexU32(static_cast<uint32_t>(m.offset))
                  << " xyz={" << m.x << "," << m.y << "," << m.z << "}"
                  << " abiType=" << m.abiType
                  << " status=" << (m.confirmed?"runtime_confirmed":"runtime_observed") << "\n";
    } else {
        std::cout << "      POSITION MATCH: not found\n";
    }
}

bool V35LooksDirectionVector(const V33MemSnap& vec,const V33MemSnap& data,
                             uint32_t& count,uint32_t& sampled) {
    uint32_t begin=0,end=0,cap=0;
    count=sampled=0;
    if(!V33LooksVector3(vec,begin,end,cap) || !begin || end<=begin) return false;
    const uint32_t bytes=end-begin;
    if(bytes%4u!=0) return false;
    count=bytes/4u;
    if(count==0 || count>512) return false;
    sampled=std::min<uint32_t>(count,data.valid/4u);
    if(sampled==0) return false;
    for(uint32_t i=0;i<sampled;++i) {
        uint32_t d=0;
        std::memcpy(&d,data.bytes+i*4u,4);
        if(d>7u) return false;
    }
    return true;
}

bool V35HolderObject(const Report& r,const V33Capture& c,int idx,std::string& cls) {
    cls.clear();
    if(idx<0 || idx>=6) return false;
    const uint32_t raw=c.stack[idx+1];
    if(!V35IsCallerStackRef(c,raw)) return false;
    cls=V33ClassFromSnap(r,c.argNested0[idx]);
    return !cls.empty();
}

bool V35ActionAlreadyConfirmed(const Report& r,const std::string& token) {
    auto a=r.actionAbi.find(token);
    if(a!=r.actionAbi.end() && a->second.status=="runtime_confirmed") return true;
    auto raw=r.actionAbiRaw.find(token);
    if(raw==r.actionAbiRaw.end()) return false;
    return JsonStringValue(raw->second,"status")=="runtime_confirmed";
}

bool V34LooksMsvcString(const V33MemSnap& s,const V33MemSnap& nested0) {
    if(s.valid<24) return false;
    const uint32_t sz=V33SnapDword(s,0x10), cp=V33SnapDword(s,0x14);
    if(sz>0x400 || cp<sz || cp>0x100000) return false;
    if(cp<16) return true;
    return nested0.valid>0;
}

ActionAbiArgV34 V34ClassifyArg(const Report& r,const std::string& token,int idx,const V33Capture& c) {
    ActionAbiArgV34 out;
    out.index=idx;
    out.semantic="Unknown";
    out.abiType="unknown";
    out.status="runtime_observed";

    if(idx<0 || idx>=6) return out;
    const V33MemSnap& snap=c.argSnap[idx];
    const V33MemSnap& n0s=c.argNested0[idx];
    const uint32_t raw=c.stack[idx+1];
    const std::string direct=V33ClassFromSnap(r,snap);

    if(token=="talkChannel") {
        if(idx==0) { out.semantic="MessageMode"; out.abiType="u32"; out.status="runtime_confirmed"; return out; }
        if(idx==1) { out.semantic="Channel"; out.abiType="u32"; out.status="runtime_confirmed"; return out; }
        if(idx==2 && V34LooksMsvcString(snap,n0s)) {
            out.semantic="Message"; out.abiType="msvc_string_ref"; out.status="runtime_confirmed"; return out;
        }
    }

    if((token=="attack" || token=="follow") && idx==0 && V34CreatureFamily(direct)) {
        // Game::attack/follow receive CreaturePtr by value on this OTC family.
        // shared_object_ptr is pointer-sized, so the stack slot contains the
        // Creature* bits directly even though ownership semantics are NOT raw.
        // The caller must add_ref before the call; the callee releases its
        // by-value parameter when it returns.
        out.semantic="Creature";
        out.abiType="shared_object_ptr_value";
        out.status="runtime_confirmed";
        out.rtti=direct;
        // This OTC family uses stdext::shared_object as LuaObject base:
        // x86 primary vptr at +0x0, intrusive refcount at +0x4.
        out.hasRefCountOffset=true;
        out.refCountOffset=0x4;
        out.ownership="caller_add_ref_callee_release";
        return out;
    }

    if(token=="attack" && idx==1 && raw<=1u) {
        out.semantic="Cancel";
        out.abiType="bool";
        out.status="runtime_confirmed";
        out.hasObservedBool=true;
        out.observedBool=(raw!=0);
        return out;
    }

    if(token=="autoWalk" && idx==0) {
        uint32_t count=0,sampled=0;
        if(V35LooksDirectionVector(snap,n0s,count,sampled)) {
            out.semantic="Directions";
            out.abiType="vector_direction_u32_ref";
            out.status="runtime_confirmed";
            return out;
        }
        uint32_t vb=0,ve=0,vc=0;
        if(V33LooksVector3(snap,vb,ve,vc) && (vb || ve || vc)) {
            out.semantic="Directions"; out.abiType="vector_ref"; out.status="runtime_observed"; return out;
        }
    }

    if(token=="move" && idx==2) {
        out.semantic="Count"; out.abiType="u32";
        out.status=(raw>0 && raw<=0xFFFFu)?"runtime_confirmed":"runtime_observed";
        return out;
    }

    if(token=="move" && idx==1) {
        const V36PositionMatch pm=V36FindMovePosition(r,c,idx);
        if(pm.ok) {
            out.semantic="Position";
            out.abiType=pm.abiType;
            out.status=pm.confirmed?"runtime_confirmed":"runtime_observed";
            return out;
        }
    }

    std::string holderCls;
    if(V35HolderObject(r,c,idx,holderCls)) {
        if(token=="useWith" && idx==1)
            out.semantic="Thing";
        else
            out.semantic=V34CreatureFamily(holderCls)?"Creature":holderCls;
        out.abiType="shared_object_ptr_ref";
        out.status="runtime_confirmed";
        out.rtti=holderCls;
        return out;
    }

    uint32_t x=0,y=0,z=0;
    if(V34LooksPosition(snap,x,y,z)) {
        out.semantic="Position"; out.abiType="position_u32x3_ref"; out.status="runtime_observed"; return out;
    }

    if(!direct.empty()) {
        out.semantic=V34CreatureFamily(direct)?"Creature":direct;
        out.abiType="raw_ptr";
        out.status="runtime_observed";
        out.rtti=direct;
        return out;
    }

    if(V34LooksMsvcString(snap,n0s)) {
        out.semantic="String"; out.abiType="msvc_string_ref"; out.status="runtime_observed"; return out;
    }

    uint32_t vb=0,ve=0,vc=0;
    if(V33LooksVector3(snap,vb,ve,vc) && (vb || ve || vc)) {
        out.semantic="Vector"; out.abiType="vector_ref"; out.status="runtime_observed"; return out;
    }

    out.semantic="Scalar";
    out.abiType="u32";
    return out;
}

ActionAbiArgV34 V38ClassifyAutoWalkStartPositionValue(const Report& r,const V33Capture& c) {
    ActionAbiArgV34 out;
    out.index=1;
    out.semantic="Position";
    out.abiType="position_u32_u32_u8_value";
    out.status="runtime_observed";

    // Candidate OTCv8-family layout after the Directions reference:
    //   [ESP+8]  = startPos.x (u32)
    //   [ESP+12] = startPos.y (u32)
    //   [ESP+16] = startPos.z in the low byte (+ struct padding)
    const uint32_t x=c.stack[2];
    const uint32_t y=c.stack[3];
    const uint32_t z=c.stack[4] & 0xFFu;

    uint32_t lx=0,ly=0,lz=0;
    const bool localOk=V36ReadLocalPosition(r,lx,ly,lz);
    const uint32_t dx=localOk ? ((x>lx)?(x-lx):(lx-x)) : 0xFFFFFFFFu;
    const uint32_t dy=localOk ? ((y>ly)?(y-ly):(ly-y)) : 0xFFFFFFFFu;

    const bool plausible=
        x>0 && x<=65535 &&
        y>0 && y<=65535 &&
        z<=15 &&
        localOk &&
        z==lz &&
        dx<=32 && dy<=32;

    std::cout << "    [V38 AUTOWALK STARTPOS] stack={"
              << x << "," << y << "," << z << "}";
    if(localOk)
        std::cout << " local={" << lx << "," << ly << "," << lz
                  << "} delta={" << dx << "," << dy << "}";
    else
        std::cout << " local=<unavailable>";
    std::cout << " status=" << (plausible?"runtime_confirmed":"runtime_observed") << "\n";

    if(plausible) out.status="runtime_confirmed";
    return out;
}

void V34PromoteCapture(Report& r,const std::string& token) {
    auto bit=r.binds.find(token);
    if(bit==r.binds.end() || !bit->second.selectedOk) return;

    ActionAbiV34 a;
    a.name=token;
    a.methodRva=bit->second.selected.methodRva;
    a.ownerRva=bit->second.selected.ownerRva;
    const bool thisOk = r.gameOwnerRva &&
        g_v33Capture.ecx==static_cast<uint32_t>(r.module.base+r.gameOwnerRva);

    if(token=="autoWalk") {
        // V40 deep scan + natural callsite proved the complete x86 ABI:
        // ECX=Game, Directions& at +8, Position value across +C/+10/+14,
        // and callee cleanup with RET 16.
        a.callingConvention="x86_ecx_callee_pop_16";
        a.args.push_back(V34ClassifyArg(r,token,0,g_v33Capture));
        a.args.push_back(V38ClassifyAutoWalkStartPositionValue(r,g_v33Capture));
    } else {
        const int argc=V33ExpectedArgs(token);
        for(int i=0;i<argc && i<6;++i)
            a.args.push_back(V34ClassifyArg(r,token,i,g_v33Capture));
    }

    a.status = thisOk ? "runtime_observed" : "runtime_this_mismatch";
    bool allArgsConfirmed=!a.args.empty();
    for(const auto& arg:a.args) if(arg.status!="runtime_confirmed") allArgsConfirmed=false;
    if(thisOk && allArgsConfirmed) a.status="runtime_confirmed";

    r.actionAbi[token]=a;
    r.actionAbiRaw.erase(token);

    std::cout << "    [V40 ABI] " << token
              << " status=" << a.status
              << " cc=" << a.callingConvention << "\n";
    for(const auto& arg:a.args) {
        std::cout << "      arg" << (arg.index+1)
                  << " semantic=" << arg.semantic
                  << " abiType=" << arg.abiType
                  << " status=" << arg.status;
        if(!arg.rtti.empty()) std::cout << " rtti=" << arg.rtti;
        std::cout << "\n";
    }
}

std::string V33AsciiPreview(const uint8_t* p,size_t n) {
    std::string out;
    for(size_t i=0;i<n && i<40;++i) {
        const unsigned char c=p[i];
        if(c==0) break;
        if(c<0x20 || c>0x7E) return {};
        out.push_back(static_cast<char>(c));
    }
    return out;
}

void V33DescribeArg(const Report& r,const std::string& token,int idx,const V33MemSnap& s,
                    const V33MemSnap& nested0,const V33MemSnap& nested1) {
    const uint32_t raw=s.address;
    std::cout << "      arg" << (idx+1) << "=" << HexU32(raw);
    if(!raw) { std::cout << " <null>\n"; return; }

    const std::string direct=V33ClassFromSnap(r,s);
    if(!direct.empty()) std::cout << " directRTTI=" << direct;

    const uint32_t p0=V33SnapDword(s,0), p1=V33SnapDword(s,4);
    const std::string n0=V33ClassFromSnap(r,nested0), n1=V33ClassFromSnap(r,nested1);
    if(!n0.empty()) std::cout << " firstDword->" << n0 << "(" << HexU32(p0) << ")";
    if(!n1.empty()) std::cout << " secondDword->" << n1 << "(" << HexU32(p1) << ")";

    // MSVC x86 std::string used by this OTC family: 16-byte buffer/pointer union,
    // then size at +0x10 and capacity at +0x14. Report only as a heuristic.
    if(s.valid>=24) {
        const uint32_t sz=V33SnapDword(s,0x10), cp=V33SnapDword(s,0x14);
        if(sz<=0x400 && cp>=sz && cp<=0x100000) {
            std::string text;
            if(cp<16) text=V33AsciiPreview(s.bytes,std::min<size_t>(sz,s.valid));
            else if(nested0.valid) text=V33AsciiPreview(nested0.bytes,std::min<size_t>(sz,nested0.valid));
            if((token=="talk" || token=="talkChannel") || !text.empty()) {
                std::cout << " msvcString?{size=" << sz << ",cap=" << cp;
                if(!text.empty()) std::cout << ",text='" << text << "'";
                std::cout << "}";
            }
        }
    }

    uint16_t px=0,py=0; uint8_t pz=0; bool special=false;
    if(V35LooksPackedPosition(s,px,py,pz,special)) {
        std::cout << " packedPos?{x=" << px << ",y=" << py << ",z=" << static_cast<unsigned>(pz)
                  << (special?",special":"") << "}";
    }

    uint32_t vb=0,ve=0,vc=0;
    if(V33LooksVector3(s,vb,ve,vc) && (vb || ve || vc)) {
        std::cout << " vectorLike?{" << HexU32(vb) << "," << HexU32(ve) << "," << HexU32(vc)
                  << ",bytes=" << (ve>=vb?ve-vb:0) << "}";
        uint32_t cnt=0,sampled=0;
        if(V35LooksDirectionVector(s,nested0,cnt,sampled))
            std::cout << " directionVector{count=" << cnt << ",sampled=" << sampled << "}";
    }
    std::cout << "\n";
}

int V33ExpectedArgs(const std::string& token) {
    // V40 prints arg1 via the generic classifier; the 12-byte Position value
    // is reconstructed from stack[2..4] by the dedicated classifier.
    if(token=="autoWalk") return 1;
    if(token=="move") return 3;
    if(token=="useWith") return 2;
    if(token=="talkChannel") return 3;
    if(token=="attack") return 2;
    return 1;
}

const char* V33Instruction(const std::string& token) {
    if(token=="autoWalk") return "Clique/ande para um destino distante o bastante para o cliente iniciar auto-walk.";
    if(token=="attack") return "Ataque uma criatura normalmente pelo cliente.";
    if(token=="follow") return "Use Follow em uma criatura normalmente pelo cliente.";
    if(token=="look") return "Dê Look em um item/creature normalmente pelo cliente.";
    if(token=="move") return "Arraste/mova um item normalmente (chao, container ou inventario).";
    if(token=="use") return "Use normalmente um item/objeto que dispara Game::use.";
    if(token=="useWith") return "Use normalmente um item/runa/ferramenta COM um alvo.";
    if(token=="talk") return "Fale uma mensagem curta e unica, por exemplo: v33test.";
    if(token=="talkChannel") return "Fale uma mensagem curta; Game::talk chama talkChannel internamente nesta build.";
    return "Execute a acao normalmente pelo cliente.";
}

void V40PrintAutoWalkCallerEvidence(const Report& r,const V33Capture& c) {
    if(!c.stack[0] || !r.module.inModule(c.stack[0])) {
        std::cout << "    [V40 CALLSITE] return address outside module\n";
        return;
    }

    const uintptr_t ret=static_cast<uintptr_t>(c.stack[0]);
    const uint32_t retRva=r.module.rva(ret);
    uint8_t before[64]{};
    uint8_t after[24]{};
    const uintptr_t beforeAt=ret>=sizeof(before)?ret-sizeof(before):ret;
    const bool beforeOk=SafeCopy(beforeAt,before,sizeof(before));
    const bool afterOk=SafeCopy(ret,after,sizeof(after));

    std::cout << "    [V40 CALLSITE] returnRva=" << HexU32(retRva);
    if(ret>=r.module.base+5) {
        uint8_t call[5]{};
        if(SafeCopy(ret-5,call,sizeof(call)) && call[0]==0xE8) {
            int32_t rel=0; std::memcpy(&rel,call+1,4);
            const uintptr_t dst=ret+static_cast<int64_t>(rel);
            if(r.module.inModule(dst))
                std::cout << " directCallTargetRva=" << HexU32(r.module.rva(dst));
        }
    }
    std::cout << "\n";

    if(beforeOk) {
        std::cout << "    [V40 PRECALL 64B]";
        for(size_t i=0;i<sizeof(before);++i) {
            if(i && i%16==0) std::cout << "\n                       ";
            std::cout << " " << std::uppercase << std::hex << std::setw(2)
                      << std::setfill('0') << static_cast<unsigned>(before[i])
                      << std::dec << std::setfill(' ');
        }
        std::cout << "\n";
    }

    if(afterOk) {
        std::cout << "    [V40 POSTCALL 24B]";
        for(size_t i=0;i<sizeof(after);++i)
            std::cout << " " << std::uppercase << std::hex << std::setw(2)
                      << std::setfill('0') << static_cast<unsigned>(after[i])
                      << std::dec << std::setfill(' ');
        std::cout << "\n";

        if(after[0]==0x83 && after[1]==0xC4) {
            std::cout << "    [V40 CALLER CLEANUP] add esp,"
                      << static_cast<unsigned>(after[2]) << " bytes\n";
        } else if(after[0]==0x81 && after[1]==0xC4) {
            uint32_t imm=0; std::memcpy(&imm,after+2,4);
            std::cout << "    [V40 CALLER CLEANUP] add esp," << imm << " bytes\n";
        } else {
            std::cout << "    [V40 CALLER CLEANUP] no immediate add esp after CALL\n";
        }
    }
}

void V33PrintCapture(const Report& r,const std::string& token) {
    const V33Capture& c=g_v33Capture;
    std::cout << "\n[V33.1 CAPTURE] " << token << " HIT"
              << " thread=" << c.threadId
              << " methodRva=" << HexU32(c.methodRva) << "\n"
              << "    ECX(this)=" << HexU32(c.ecx)
              << " EAX=" << HexU32(c.eax) << " EDX=" << HexU32(c.edx)
              << " EBX=" << HexU32(c.ebx) << " ESI=" << HexU32(c.esi)
              << " EDI=" << HexU32(c.edi) << "\n"
              << "    ESP=" << HexU32(c.esp) << " EBP=" << HexU32(c.ebp)
              << " return=" << HexU32(c.stack[0]) << "\n";

    if(r.gameOwnerRva && c.ecx==static_cast<uint32_t>(r.module.base+r.gameOwnerRva))
        std::cout << "    THIS MATCH: Game owner exact\n";
    else
        std::cout << "    THIS MATCH: " << (c.ecx?"NOT exact Game owner":"NULL") << "\n";

    const int argc=V33ExpectedArgs(token);
    for(int i=0;i<argc && i<6;++i)
        V33DescribeArg(r,token,i,c.argSnap[i],c.argNested0[i],c.argNested1[i]);

    std::cout << "    raw stack:";
    const int rawLast=(token=="autoWalk")?5:std::min(argc+1,7);
    for(int i=0;i<=rawLast;++i)
        std::cout << " [ESP+" << HexU32(i*4) << "]=" << HexU32(c.stack[i]);
    std::cout << "\n";

    if(token=="autoWalk")
        V40PrintAutoWalkCallerEvidence(r,c);
}

void V33CaptureAction(Report& r,const std::string& token) {
    auto it=r.binds.find(token);
    if(it==r.binds.end() || !it->second.selectedOk) {
        std::cout << "[V33.1] " << token << " <bind indisponivel>\n"; return;
    }
    if(AskYesNo(std::string("Capturar runtime de ")+token+" agora?")!='s') {
        std::cout << "[V33.1] " << token << " SKIP pelo usuario.\n"; return;
    }

    const uint32_t rva=it->second.selected.methodRva;
    const uintptr_t fn=r.module.base+rva;
    for(int attempt=1;attempt<=2;++attempt) {
        if(!V33InstallOneShot(fn,rva)) {
            std::cout << "[V33.1] falha ao instalar breakpoint reversivel em " << token << ".\n"; return;
        }
        std::cout << "\n[V33.1 ARMED] " << token << " RVA=" << HexU32(rva) << " attempt=" << attempt << "\n"
                  << "  " << V33Instruction(token) << "\n"
                  << "  Quando executar a acao, volte ao console e pressione ENTER.\n";
        WaitEnter("  ENTER apos a acao... ");
        V33Disarm();
        if(InterlockedCompareExchange(&g_v33Capture.hit,0,0)!=0) {
            V33PrintCapture(r,token);
            if(token=="move") V36PrintMoveArg2Evidence(r,g_v33Capture);
            V34PromoteCapture(r,token);
            SaveCheckpoint(r,std::string("action_abi_")+token);
            return;
        }
        std::cout << "[V33.1 MISS] o metodo nao foi atingido nesta tentativa.\n";
        if(attempt==1 && AskYesNo("Tentar esta mesma acao mais uma vez?")=='s') continue;
        break;
    }
}

void RunGameActionRuntimeCaptureV33(Report& r) {
    std::cout << "\n============================================================\n"
              << "          GAME ACTION ABI RUNTIME CAPTURE V33.1\n"
              << "============================================================\n"
              << "[MODE] nenhuma chamada interna do jogo feita pelo probe.\n"
              << "[MODE] um INT3 temporario por vez; byte restaurado no hit; protecao restaurada fora do VEH.\n"
              << "[CAPTURE] ECX + pilha de entrada + snapshots via ReadProcessMemory; nenhuma VirtualProtect dentro do VEH.\n"
              << "[RULE] V40 recaptura autoWalk e imprime caller/callee cleanup antes de qualquer nova mudanca de ABI.\n";

    for(const char* tok:ramonia_baseline::gameActions) {
        const bool alreadyConfirmed=V35ActionAlreadyConfirmed(r,tok);
        if(alreadyConfirmed && std::string(tok)!="autoWalk") {
            std::cout << "[V40 SKIP] " << tok << " ja runtime-confirmed no profile.\n";
            continue;
        }
        if(alreadyConfirmed && std::string(tok)=="autoWalk") {
            std::cout << "[V40 RECAPTURE] autoWalk sera capturado apenas para fechar stack cleanup/callsite.\n";
        }
        // Game::talk is bypassed by the normal UI path in this build; talkChannel is the live route.
        if(std::string(tok)=="talk" && r.actionAbiRaw.count("talkChannel")) {
            std::cout << "[V40 SKIP] talk nao e atingido pela UI normal; usando talkChannel como contrato ativo.\n";
            continue;
        }
        V33CaptureAction(r,tok);
    }

    V33Disarm();
    if(g_v33Veh) { RemoveVectoredExceptionHandler(g_v33Veh); g_v33Veh=nullptr; }
    std::cout << "\n[V40 COMPLETE] diagnostico autoWalk capturado; aguarde revisao antes de alterar ABI ativa.\n";
}


void RunTopMoveAbiBaseline(Report& r) {
    const auto bind=r.binds.find("getTopMoveThing");
    const uint32_t rva=bind!=r.binds.end() && bind->second.selectedOk
        ? bind->second.selected.methodRva : 0;
    const auto old=r.actionAbiRaw.find("getTopMoveThing");
    if(old!=r.actionAbiRaw.end() && TopMoveContractMatches(old->second,rva)) {
        std::cout << "[PIPE BASELINE] Tile::getTopMoveThing ABI/return/refcount "
                     "confirmados no profile da mesma build e mesmo bind.\n";
        return;
    }
    // Discovery never imports a known RVA or treats a RET/entry hit as proof
    // of returned ownership. A changed bind invalidates prior confirmation.
    r.actionAbi.erase("getTopMoveThing");
    bool tileThisObserved=false,hiddenStorageObserved=false;
    if(rva && r.localPlayerPtr &&
       AskYesNo("Capturar entrada natural de Tile::getTopMoveThing para ABI?")=='s') {
        if(V33InstallOneShot(r.module.base+rva,rva)) {
            std::cout << "[TOPMOVE ABI] Arraste um item do chao normalmente. "
                         "Depois volte ao console. Nenhuma chamada interna feita pelo probe.\n";
            WaitEnter("ENTER apos a acao... ");
            V33Disarm();
            if(InterlockedCompareExchange(&g_v33Capture.hit,0,0)!=0) {
                const auto& c=g_v33Capture;
                tileThisObserved=V33ClassFromSnap(r,c.ecxSnap)=="Tile";
                hiddenStorageObserved=V35IsCallerStackRef(c,c.stack[1]) &&
                    c.argSnap[0].valid>=4;
                std::cout << "[TOPMOVE ENTRY] ECX=" << HexU32(c.ecx)
                          << " TileRTTI=" << tileThisObserved
                          << " hiddenStorage[ESP+4]=" << HexU32(c.stack[1])
                          << " callerStack=" << hiddenStorageObserved << "\n";
            }
        }
        V33Disarm();
        if(g_v33Veh) { RemoveVectoredExceptionHandler(g_v33Veh); g_v33Veh=nullptr; }
    }
    std::ostringstream o;
    o << "{\"methodRva\":\"" << HexU32(rva)
      << "\",\"callingConvention\":\"pending\","
      << "\"status\":\"" << (rva?"abi_validation_required":"discovery_required") << "\","
      << "\"this\":{\"semantic\":\"Tile\",\"location\":\"ecx\",\"status\":\""
      << (tileThisObserved?"runtime_observed":"pending") << "\"},"
      << "\"args\":[],\"return\":{\"expectedAbiType\":\"shared_object_ptr<Thing>\","
      << "\"status\":\"pending\",\"hiddenStorage\":{\"expectedEntryLocation\":\"[esp+4]\","
      << "\"expectedFrameLocation\":\"[ebp+8]\",\"status\":\""
      << (hiddenStorageObserved?"runtime_observed":"pending")
      << "\"},\"refCount\":{\"status\":\"pending\"},"
      << "\"tileMembership\":\"pending\"},"
      << "\"validationRequired\":[\"callee_cleanup\",\"returned_ThingPtr\","
      << "\"same_tile_membership\",\"refcount_increment_and_release\"],"
      << "\"validationRoute\":\"runtime_return_capture_or_build_matched_bridge_topmoveat\"}";
    r.actionAbiRaw["getTopMoveThing"]=o.str();
    std::cout << "[PIPE BASELINE] getTopMoveThing pendente: confirmar retorno, "
                 "RET, Tile::things e refcount. Entrada sozinha nao promove ABI.\n";
}

std::vector<std::string> GuidedSavedObservations(const std::string& json) {
    std::vector<std::string> objects;
    const auto key=json.find("\"observations\"");
    if(key==std::string::npos) return objects;
    const auto start=json.find('[',key);
    if(start==std::string::npos) return objects;
    size_t objectStart=0;
    int depth=0;
    bool quoted=false,escaped=false;
    for(size_t i=start+1;i<json.size();++i) {
        const char c=json[i];
        if(quoted) {
            if(escaped) escaped=false;
            else if(c=='\\') escaped=true;
            else if(c=='"') quoted=false;
            continue;
        }
        if(c=='"') { quoted=true; continue; }
        if(c=='{' && depth++==0) objectStart=i;
        else if(c=='}') {
            if(depth<=0) return {};
            if(--depth==0) objects.push_back(json.substr(objectStart,i-objectStart+1));
        } else if(c==']' && depth==0) return objects;
    }
    return {}; // Reject truncated snapshots instead of retaining partial JSON.
}

std::string GuidedCandidateSignature(const BindResult& bind) {
    auto addresses=guided_discovery::candidates(bind);
    std::sort(addresses.begin(),addresses.end());
    std::ostringstream o;
    for(uint32_t address:addresses) {
        o << HexU32(address) << ":";
        std::set<uint32_t> owners;
        if(bind.selectedOk && bind.selected.methodRva==address) owners.insert(bind.selected.ownerRva);
        for(const auto& candidate:bind.candidates)
            if(candidate.methodRva==address) owners.insert(candidate.ownerRva);
        for(uint32_t owner:owners) o << HexU32(owner) << "/";
        o << ";";
    }
    return o.str();
}

std::string GuidedSnapshotJson(const V33MemSnap& snap) {
    std::ostringstream o;
    const size_t valid=std::min<size_t>(snap.valid,sizeof(snap.bytes));
    o << "{\"address\":\"" << HexU32(snap.address) << "\",\"validBytes\":" << valid << ",\"bytesHex\":\"";
    for(size_t i=0;i<valid;++i)
        o << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(snap.bytes[i]);
    o << "\"}";
    return o.str();
}

std::string GuidedCaptureJson(const Report& r,const V33Capture& c) {
    std::ostringstream o;
    o << "{\"pid\":" << GetCurrentProcessId() << ",\"moduleBase\":\"" << HexU64(r.module.base)
      << "\",\"threadId\":" << c.threadId << ",\"methodRva\":\"" << HexU32(c.methodRva)
      << "\",\"registers\":{\"eax\":\"" << HexU32(c.eax) << "\",\"ecx\":\"" << HexU32(c.ecx)
      << "\",\"edx\":\"" << HexU32(c.edx) << "\",\"esp\":\"" << HexU32(c.esp)
      << "\",\"ebp\":\"" << HexU32(c.ebp) << "\"},\"stack\":[";
    for(size_t i=0;i<8;++i) { if(i) o << ","; o << "\"" << HexU32(c.stack[i]) << "\""; }
    o << "],\"thisSnapshot\":" << GuidedSnapshotJson(c.ecxSnap) << ",\"arguments\":[";
    for(size_t i=0;i<6;++i) {
        if(i) o << ",";
        o << "{\"direct\":" << GuidedSnapshotJson(c.argSnap[i])
          << ",\"nested0\":" << GuidedSnapshotJson(c.argNested0[i])
          << ",\"nested1\":" << GuidedSnapshotJson(c.argNested1[i]) << "}";
    }
    o << "],\"thisClassObserved\":\"" << JsonEscape(V33ClassFromSnap(r,c.ecxSnap))
      << "\",\"thisMatchesGame\":"
      << ((r.gameOwnerRva && c.ecx==r.module.base+r.gameOwnerRva)?"true":"false")
      << ",\"callerRva\":\"" << HexU32(r.module.inModule(c.stack[0])?r.module.rva(c.stack[0]):0)
      << "\",\"savedPointersAreHistorical\":true}";
    return o.str();
}

std::string GuidedMethodJson(const std::string& signature,const std::vector<std::string>& observations,
                            const std::string& status) {
    std::ostringstream o;
    o << "{\"candidateSignature\":\"" << JsonEscape(signature) << "\",\"status\":\"" << status
      << "\",\"callable\":false,\"contractComplete\":false,\"activeCallValidated\":false,"
      << "\"pending\":[\"typed_argument_comparison\",\"return_and_stack_cleanup\","
      << "\"ownership_if_applicable\",\"game_thread_dispatch\",\"active_call\"],\"observations\":[";
    for(size_t i=0;i<observations.size();++i) { if(i) o << ","; o << observations[i]; }
    o << "]}";
    return o.str();
}

bool GuidedExecutableRva(const ModuleView& module,uint32_t rva) {
    if(!rva || rva>=module.sizeOfImage) return false;
    const uintptr_t address=module.base+rva;
    for(const auto& section:module.sections)
        if(section.executable() && address>=section.start && address<section.end) return true;
    return false;
}

#include "GroupedObservation.hpp"

DWORD WINAPI Worker(LPVOID) {
    OpenConsole();
    SetConsoleOutputCP(CP_UTF8);
    probe_console::Session presentation(g_self);
    probe_console::ui()<<"Crome - Descoberta x86 R4\nBuscando estruturas do cliente...\n"
        <<"Detalhes tecnicos: ResearchProbeDiagnostic.log ao lado da DLL.\n";
    PrintHeader();

    Report r;
    if (!BuildModuleView(r.module)) {
        std::cout << "[STOP] Esta DLL e x86: exige executavel PE32/i386 valido.\n"
                  << "Para clientes x64, use o BuildExplorer/ABIBuilder x64.\n";
        WaitEnter("ENTER para descarregar... ");
        presentation.close();
        if(grouped_observation::veh){FreeConsole();return 0;} // Retain delayed INT3 handler until client shutdown.
    FreeLibraryAndExitThread(g_self, 0);
    }

    r.fp = FingerprintTextFromDisk(r.module.path);
    r.mappedTextHash = FingerprintMappedText(r.module);
    r.imports = ReadImports(r.module);
    r.renderer = RendererHint(r.imports);

    ProfileCache cached = LoadProfileForBuild(r);
    if (cached.exactBuild) {
        r.profileLoaded = true;
        r.profileSources = cached.sources;
        std::cout << "[PROFILE RESUME] fingerprint exato encontrado. Fontes=" << cached.sources.size() << "\n";
        for (const auto& p : cached.sources) std::cout << "  " << p << "\n";
    } else {
        std::cout << "[PROFILE RESUME] nenhum profile exato. Discovery inicial desta fingerprint.\n";
    }

    std::cout << "[BUILD]\n"
              << "Executable     : " << r.module.path << "\n"
              << "ModuleBase     : " << HexU64(r.module.base) << "\n"
              << "Timestamp      : " << HexU32(r.module.timestamp) << "\n"
              << "SizeOfImage    : " << HexU32(r.module.sizeOfImage) << "\n"
              << "EntryPoint RVA : " << HexU32(r.module.entryRva) << "\n"
              << "Stable text hash (disk raw) : " << (r.fp.ok?HexU64(r.fp.textHash):"<unavailable>") << "\n"
              << "Mapped text hash (diagnostic): " << HexU64(r.mappedTextHash) << "\n"
              << "Renderer       : " << r.renderer << "\n\n";

    std::cout << "[SECTIONS]\n";
    for (const auto& s : r.module.sections) {
        std::cout << "  " << std::left << std::setw(8) << s.name << std::right
                  << " RVA=" << HexU32(s.rva)
                  << " mapped=" << HexU32(static_cast<uint32_t>(s.end-s.start))
                  << " " << (s.writable()?"W":"-") << (s.executable()?"X":"-") << "\n";
    }
    std::cout << "\n[IMPORTS/RENDERER]\n";
    for (const auto& imp : r.imports) {
        const auto l = Lower(imp);
        if (l.find("opengl")!=std::string::npos || l.find("gles")!=std::string::npos ||
            l.find("egl")!=std::string::npos || l.find("d3d")!=std::string::npos ||
            l.find("dxgi")!=std::string::npos) {
            std::cout << "  " << imp << "\n";
        }
    }

    std::cout << "\n============================================================\n"
              << "                     RTTI FIRST PASS\n"
              << "============================================================\n";
    const std::vector<std::string> classes = {
        "LocalPlayer","Player","Monster","Npc","Item","Tile","Container","UIMap","MapView"
    };
    for (const auto& cls : classes) {
        auto rr = ResolveRttiClass(r.module, cls);
        r.rtti[cls] = rr;
        std::cout << std::left << std::setw(12) << cls << std::right
                  << " TD=" << HexU32(rr.typeDescriptorRva) << " VTables=" << rr.vtableRvas.size();
        for (auto vt : rr.vtableRvas) std::cout << " " << HexU32(vt);
        std::cout << "\n";
    }

    std::cout << "\n============================================================\n"
              << "                 X86 SEMANTIC BIND PASS\n"
              << "============================================================\n"
              << "Method: exact token\\0 -> immediate xref -> PUSH owner + PUSH method\n";

    std::vector<std::string> tokens = {
        "autoWalk","attack","follow","look","move","use","useWith","talk","talkChannel",
        "getTile","getCreatureById","getCreatureByName","getTopUseThing",
        "getPosition","getThings","getThing","getThingCount","getGround","getTopThing","getTopLookThing","getTopMoveThing","getTopCreature","isWalkable","isPathable",
        "getAmbientLight","setAmbientLight",
        "getMinimumAmbientLight","setMinimumAmbientLight","setDrawLights","isDrawingLights",
        "getHealthPercent","setHealthPercent",
        "getBaseSpeed","setBaseSpeed","getSpeed","setSpeed",
        "getSkull","setSkull","getShield","setShield",
        "getOutfit","setOutfit","getDirection","setDirection",
        "getMagicLevel","setMagicLevel","getStates","setStates","getSoul","setSoul","getStamina","setStamina",
        "getInventoryItem","getItem","getItems","getId","setId","getCount","setCount","getItemCount","getCapacity","setCapacity","hasParent",
        "getContainer","getContainers","openContainer","closeContainer","getName","setName",
        "getFirstIndex","setFirstIndex","isUnlocked","setUnlocked","hasPages","setHasPages"
    };
    for (const char* required : ramonia_baseline::requiredBinds)
        if(std::find(tokens.begin(),tokens.end(),required)==tokens.end())
            tokens.emplace_back(required);
    BuildSemanticStringIndex(r.module,tokens);
    size_t tokenProgress=0;
    for (const auto& token : tokens) {
        std::cout << "[BUSCANDO METODO " << ++tokenProgress << "/" << tokens.size() << "] " << token << "\n" << std::flush;
        BindResult br;
        br.name = token;
        br.candidates = ResolveBindCandidates(r.module, token);
        if (br.candidates.empty() &&
            (token=="getMinimumAmbientLight" || token=="setMinimumAmbientLight" ||
             token=="setDrawLights" || token=="isDrawingLights")) {
            br.candidates = ResolveInstanceMethodCandidatesV13(r.module, token);
        }
        const bool creatureInstanceToken =
            token=="getHealthPercent" || token=="setHealthPercent" ||
            token=="getBaseSpeed" || token=="setBaseSpeed" ||
            token=="getSpeed" || token=="setSpeed" ||
            token=="getSkull" || token=="setSkull" ||
            token=="getShield" || token=="setShield" ||
            token=="getOutfit" || token=="setOutfit" ||
            token=="getDirection" || token=="setDirection" ||
            token=="getMagicLevel" || token=="setMagicLevel" ||
            token=="getStates" || token=="setStates" ||
            token=="getSoul" || token=="setSoul" ||
            token=="getStamina" || token=="setStamina" ||
            token=="getInventoryItem" || token=="getItem" || token=="getItems" ||
            token=="getId" || token=="setId" || token=="getCount" || token=="setCount" ||
            token=="getItemCount" || token=="getCapacity" || token=="setCapacity" || token=="hasParent" ||
            token=="getContainer" || token=="getContainers" || token=="openContainer" || token=="closeContainer" ||
            token=="getPosition" || token=="getThings" || token=="getThing" || token=="getThingCount" ||
            token=="getGround" || token=="getTopThing" || token=="getTopLookThing" || token=="getTopUseThing" ||
            token=="getTopMoveThing" || token=="getTopCreature" || token=="isWalkable" || token=="isPathable" ||
            token=="getName" || token=="setName" || token=="getFirstIndex" || token=="setFirstIndex" ||
            token=="isUnlocked" || token=="setUnlocked" || token=="hasPages" || token=="setHasPages";
        if (creatureInstanceToken) {
            auto extra = ResolveInstanceMethodCandidatesV13(r.module, token);
            br.candidates.insert(br.candidates.end(), extra.begin(), extra.end());
            std::sort(br.candidates.begin(), br.candidates.end(), [](const BindCandidate& a,const BindCandidate& b){
                if(a.score!=b.score) return a.score>b.score;
                return a.methodRva<b.methodRva;
            });
            br.candidates.erase(std::unique(br.candidates.begin(),br.candidates.end(),[](const BindCandidate& a,const BindCandidate& b){
                return a.methodRva==b.methodRva && a.tokenRva==b.tokenRva;
            }),br.candidates.end());
        }
        r.binds[token] = br;
        PrintAllBindCandidates(br);
    }

    if (r.binds["getTopUseThing"].candidates.empty()) {
        auto aliases = FindAsciiStringsContaining(r.module, "TopUseThing");
        for (const auto& actual : aliases) {
            auto extra = ResolveBindCandidates(r.module, actual);
            r.binds["getTopUseThing"].candidates.insert(r.binds["getTopUseThing"].candidates.end(),
                                                         extra.begin(), extra.end());
        }
        if (!aliases.empty()) {
            std::cout << "[FALLBACK] TopUseThing-like strings=" << aliases.size() << "\n";
        }
    }

    const std::vector<std::string> gameTokens(
        ramonia_baseline::gameActions.begin(),ramonia_baseline::gameActions.end());
    r.gameOwnerRva = ChooseOwnerCluster(r.binds, gameTokens, 4);
    r.mapOwnerRva = ChooseOwnerCluster(r.binds, {"getTile","getCreatureById"}, 2);

    for (auto& kv : r.binds) {
        uint32_t pref = 0;
        if (std::find(gameTokens.begin(), gameTokens.end(), kv.first) != gameTokens.end()) pref = r.gameOwnerRva;
        if (kv.first == "getTile" || kv.first == "getCreatureById") pref = r.mapOwnerRva;
        SelectBind(kv.second, pref);
        std::cout << std::left << std::setw(20) << kv.first << std::right;
        if (kv.second.selectedOk) {
            const auto& c = kv.second.selected;
            std::cout << " method=" << HexU32(c.methodRva)
                      << " owner=" << HexU32(c.ownerRva)
                      << " [" << c.ownerSection << "]"
                      << " candidates=" << kv.second.candidates.size() << "\n";
        } else {
            std::cout << " <not found>\n";
        }
    }

    std::cout << "\n[OWNER CLUSTERS]\n"
              << "Game owner RVA : " << HexU32(r.gameOwnerRva) << "\n"
              << "Map owner RVA  : " << HexU32(r.mapOwnerRva) << "\n";

    uint32_t attackRva = 0, followRva = 0;
    if (r.binds["attack"].selectedOk) attackRva = r.binds["attack"].selected.methodRva;
    if (r.binds["follow"].selectedOk) followRva = r.binds["follow"].selected.methodRva;
    r.gameLayout = InferGameLayout(r.module, attackRva, followRva);
    ApplyLoadedProfile(r, cached);
    std::cout << "\n[CONTINUANDO] Buscando hints de luz em blocos de memoria...\n" << std::flush;
    const auto lightStrings = FindAsciiStringsContaining(r.module, "light");
    std::cout << "\n[LIGHT SEMANTIC HINTS] strings containing 'light'=" << lightStrings.size() << "\n";
    for (size_t i=0; i<lightStrings.size() && i<24; ++i)
        std::cout << "  " << lightStrings[i] << "\n";

    // Validate the imported Tile ABI before any discovery checkpoint can save it.
    RunTopMoveAbiBaseline(r);
    RunUiMapLightStaticDiscoveryV13(r);
    r.gameLayout.inferred = r.gameLayout.localPlayer >= 0 && r.gameLayout.attackTarget >= 0 &&
                            r.gameLayout.followTarget >= 0 &&
                            r.gameLayout.localPlayer != r.gameLayout.attackTarget &&
                            r.gameLayout.attackTarget != r.gameLayout.followTarget;
    if (r.profileLoaded) {
        std::cout << "\n[PROFILE LOADED] mesmos dados confirmados nao serao recalibrados se o quick-check passar.\n";
    }

    std::cout << "\n============================================================\n"
              << "                 STATIC GAME LAYOUT INFERENCE\n"
              << "============================================================\n";
    auto poff=[](int v){return v>=0?HexU32(static_cast<uint32_t>(v)):std::string("<pending>");};
    std::cout << "Game.LocalPlayer candidate : +" << poff(r.gameLayout.localPlayer) << "\n"
              << "Game.AttackTarget candidate: +" << poff(r.gameLayout.attackTarget) << "\n"
              << "Game.FollowTarget candidate: +" << poff(r.gameLayout.followTarget) << "\n"
              << "Game.Protocol candidate    : +" << poff(r.gameLayout.protocolCandidate) << "\n"
              << "Status                     : " << (r.gameLayout.inferred?"OFFLINE ABI CANDIDATE":"PARTIAL") << "\n";

    std::cout << "\n============================================================\n"
              << "                    RUNTIME VALIDATION\n"
              << "============================================================\n";
    WaitEnter("Entre no personagem e deixe o mundo carregado. Depois ENTER... ");

    if (r.gameOwnerRva && r.gameLayout.localPlayer >= 0) {
        const uintptr_t game = r.module.base + r.gameOwnerRva;
        uint32_t lp = 0;
        if (SafeRead(game + r.gameLayout.localPlayer, lp)) {
            r.localPlayerPtr = lp;
            uint32_t vt = 0;
            if (lp && SafeRead(lp, vt) && r.module.inModule(vt)) {
                r.localPlayerVtableRva = r.module.rva(vt);
                const auto it = r.rtti.find("LocalPlayer");
                if (it != r.rtti.end()) {
                    r.localPlayerVtableMatch = std::find(it->second.vtableRvas.begin(), it->second.vtableRvas.end(),
                                                        r.localPlayerVtableRva) != it->second.vtableRvas.end();
                }
            }
        }
    }

    std::cout << "Game object     : " << (r.gameOwnerRva?HexU64(r.module.base+r.gameOwnerRva):"<pending>") << "\n"
              << "LocalPlayer ptr : " << HexU64(r.localPlayerPtr) << "\n"
              << "LP VTable RVA   : " << HexU32(r.localPlayerVtableRva) << "\n"
              << "RTTI match      : " << (r.localPlayerVtableMatch?"YES":"NO") << "\n";

    bool activitySessionsStarted=false;
    if (r.localPlayerPtr && r.localPlayerVtableMatch) {
        PrintLoadedFieldQuickChecks(r);
        const auto knownField=[&](const std::string& n){ auto it=r.selectedFields.find(n); return it!=r.selectedFields.end() && it->second.offset>=0; };
        const bool numericCoreComplete = knownField("health") && knownField("maxHealth") &&
                                         knownField("experience") && knownField("level") &&
                                         knownField("mana") && knownField("maxMana") &&
                                         knownField("capacityPairA") && knownField("capacityPairB");
        bool runNumeric = false;
        if (numericCoreComplete) {
            std::cout << "\n[SKIP] HP/Mana/XP/Level/Capacity ja existem no profile desta fingerprint.\n";
        } else {
            runNumeric = AskYesNo("Existem campos numericos pendentes. Calibrar HP/Mana/XP/Level/Cap agora?") == 's';
        }
        if (runNumeric) {
            std::map<std::string,double> values;
            values["health"]=AskDouble("Health atual");
            values["maxHealth"]=AskDouble("Health maximo");
            values["mana"]=AskDouble("Mana atual");
            values["maxMana"]=AskDouble("Mana maxima");
            values["experience"]=AskDouble("Experience");
            values["level"]=AskDouble("Level");
            values["capacity"]=AskDouble("Capacity");

            std::cout << "\n[VALUE CANDIDATES]\n";
            for (const auto& kv : values) {
                auto hits = FindNumericValue(r.localPlayerPtr, kv.second);
                r.valueHits[kv.first] = hits;
                std::cout << kv.first << " target=" << kv.second << " hits=" << hits.size() << "\n";
                size_t shown=0;
                for (const auto& h : hits) {
                    std::cout << "  " << h.type << " +" << HexU32(h.offset) << "\n";
                    if (++shown >= 20) break;
                }
                auto sel=SelectPreferredUnique(hits,"unique_typed_value");
                if(sel.offset>=0) r.selectedFields[kv.first]=sel;
            }

            // V5: low-level characters may have Mana==MaxMana and no spell available.
            // Prefer a differential sample when possible; otherwise validate the adjacent
            // pair through one level-up and keep order provenance explicit.
            if (r.selectedFields["mana"].offset < 0 || r.selectedFields["maxMana"].offset < 0) {
                if (AskYesNo("Consegue GASTAR mana sem ganhar level agora?")=='s') {
                    auto pair=ResolveCurrentMaxPairTransition(r.localPlayerPtr,r.valueHits["mana"],r.valueHits["maxMana"],
                                                              values["mana"],values["maxMana"]);
                    if(pair.current.offset>=0 && pair.maximum.offset>=0) {
                        r.selectedFields["mana"]=pair.current;
                        r.selectedFields["maxMana"]=pair.maximum;
                    }
                } else if (AskYesNo("Validar o par Mana/MaxMana por UM level-up?")=='s') {
                    SelectedField levelField;
                    auto lit=r.selectedFields.find("level");
                    if(lit!=r.selectedFields.end()) levelField=lit->second;
                    auto pair=ResolveManaPairByLevelUp(r.localPlayerPtr,r.valueHits["mana"],r.valueHits["maxMana"],
                                                       values["mana"],values["maxMana"],levelField,values["level"]);
                    if(pair.current.offset>=0 && pair.maximum.offset>=0) {
                        r.selectedFields["mana"]=pair.current;
                        r.selectedFields["maxMana"]=pair.maximum;
                    }
                }
            }

            // Capacity V9: refresh Capacity after any level-up, then record an adjacent scaled pair if the client exposes
            // Free/Total together. A weight-only transition can then identify which
            // member is the displayed/free capacity. Level-up is NOT required.
            // A mana-by-level-up test may have changed Capacity after the initial values
            // were entered. Refresh the display NOW so the memory snapshot and the
            // typed value refer to the same instant. V4 used the stale pre-level value.
            const double capacityNow = AskDouble("Capacity atual AGORA (apos qualquer level-up)");
            values["capacity"] = capacityNow;
            auto capPair=FindScaledCapacityPair(r.localPlayerPtr,capacityNow);
            if(capPair.first.offset>=0 && capPair.second.offset>=0) {
                r.selectedFields["capacityPairA"]=capPair.first;
                r.selectedFields["capacityPairB"]=capPair.second;
                PrintMemberOffsetRefsV9(r.module,capPair.first.offset,"CapacityPairA");
                PrintMemberOffsetRefsV9(r.module,capPair.second.offset,"CapacityPairB");
                PrintCapacityPairCodeUseV9(r.module,capPair.first.offset,capPair.second.offset);
            }
            if (AskYesNo("Consegue alterar Capacity SEM ganhar level (pegando/largando item)?")=='s') {
                r.selectedFields.erase("capacity");
                auto capResult=ResolveCapacityTransitionV6(r.localPlayerPtr,capacityNow,capPair);
                if(capResult.canonical.offset>=0) r.selectedFields["capacity"]=capResult.canonical;
                else {
                    r.selectedFields["capacity"]={"",-1,"runtime_weight_transition_not_unique"};
                    if(capResult.mirrorPairConfirmed && capPair.first.offset>=0 && capPair.second.offset>=0) {
                        const std::string st = capResult.mirrorPairConfirmedTwice
                            ? "runtime_mirrored_scaled_pair_two_transitions"
                            : "runtime_mirrored_scaled_pair_single_transition";
                        r.selectedFields["capacityPairA"]={capPair.first.type,capPair.first.offset,st};
                        r.selectedFields["capacityPairB"]={capPair.second.type,capPair.second.offset,st};
                        std::cout << "[CAPACITY] nenhum membro isolado como canonico; os dois foram confirmados como espelhos runtime.\n";
                    } else {
                        std::cout << "[CAPACITY] transicao por peso nao separou um campo unico; o par estrutural foi preservado.\n";
                    }
                }
            } else {
                std::cout << "[CAPACITY] sem teste por peso; par estrutural sera salvo sem promover um membro como canonico.\n";
            }

            std::cout << "\n[SELECTED FIELDS]\n";
            for(const auto& name:{std::string("health"),std::string("maxHealth"),std::string("mana"),std::string("maxMana"),std::string("experience"),std::string("level"),std::string("capacity"),std::string("capacityPairA"),std::string("capacityPairB")}) {
                const auto it=r.selectedFields.find(name);
                std::cout << std::left << std::setw(12) << name << std::right;
                if(it!=r.selectedFields.end() && it->second.offset>=0) {
                    std::cout << " " << it->second.type << " +" << HexU32(it->second.offset)
                              << " [" << it->second.status << "]\n";
                } else std::cout << " <pending>\n";
            }
            SaveCheckpoint(r,"numeric_fields");
        }

        if (r.gameOwnerRva) {
            const uintptr_t game = r.module.base + r.gameOwnerRva;
            const bool attackKnown = r.gameLayout.attackTarget >= 0 && EvidenceScore(r.attackTargetSource) >= 90;
            const bool followKnown = r.gameLayout.followTarget >= 0 && EvidenceScore(r.followTargetSource) >= 90;
            if (attackKnown) std::cout << "[SKIP] AttackTarget ja runtime-confirmed no profile: +" << HexU32(r.gameLayout.attackTarget) << "\n";
            if (followKnown) std::cout << "[SKIP] FollowTarget ja runtime-confirmed no profile: +" << HexU32(r.gameLayout.followTarget) << "\n";
            r.gameRuntime = RunGameTargetValidation(game, r.gameLayout.localPlayer, r.module, r.rtti, !attackKnown, !followKnown);
            if (r.gameRuntime.attackTarget >= 0) { r.gameLayout.attackTarget = r.gameRuntime.attackTarget; r.attackTargetSource = "runtime_transition"; }
            if (r.gameRuntime.followTarget >= 0) { r.gameLayout.followTarget = r.gameRuntime.followTarget; r.followTargetSource = "runtime_transition"; }
            r.gameLayout.inferred = r.gameLayout.localPlayer >= 0 && r.gameLayout.attackTarget >= 0 &&
                                    r.gameLayout.followTarget >= 0 &&
                                    r.gameLayout.localPlayer != r.gameLayout.attackTarget &&
                                    r.gameLayout.attackTarget != r.gameLayout.followTarget;
            SaveCheckpoint(r, "game_layout");
            std::cout << "\n[GAME LAYOUT RUNTIME RESULT]\n"
                      << "LocalPlayer : +" << poff(r.gameLayout.localPlayer) << "\n"
                      << "AttackTarget: +" << poff(r.gameLayout.attackTarget)
                      << (EvidenceScore(r.attackTargetSource)>=90?" [runtime/profile]":" [offline]") << "\n"
                      << "FollowTarget: +" << poff(r.gameLayout.followTarget)
                      << (EvidenceScore(r.followTargetSource)>=90?" [runtime/profile]":" [offline]") << "\n";
        }

        uint32_t qx=0,qy=0; uint8_t qz=0;
        const bool loadedPositionOk = EvidenceScore(r.positionSource) >= 90 &&
                                      QuickCheckPosition(r.localPlayerPtr,r.positionRuntime,&qx,&qy,&qz);
        if (loadedPositionOk) {
            std::cout << "\n[POSITION QUICK-CHECK] OK -> " << qx << "," << qy << "," << static_cast<int>(qz)
                      << " offsets +" << HexU32(r.positionRuntime.x) << "/+" << HexU32(r.positionRuntime.y)
                      << "/+" << HexU32(r.positionRuntime.z) << " dir +" << HexU32(r.positionRuntime.direction) << "\n"
                      << "[SKIP] Position calibration ja confirmada para esta fingerprint.\n";
        } else {
            r.positionCandidates = FindPositionCandidates(r.localPlayerPtr);
            std::cout << "\n[POSITION STRUCTURAL CANDIDATES]\n";
            for (const auto& c : r.positionCandidates) {
                std::cout << "  +" << HexU32(c.offset)
                          << " -> " << c.x << "," << c.y << "," << static_cast<int>(c.z)
                          << " score=" << c.score << "\n";
            }
            r.positionRuntime = RunPositionValidation(r.localPlayerPtr, r.positionCandidates);
            if (r.positionRuntime.x>=0 && r.positionRuntime.y>=0 && r.positionRuntime.z>=0)
                r.positionSource = r.positionRuntime.xyzPromotedByAdjacency ? "runtime_transition_plus_xyz_adjacency" : "runtime_transition";
        }
        std::cout << "\n[POSITION RESULT]\n"
                  << "X         : +" << poff(r.positionRuntime.x) << "\n"
                  << "Y         : +" << poff(r.positionRuntime.y) << "\n"
                  << "Z         : +" << poff(r.positionRuntime.z) << "\n"
                  << "Direction : +" << poff(r.positionRuntime.direction) << "\n";
        if (r.positionRuntime.x>=0 && r.positionRuntime.y>=0 && r.positionRuntime.z>=0) SaveCheckpoint(r,"position");

        if (r.mapOwnerRva) {
            const uintptr_t mapObj = r.module.base + r.mapOwnerRva;
            const bool loadedMapOk = r.mapCenterOffset>=0 && EvidenceScore(r.mapCenterStatus)>=80 &&
                                     QuickCheckMapCenter(mapObj,r.localPlayerPtr,r.positionRuntime,r.mapCenterOffset);
            if (loadedMapOk) {
                std::cout << "\n[MAP CENTER QUICK-CHECK] +" << HexU32(r.mapCenterOffset)
                          << " == LocalPlayer Position [OK]\n"
                          << "[SKIP] Map.Center calibration ja confirmada para esta fingerprint.\n";
            } else {
                r.mapCenters = FindMapCenter(mapObj,r.localPlayerPtr,r.positionRuntime);
                std::cout << "\n[MAP CENTER CANDIDATES]\n";
                if (r.mapCenters.empty()) std::cout << "  <none>\n";
                for (const auto& c : r.mapCenters) std::cout << "  +" << HexU32(c.offset) << " (X/Y/Z contiguous)\n";
                if (!r.mapCenters.empty() && AskYesNo("Validar Map.Center por transicoes runtime agora?")=='s') {
                    r.mapCenterOffset = ValidateMapCenterV3(mapObj, r.mapCenters, r.mapCenterStatus);
                }
            }
            std::cout << "[MAP CENTER RESULT] "
                      << (r.mapCenterOffset>=0 ? ("+"+HexU32(r.mapCenterOffset)) : std::string("<pending>"))
                      << " [" << r.mapCenterStatus << "]\n";
            if (r.mapCenterOffset>=0) SaveCheckpoint(r,"map_center");
        }

        RunGuidedDiscoveryValidation(r);
        activitySessionsStarted=true;
        if(AskYesNo("Executar tambem a descoberta detalhada de layouts/ABI (testes adicionais)?") == 's') {
        RunCreatureLayoutDiscovery(r);
        RunCreatureIdDiscoveryV18(r);
        RunCreatureSemanticFieldsV18(r);
        RunLocalPlayerSemanticFieldsV18(r);
        RunInventoryItemDiscoveryV24(r);
        RunTileGroundDiscoveryV30(r);
        RunGameActionAbiProbeV32(r);
        RunAutoWalkDeepAbiProbeV40(r);
        // Group entry observations never imply a complete ABI.
        RunOpenContainerDiscoveryV24Multi(r);
        RunWorldAmbientLightDiscovery(r);
        }
    } else {
        std::cout << "\n[NOTE] LocalPlayer nao foi promovido. O probe preserva os resultados offline\n"
                  << "       e nao executa scan cego de heap como fallback.\n";
    }

    if(!activitySessionsStarted)RunGuidedDiscoveryValidation(r);
    RunTopMoveAbiBaseline(r);
    V33Disarm();
    if(g_v33Veh) { RemoveVectoredExceptionHandler(g_v33Veh); g_v33Veh=nullptr; }
    r.checkpoint = "complete";
    const std::string saved = SaveJson(r);
    std::cout << "\n============================================================\n"
              << "                         RESULT\n"
              << "============================================================\n"
              << "RTTI LocalPlayer : " << (!r.rtti["LocalPlayer"].vtableRvas.empty()?"FOUND":"MISSING") << "\n"
              << "Game owner       : " << (r.gameOwnerRva?"FOUND":"MISSING") << "\n"
              << "Map owner        : " << (r.mapOwnerRva?"FOUND":"MISSING") << "\n"
              << "Attack/Follow    : " << ((attackRva&&followRva)?"FOUND":"PARTIAL") << "\n"
              << "Game layout      : " << (r.gameLayout.inferred?"INFERRED":"PARTIAL") << "\n"
              << "Runtime LP       : " << (r.localPlayerVtableMatch?"RTTI CONFIRMED":"NOT CONFIRMED") << "\n"
              << "Map center       : " << (r.mapCenterOffset>=0?("+"+HexU32(r.mapCenterOffset)):std::string("PENDING"))
              << " [" << r.mapCenterStatus << "]\n"
              << "Mana pair        : " << ((r.selectedFields.count("mana") && r.selectedFields["mana"].offset>=0 &&
                                               r.selectedFields.count("maxMana") && r.selectedFields["maxMana"].offset>=0)?"CONFIRMED":"PENDING") << "\n"
              << "Capacity         : " << ((r.selectedFields.count("capacity") && r.selectedFields["capacity"].offset>=0)?"CANONICAL CONFIRMED":"NO SINGLE CANONICAL") << "\n"
              << "Capacity pair    : " << ((r.selectedFields.count("capacityPairA") && r.selectedFields["capacityPairA"].offset>=0 &&
                                               r.selectedFields.count("capacityPairB") && r.selectedFields["capacityPairB"].offset>=0)
                                              ? ((r.selectedFields["capacityPairA"].status.find("runtime_mirrored")!=std::string::npos &&
                                                  r.selectedFields["capacityPairB"].status.find("runtime_mirrored")!=std::string::npos)
                                                 ? "CONFIRMED MIRROR" : "FOUND")
                                              : "PENDING") << "\n"
              << "Creature Position: " << (r.creatureLayout.x>=0?"CONFIRMED":"PENDING") << "\n"
              << "Creature Name    : " << (r.creatureLayout.nameOffset>=0?"CONFIRMED":"PENDING") << "\n"
              << "Creature HP%     : " << (r.creatureLayout.hpPercent.offset>=0?"CONFIRMED":"PENDING") << "\n"
              << "Creature ID      : " << (r.creatureLayout.id.offset>=0?"CONFIRMED":"PENDING") << "\n"
              << "Creature Speed   : " << (r.creatureLayout.speed.offset>=0?("+"+HexU32(static_cast<uint32_t>(r.creatureLayout.speed.offset))):std::string("PENDING")) << "\n"
              << "Creature BaseSpd : " << (r.creatureLayout.baseSpeed.offset>=0?("+"+HexU32(static_cast<uint32_t>(r.creatureLayout.baseSpeed.offset))):std::string("PENDING")) << "\n"
              << "Magic Level      : " << ((r.selectedFields.count("magicLevel") && r.selectedFields["magicLevel"].offset>=0)?("+"+HexU32(static_cast<uint32_t>(r.selectedFields["magicLevel"].offset))):std::string("PENDING")) << "\n"
              << "States           : " << ((r.selectedFields.count("states") && r.selectedFields["states"].offset>=0)?("+"+HexU32(static_cast<uint32_t>(r.selectedFields["states"].offset))):std::string("PENDING")) << "\n"
              << "Soul             : " << ((r.selectedFields.count("soul") && r.selectedFields["soul"].offset>=0)?("+"+HexU32(static_cast<uint32_t>(r.selectedFields["soul"].offset))):std::string("PENDING")) << "\n"
              << "Stamina          : " << ((r.selectedFields.count("stamina") && r.selectedFields["stamina"].offset>=0)?("+"+HexU32(static_cast<uint32_t>(r.selectedFields["stamina"].offset))):std::string("PENDING")) << "\n"
              << "Creature Skull   : " << (r.creatureLayout.skull.offset>=0?("+"+HexU32(static_cast<uint32_t>(r.creatureLayout.skull.offset))):std::string("PENDING")) << "\n"
              << "Creature Shield  : " << (r.creatureLayout.shield.offset>=0?("+"+HexU32(static_cast<uint32_t>(r.creatureLayout.shield.offset))):std::string("PENDING")) << "\n"
              << "Creature Outfit  : " << (r.creatureLayout.outfitOffset>=0?("+"+HexU32(static_cast<uint32_t>(r.creatureLayout.outfitOffset))):std::string("PENDING")) << "\n"
              << "Inventory Array  : " << ((r.selectedFields.count("inventoryArray") && r.selectedFields["inventoryArray"].offset>=0)?("+"+HexU32(static_cast<uint32_t>(r.selectedFields["inventoryArray"].offset))):std::string("PENDING")) << "\n"
              << "Inventory Stride : " << ((r.selectedFields.count("inventoryStride") && r.selectedFields["inventoryStride"].offset>0)?std::to_string(r.selectedFields["inventoryStride"].offset):std::string("PENDING")) << "\n"
              << "Item ID          : " << ((r.selectedFields.count("itemId") && r.selectedFields["itemId"].offset>=0)?("+"+HexU32(static_cast<uint32_t>(r.selectedFields["itemId"].offset))):std::string("PENDING")) << "\n"
              << "Item Count       : " << ((r.selectedFields.count("itemCount") && r.selectedFields["itemCount"].offset>=0)?("+"+HexU32(static_cast<uint32_t>(r.selectedFields["itemCount"].offset))):std::string("PENDING")) << "\n"
              << "Tile Position    : NOT REQUIRED (x64-style Tile::things locator)\n"
              << "Tile VTable RVA  : " << ((r.selectedFields.count("tileVtableRva") && r.selectedFields["tileVtableRva"].offset>=0)?HexU32(static_cast<uint32_t>(r.selectedFields["tileVtableRva"].offset)):std::string("PENDING")) << "\n"
              << "Tile Things      : " << ((r.selectedFields.count("tileThings") && r.selectedFields["tileThings"].offset>=0)?("+"+HexU32(static_cast<uint32_t>(r.selectedFields["tileThings"].offset))):std::string("PENDING")) << "\n"
              << "Tile Thing Elem  : " << ((r.selectedFields.count("tileThingsElem") && r.selectedFields["tileThingsElem"].offset>0)?std::to_string(r.selectedFields["tileThingsElem"].offset):std::string("PENDING")) << "\n"
              << "OpenContainers   : " << ((r.selectedFields.count("openContainersMap") && r.selectedFields["openContainersMap"].offset>=0)?("Game+"+HexU32(static_cast<uint32_t>(r.selectedFields["openContainersMap"].offset))):std::string("PENDING")) << "\n"
              << "Container Items  : " << ((r.selectedFields.count("containerItems") && r.selectedFields["containerItems"].offset>=0)?("+"+HexU32(static_cast<uint32_t>(r.selectedFields["containerItems"].offset))):std::string("PENDING")) << "\n"
              << "Container ID     : " << ((r.selectedFields.count("containerId") && r.selectedFields["containerId"].offset>=0)?("+"+HexU32(static_cast<uint32_t>(r.selectedFields["containerId"].offset))):std::string("PENDING")) << "\n"
              << "Container Cap    : " << ((r.selectedFields.count("containerCapacity") && r.selectedFields["containerCapacity"].offset>=0)?("+"+HexU32(static_cast<uint32_t>(r.selectedFields["containerCapacity"].offset))):std::string("PENDING")) << "\n"
              << "Container Name   : " << ((r.selectedFields.count("containerName") && r.selectedFields["containerName"].offset>=0)?("+"+HexU32(static_cast<uint32_t>(r.selectedFields["containerName"].offset))):std::string("PENDING")) << "\n"
              << "UIMap->MapView   : " << (r.worldLight.uiMapToMapView>=0?("+"+HexU32(static_cast<uint32_t>(r.worldLight.uiMapToMapView))):std::string("PENDING")) << "\n"
              << "Draw Lights      : " << (r.worldLight.drawLights.offset>=0?("+"+HexU32(static_cast<uint32_t>(r.worldLight.drawLights.offset))):std::string("PENDING")) << "\n"
              << "World Ambient    : " << (r.worldLight.minimumAmbient.offset>=0?("+"+HexU32(static_cast<uint32_t>(r.worldLight.minimumAmbient.offset))):
                                               (r.worldLight.candidateA>=0?"CANDIDATE":"PENDING")) << "\n"
              << "PROFILE JSON     : " << (saved.empty()?"<save failed>":saved) << "\n"
              << "============================================================\n";

    observation_summary::basics(r);
    observation_summary::finish();
    probe_console::ui()<<"\nSessao encerrada. Resultados das atividades exibidos acima.\n"
        <<"Funcoes observadas continuam aguardando validacao da ABI.\n"
        <<"Profile: "<<saved<<"\nDetalhes: ResearchProbeDiagnostic.log\n";
    WaitEnter("ENTER para encerrar... ");
    presentation.close();
    if(grouped_observation::veh){FreeConsole();return 0;} // Retain delayed INT3 handler until client shutdown.
    FreeLibraryAndExitThread(g_self, 0);
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = hModule;
        DisableThreadLibraryCalls(hModule);
        HANDLE h = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
