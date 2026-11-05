#include "../../common/PipeDiscoveryCatalog.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <sstream>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <iterator>
#include <cctype>
#include <filesystem>
#include <streambuf>
#include <stdexcept>
#include "../../../../Crome/Developer/native_runtime/profiles/MiniJson.hpp"

static_assert(sizeof(void*) == 8, "OTCLab ABI Builder V1.3 is x64-only");

namespace fs = std::filesystem;

// Keep filesystem paths in their native UTF-16 representation on Windows.
// UTF-8 is only for display, never passed back to filesystem constructors.
static std::string DisplayPath(const fs::path& path){
    const auto utf8=path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()),utf8.size());
}

static HMODULE g_self = nullptr;
static uintptr_t g_base = 0;
static size_t g_imageSize = 0;
static HANDLE g_startupLog = INVALID_HANDLE_VALUE;

static void LogStage(const char* stage) {
    if(g_startupLog==INVALID_HANDLE_VALUE) return;
    SYSTEMTIME now{}; GetLocalTime(&now);
    char line[512]{};
    const int n=snprintf(line,sizeof(line),"[%02u:%02u:%02u] PID=%lu %s\r\n",
        now.wHour,now.wMinute,now.wSecond,GetCurrentProcessId(),stage);
    if(n>0){DWORD written=0;WriteFile(g_startupLog,line,static_cast<DWORD>(std::min(n,511)),&written,nullptr);FlushFileBuffers(g_startupLog);}
}

static void OpenStartupLog() {
    wchar_t path[32768]{};
    DWORD n=GetModuleFileNameW(g_self,path,32768);
    if(!n||n>=32768) return;
    wchar_t* slash=wcsrchr(path,L'\\'); if(!slash) return;
    const wchar_t name[]=L"ABIBuilderStartup.log";
    if(static_cast<size_t>(slash-path)+1+std::size(name)>32768) return;
    memcpy(slash+1,name,sizeof(name));
    g_startupLog=CreateFileW(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,
        nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    LogStage("worker_entered; build=isolated_console_unicode_v2");
}

// Use owned Win32 console handles. freopen(stdout/stderr/stdin) also changes
// the shared CRT streams used by the injected host and other loaded tools.
class ConsoleOutputBuffer : public std::streambuf {
    HANDLE handle_;
protected:
    std::streamsize xsputn(const char* s,std::streamsize n) override {
        DWORD written=0;
        if(n<=0) return 0;
        if(g_startupLog!=INVALID_HANDLE_VALUE){DWORD logged=0;WriteFile(g_startupLog,s,static_cast<DWORD>(n),&logged,nullptr);}
        return WriteFile(handle_,s,static_cast<DWORD>(n),&written,nullptr)?written:0;
    }
    int_type overflow(int_type ch) override {
        if(traits_type::eq_int_type(ch,traits_type::eof())) return traits_type::not_eof(ch);
        char c=traits_type::to_char_type(ch);
        return xsputn(&c,1)==1?ch:traits_type::eof();
    }
public:
    explicit ConsoleOutputBuffer(HANDLE handle):handle_(handle){}
};

class ConsoleInputBuffer : public std::streambuf {
    HANDLE handle_; char bytes_[256]{};
protected:
    int_type underflow() override {
        if(gptr()&&gptr()<egptr()) return traits_type::to_int_type(*gptr());
        for(;;){
            DWORD read=0;
            if(!ReadFile(handle_,bytes_,sizeof(bytes_),&read,nullptr)||!read) return traits_type::eof();
            // Match CRT text mode so ENTER preserves the guided default.
            DWORD kept=0;for(DWORD i=0;i<read;++i)if(bytes_[i]!='\r')bytes_[kept++]=bytes_[i];
            if(!kept)continue;
            setg(bytes_,bytes_,bytes_+kept);
            return traits_type::to_int_type(*gptr());
        }
    }
public:
    explicit ConsoleInputBuffer(HANDLE handle):handle_(handle){}
};

class ScopedConsoleStreams {
    ConsoleInputBuffer input_; ConsoleOutputBuffer output_;
    std::streambuf *oldIn_,*oldOut_,*oldErr_;
public:
    ScopedConsoleStreams(HANDLE input,HANDLE output):input_(input),output_(output),
        oldIn_(std::cin.rdbuf(&input_)),oldOut_(std::cout.rdbuf(&output_)),oldErr_(std::cerr.rdbuf(&output_)){}
    ~ScopedConsoleStreams(){std::cin.rdbuf(oldIn_);std::cout.rdbuf(oldOut_);std::cerr.rdbuf(oldErr_);}
};

static std::string Hex(uint64_t v) {
    std::ostringstream s; s << "0x" << std::uppercase << std::hex << v; return s.str();
}

static std::string EscapeJson(const std::string& s) {
    std::string o; o.reserve(s.size()+8);
    for(char c: s) {
        switch(c) {
            case '\\': o += "\\\\"; break;
            case '"': o += "\\\""; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default: o += c; break;
        }
    }
    return o;
}

static std::string ReadText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if(!f) return {};
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

static bool WriteText(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if(!f) return false;
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return !!f;
}

static std::string FlatJsonString(const std::string& text, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    size_t p = text.find(token);
    if(p == std::string::npos) return {};
    size_t c = text.find(':', p + token.size());
    if(c == std::string::npos) return {};
    size_t q = text.find('"', c + 1);
    if(q == std::string::npos) return {};
    std::string v; bool esc=false;
    for(size_t i=q+1;i<text.size();++i){
        char ch=text[i];
        if(esc){ v += ch; esc=false; continue; }
        if(ch=='\\'){ esc=true; continue; }
        if(ch=='"') return v;
        v += ch;
    }
    return {};
}

static uint64_t ParseNum(const std::string& s) {
    if(s.empty()) return 0;
    try { return std::stoull(s, nullptr, 0); } catch(...) { return 0; }
}

static std::string ExtractNamedObject(const std::string& json, const std::string& key) {
    const std::string token = "\"" + key + "\"";
    size_t kp = json.find(token);
    if(kp == std::string::npos) return {};
    size_t colon = json.find(':', kp + token.size());
    if(colon == std::string::npos) return {};
    size_t start = json.find('{', colon + 1);
    if(start == std::string::npos) return {};
    bool inString=false, esc=false; int depth=0;
    for(size_t i=start;i<json.size();++i){
        char ch=json[i];
        if(inString){
            if(esc){esc=false;continue;}
            if(ch=='\\'){esc=true;continue;}
            if(ch=='"') inString=false;
            continue;
        }
        if(ch=='"'){inString=true;continue;}
        if(ch=='{') ++depth;
        else if(ch=='}') { --depth; if(depth==0) return json.substr(start, i-start+1); }
    }
    return {};
}

static bool ReplaceOrInsertRootObject(std::string& json, const std::string& key, const std::string& objectJson) {
    const std::string token = "\"" + key + "\"";
    size_t kp = json.find(token);
    if(kp != std::string::npos) {
        size_t colon = json.find(':', kp + token.size());
        if(colon == std::string::npos) return false;
        size_t start = json.find('{', colon + 1);
        if(start == std::string::npos) return false;
        bool inString=false, esc=false; int depth=0;
        size_t end=std::string::npos;
        for(size_t i=start;i<json.size();++i){
            char ch=json[i];
            if(inString){ if(esc){esc=false;continue;} if(ch=='\\'){esc=true;continue;} if(ch=='"') inString=false; continue; }
            if(ch=='"'){inString=true;continue;}
            if(ch=='{') ++depth;
            else if(ch=='}'){ --depth; if(depth==0){ end=i+1; break; } }
        }
        if(end==std::string::npos) return false;
        json.replace(kp, end-kp, "\""+key+"\": "+objectJson);
        return true;
    }
    size_t rootEnd = json.find_last_of('}');
    if(rootEnd == std::string::npos) return false;
    size_t prev = rootEnd;
    while(prev>0 && isspace(static_cast<unsigned char>(json[prev-1]))) --prev;
    bool needComma = prev>0 && json[prev-1] != '{' && json[prev-1] != ',';
    std::string ins = (needComma ? ",\n" : "\n") + std::string("  \"") + key + "\": " + objectJson + "\n";
    json.insert(rootEnd, ins);
    return true;
}

struct Fingerprint {
    uint32_t timestamp=0, sizeOfImage=0, entryPoint=0, textRva=0, textSize=0;
    uint64_t textHash=0;
};

static uint64_t Fnv1a64(const uint8_t* p, size_t n) {
    uint64_t h=14695981039346656037ull;
    for(size_t i=0;i<n;++i){ h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static bool GetFingerprint(Fingerprint& fp) {
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if(!g_base) return false;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    if(dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    if(nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 || nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    fp.timestamp = nt->FileHeader.TimeDateStamp;
    fp.sizeOfImage = nt->OptionalHeader.SizeOfImage;
    fp.entryPoint = nt->OptionalHeader.AddressOfEntryPoint;
    g_imageSize = fp.sizeOfImage;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i){
        char name[9]{}; memcpy(name, sec[i].Name, 8);
        if(std::string(name)==".text"){
            fp.textRva = sec[i].VirtualAddress;
            // Match BuildExplorer's mapped .text fingerprint definition.
            fp.textSize = std::max(sec[i].Misc.VirtualSize,sec[i].SizeOfRawData);
            if(fp.textRva + fp.textSize <= fp.sizeOfImage)
                fp.textHash = Fnv1a64(reinterpret_cast<const uint8_t*>(g_base + fp.textRva), fp.textSize);
            break;
        }
    }
    return fp.timestamp && fp.sizeOfImage;
}

static bool Readable(uintptr_t a, size_t n) {
    if(!a || !n) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if(!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) return false;
    if(mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || mbi.Protect == PAGE_NOACCESS) return false;
    DWORD p=mbi.Protect & 0xFF;
    bool ok = p==PAGE_READONLY || p==PAGE_READWRITE || p==PAGE_WRITECOPY || p==PAGE_EXECUTE_READ || p==PAGE_EXECUTE_READWRITE || p==PAGE_EXECUTE_WRITECOPY;
    if(!ok) return false;
    uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return a+n <= end;
}

template<class T> static bool SafeRead(uintptr_t a, T& out) {
    if(!Readable(a,sizeof(T))) return false;
    memcpy(&out, reinterpret_cast<const void*>(a), sizeof(T)); return true;
}

static bool IsExecutable(uintptr_t a) {
    MEMORY_BASIC_INFORMATION mbi{};
    if(!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) return false;
    DWORD p=mbi.Protect & 0xFF;
    return mbi.State==MEM_COMMIT && !(mbi.Protect&PAGE_GUARD) && (p==PAGE_EXECUTE || p==PAGE_EXECUTE_READ || p==PAGE_EXECUTE_READWRITE || p==PAGE_EXECUTE_WRITECOPY);
}

struct Profile {
    fs::path path;
    std::string raw;
    uint32_t timestamp=0, sizeOfImage=0;
    uint64_t textHash=0;
    uintptr_t directGlobalRva=0, indirectGlobalRva=0, ownerOffset=0;
    uintptr_t x=0,y=0,z=0,direction=0;
    uintptr_t gameRva=0, gameAttackTarget=0, gameFollowTarget=0;
    uintptr_t itemVtableRva=0, monsterVtableRva=0, playerVtableRva=0, npcVtableRva=0, localPlayerVtableRva=0;
    uintptr_t itemIdOffset=0, itemCountOffset=0, itemCountWidth=0;
    uintptr_t inventoryBaseOffset=0, inventorySlotStride=0, gameContainersOffset=0;
    std::string sigGameAutoWalk, sigLocalPlayerAutoWalk, sigGameAttack, sigGameFollow, sigGameLook, sigGameMove, sigGameUse, sigGameUseWith;
    uintptr_t fnGameAutoWalk=0, fnLocalPlayerAutoWalk=0, fnGameAttack=0, fnGameFollow=0;
    uintptr_t fnGameLook=0, fnGameDetailedLook=0, fnGameMove=0, fnGameUse=0, fnGameUseWith=0, fnGameTalk=0, fnGameTalkChannel=0;
    uintptr_t fnMapGetTile=0, fnMapGetCreatureById=0, fnMapGetCreatureByName=0, fnContainerGetItem=0;
};


static bool DiscoveryFingerprintMatches(const Profile& p) {
    const auto cat=ExtractNamedObject(p.raw,"pipeDiscovery");
    const auto fp=ExtractNamedObject(cat,"fingerprint");
    return FlatJsonString(cat,"schema")==pipe_discovery::schema &&
        FlatJsonString(cat,"architecture")=="x64" &&
        pipe_discovery::exactFingerprint(ParseNum(FlatJsonString(fp,"timestamp")),
            ParseNum(FlatJsonString(fp,"sizeOfImage")),ParseNum(FlatJsonString(fp,"textHashFNV1A64")),
            p.timestamp,p.sizeOfImage,p.textHash);
}
static uintptr_t DiscoveredRva(const Profile& p,const std::string& key) {
    if(!DiscoveryFingerprintMatches(p)) return 0;
    const auto obj=ExtractNamedObject(ExtractNamedObject(ExtractNamedObject(p.raw,"pipeDiscovery"),"functions"),key);
    if(FlatJsonString(obj,"status")!="unique_bind_candidate") return 0;
    const auto countKey=obj.find("\"candidateCount\"");
    if(countKey==std::string::npos) return 0;
    const auto colon=obj.find(':',countKey);
    if(colon==std::string::npos) return 0;
    try {
        std::size_t used=0;
        if(std::stoull(obj.substr(colon+1),&used)!=1) return 0;
        const auto tail=obj.find_first_not_of(" \t\r\n",colon+1+used);
        if(tail==std::string::npos || (obj[tail]!=',' && obj[tail]!='}')) return 0;
    } catch(...) { return 0; }
    const auto rva=ParseNum(FlatJsonString(obj,"rva"));
    return rva && rva<p.sizeOfImage ? static_cast<uintptr_t>(rva) : 0;
}
static std::string ForcePassive(std::string obj) {
    for(const char* field:{"callable","contractComplete","activeCallValidated"}) {
        const std::string key=std::string("\"")+field+"\"";
        const auto at=obj.find(key);
        if(at==std::string::npos) { obj.insert(obj.find('{')+1,key+":false,"); continue; }
        auto value=obj.find(':',at+key.size())+1;
        while(value<obj.size() && std::isspace(static_cast<unsigned char>(obj[value]))) ++value;
        if(obj.compare(value,4,"true")==0) obj.replace(value,4,"false");
    }
    return obj;
}

static Profile ParseProfile(const fs::path& path) {
    Profile p; p.path=path; p.raw=ReadText(path);
    std::string m=ExtractNamedObject(p.raw,"migratorState"); if(m.empty()) m=p.raw;
    auto N=[&](const char* k)->uintptr_t{return static_cast<uintptr_t>(ParseNum(FlatJsonString(m,k)));};
    p.timestamp=static_cast<uint32_t>(N("timestamp")); p.sizeOfImage=static_cast<uint32_t>(N("sizeOfImage")); p.textHash=ParseNum(FlatJsonString(m,"textHashFNV1A64"));
    p.directGlobalRva=N("directGlobalRva"); p.indirectGlobalRva=N("indirectGlobalRva"); p.ownerOffset=N("ownerOffset");
    p.x=N("x");p.y=N("y");p.z=N("z");p.direction=N("direction");
    p.gameRva=N("gameRva");p.gameAttackTarget=N("gameAttackTarget");p.gameFollowTarget=N("gameFollowTarget");
    p.itemVtableRva=N("itemVtableRva");p.monsterVtableRva=N("monsterVtableRva");p.playerVtableRva=N("playerVtableRva");p.npcVtableRva=N("npcVtableRva");p.localPlayerVtableRva=N("vtableRva");
    p.itemIdOffset=N("itemIdOffset");p.itemCountOffset=N("itemCountOffset");p.itemCountWidth=N("itemCountWidth");
    p.inventoryBaseOffset=N("inventoryBaseOffset");p.inventorySlotStride=N("inventorySlotStride");p.gameContainersOffset=N("gameContainersOffset");
    p.sigGameAutoWalk=FlatJsonString(m,"sigGameAutoWalk");p.sigLocalPlayerAutoWalk=FlatJsonString(m,"sigLocalPlayerAutoWalk");p.sigGameAttack=FlatJsonString(m,"sigGameAttack");p.sigGameFollow=FlatJsonString(m,"sigGameFollow");p.sigGameLook=FlatJsonString(m,"sigGameLook");p.sigGameMove=FlatJsonString(m,"sigGameMove");p.sigGameUse=FlatJsonString(m,"sigGameUse");p.sigGameUseWith=FlatJsonString(m,"sigGameUseWith");
    p.fnGameAutoWalk=N("fnGameAutoWalk");p.fnLocalPlayerAutoWalk=N("fnLocalPlayerAutoWalk");p.fnGameAttack=N("fnGameAttack");p.fnGameFollow=N("fnGameFollow");
    p.fnGameLook=N("fnGameLook");p.fnGameDetailedLook=N("fnGameDetailedLook");p.fnGameMove=N("fnGameMove");p.fnGameUse=N("fnGameUse");p.fnGameUseWith=N("fnGameUseWith");p.fnGameTalk=N("fnGameTalk");
    p.fnMapGetTile=N("fnMapGetTile");p.fnMapGetCreatureById=N("fnMapGetCreatureById");p.fnMapGetCreatureByName=N("fnMapGetCreatureByName");p.fnContainerGetItem=N("fnContainerGetItem");
    // Fresh discovery is mandatory. Legacy/reference RVAs cannot authorize captures.
    p.fnGameAutoWalk=DiscoveredRva(p,"gameAutoWalk");
    p.fnLocalPlayerAutoWalk=DiscoveredRva(p,"localPlayerAutoWalk");
    p.fnGameAttack=DiscoveredRva(p,"gameAttack");
    p.fnGameFollow=DiscoveredRva(p,"gameFollow");
    p.fnGameLook=DiscoveredRva(p,"gameLook");
    p.fnGameDetailedLook=DiscoveredRva(p,"gameDetailedLook");
    p.fnGameMove=DiscoveredRva(p,"gameMove");
    p.fnGameUse=DiscoveredRva(p,"gameUse");
    p.fnGameUseWith=DiscoveredRva(p,"gameUseWith");
    p.fnGameTalk=DiscoveredRva(p,"gameTalk");
    p.fnGameTalkChannel=DiscoveredRva(p,"gameTalkChannel");
    p.fnMapGetTile=DiscoveredRva(p,"mapGetTile");
    p.fnMapGetCreatureById=DiscoveredRva(p,"mapGetCreatureById");
    p.fnMapGetCreatureByName=DiscoveredRva(p,"mapGetCreatureByName");
    p.fnContainerGetItem=DiscoveredRva(p,"containerGetItem");
    return p;
}

static int ProfileEvidenceScore(const Profile& p){
    std::string abi=ExtractNamedObject(p.raw,"abiCatalog");if(abi.empty())return 0;int score=24;
    auto count=[&](const std::string& needle){int n=0;size_t pos=0;while((pos=abi.find(needle,pos))!=std::string::npos){++n;pos+=needle.size();}return n;};
    score+=std::min(24,count("confirmed_runtime")*2);
    score+=std::min(16,count("strong_candidate")*2);
    score+=std::min(12,count("\"callerCandidateReady\": true")*3);
    score+=std::min(12,count("\"callerSiteValidated\": true")*3);
    std::string low=DisplayPath(p.path);std::transform(low.begin(),low.end(),low.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    if(low.find("profiles")!=std::string::npos)score+=2;return score;
}

static fs::path DllDir() {
    wchar_t buf[32768]{};const DWORD n=GetModuleFileNameW(g_self,buf,32768);
    if(!n||n>=32768)throw std::runtime_error("Cannot resolve ABI DLL directory");
    return fs::path(buf).parent_path();
}

static std::vector<fs::path> FindProfilesInDirectories(const std::vector<fs::path>& dirs) {
    std::set<fs::path> seen; std::vector<fs::path> out;
    for(const auto& d:dirs){
        std::error_code ec; if(!fs::exists(d,ec)||!fs::is_directory(d,ec)) continue;
        for(auto& e:fs::directory_iterator(d,ec)){
            if(ec) break; if(!e.is_regular_file(ec)||ec) continue;
            auto ext=e.path().extension().native();
            std::transform(ext.begin(),ext.end(),ext.begin(),[](wchar_t c){return c>=L'A'&&c<=L'Z'?c-L'A'+L'a':c;});
            if(ext!=L".json") continue;
            if(seen.insert(e.path()).second) out.push_back(e.path());
        }
    }
    return out;
}

static std::vector<fs::path> FindProfiles() {
    const auto dir=DllDir();std::error_code ec;
    std::vector<fs::path> dirs={dir,dir/L"profiles",dir.parent_path(),dir.parent_path()/L"profiles"};
    auto current=fs::current_path(ec);if(!ec)dirs.push_back(current);
    return FindProfilesInDirectories(dirs);
}

static bool AskYes(const std::string& q, bool def=false) {
    std::cout<<q<<(def?" [S/n]: ":" [S/N]: "); std::string s; std::getline(std::cin,s);
    if(s.empty()) return def; char c=static_cast<char>(tolower(static_cast<unsigned char>(s[0]))); return c=='s'||c=='y';
}

static void Wait(const std::string& q) { std::cout<<q; std::string s; std::getline(std::cin,s); }

static uint32_t AskU32(const std::string& q, uint32_t def=0) {
    for(;;){ std::cout<<q; if(def) std::cout<<" [ENTER="<<def<<"]"; std::cout<<": "; std::string s; std::getline(std::cin,s); if(s.empty()&&def) return def; try{return static_cast<uint32_t>(std::stoul(s,nullptr,0));}catch(...){std::cout<<"Valor invalido.\n";} }
}

static uintptr_t ResolveLocalPlayer(const Profile& p) {
    uintptr_t lp=0;
    if(p.directGlobalRva && SafeRead(g_base+p.directGlobalRva,lp) && lp) return lp;
    if(p.indirectGlobalRva && p.ownerOffset){ uintptr_t owner=0; if(SafeRead(g_base+p.indirectGlobalRva,owner)&&owner) SafeRead(owner+p.ownerOffset,lp); }
    return lp;
}

static uintptr_t ReadGameTarget(const Profile& p, uintptr_t off) {
    if(!p.gameRva || !off) return 0;
    uintptr_t v=0; SafeRead(g_base+p.gameRva+off,v); return v;
}

static bool ReadItemId(uintptr_t thing, const Profile& p, uint32_t& id) {
    id=0; if(!thing||!p.itemIdOffset) return false;
    if(p.itemVtableRva){ uintptr_t vt=0; if(!SafeRead(thing,vt)||vt!=g_base+p.itemVtableRva) return false; }
    if(!SafeRead(thing+p.itemIdOffset,id)) return false;
    return id>0 && id<1000000;
}

static bool ReadItemCount(uintptr_t thing, const Profile& p, uint32_t& count) {
    count=0; if(!thing||!p.itemCountOffset) return false;
    if(p.itemVtableRva){ uintptr_t vt=0; if(!SafeRead(thing,vt)||vt!=g_base+p.itemVtableRva) return false; }
    if(p.itemCountWidth==1){ uint8_t v=0; if(!SafeRead(thing+p.itemCountOffset,v)) return false; count=v; }
    else if(p.itemCountWidth==2){ uint16_t v=0; if(!SafeRead(thing+p.itemCountOffset,v)) return false; count=v; }
    else { uint32_t v=0; if(!SafeRead(thing+p.itemCountOffset,v)) return false; count=v; }
    if(count==0) count=1;
    return count<1000000;
}

static uintptr_t KnownGameAddress(const Profile& p){ return p.gameRva ? g_base+p.gameRva : 0; }

static bool ReadPosition(const Profile& p, uint32_t& x,uint32_t& y,uint32_t& z) {
    uintptr_t lp=ResolveLocalPlayer(p); if(!lp||!p.x||!p.y||!p.z) return false;
    uint8_t z8=0; if(!SafeRead(lp+p.x,x)||!SafeRead(lp+p.y,y)||!SafeRead(lp+p.z,z8)) return false; z=z8; return x>1000&&y>1000&&z<32;
}

constexpr int MAX_CAP=96;
constexpr int STACK_QWORDS=12;
constexpr int ARG_SNAP_BYTES=64;
struct MemSnap { uintptr_t base=0; uint32_t validBytes=0; uint8_t bytes[ARG_SNAP_BYTES]{}; };
enum class NaturalCallKind : uint8_t { Unknown=0, Rel32=1, Indirect=2 };
struct Capture {
    DWORD tid=0; uintptr_t rcx=0,rdx=0,r8=0,r9=0,rsp=0,ret=0; uintptr_t stack[STACK_QWORDS]{};
    MemSnap rcxMem{},rdxMem{},r8Mem{},r9Mem{};
    uintptr_t callSite=0, callerFunction=0, callerFunctionEnd=0;
    NaturalCallKind callKind=NaturalCallKind::Unknown;
};

struct RuntimeFunctionLite { uint32_t beginAddress=0, endAddress=0, unwindData=0; };

static bool InMainModule(uintptr_t a){ return a>=g_base && a<g_base+g_imageSize; }

static bool ResolveContainingFunction(uintptr_t ip, uintptr_t& begin, uintptr_t& end){
    begin=end=0; if(!InMainModule(ip))return false;
    auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    if(!dos||dos->e_magic!=IMAGE_DOS_SIGNATURE)return false;
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(g_base+dos->e_lfanew);
    if(!nt||nt->Signature!=IMAGE_NT_SIGNATURE)return false;
    auto dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if(!dir.VirtualAddress||dir.Size<sizeof(RuntimeFunctionLite)||dir.VirtualAddress+dir.Size>g_imageSize)return false;
    auto* t=reinterpret_cast<const RuntimeFunctionLite*>(g_base+dir.VirtualAddress);
    size_t n=dir.Size/sizeof(RuntimeFunctionLite); uint32_t rva=static_cast<uint32_t>(ip-g_base);
    size_t lo=0,hi=n;
    while(lo<hi){size_t mid=lo+(hi-lo)/2;if(t[mid].beginAddress<=rva)lo=mid+1;else hi=mid;}
    if(lo==0)return false; const auto& e=t[lo-1];
    if(rva<e.beginAddress||rva>=e.endAddress)return false;
    begin=g_base+e.beginAddress;end=g_base+e.endAddress;return true;
}

static bool ParseIndirectCallEndingAt(uintptr_t start, uintptr_t ret){
    if(!start||ret<=start||ret-start>15||!Readable(start,static_cast<size_t>(ret-start)))return false;
    size_t i=0,n=static_cast<size_t>(ret-start);
    auto B=[&](size_t k)->uint8_t{return *reinterpret_cast<const uint8_t*>(start+k);};
    while(i<n){uint8_t b=B(i);if((b>=0x40&&b<=0x4F)||b==0x66||b==0x67||b==0xF2||b==0xF3){++i;continue;}break;}
    if(i>=n||B(i++)!=0xFF||i>=n)return false;
    uint8_t modrm=B(i++);if(((modrm>>3)&7)!=2)return false;uint8_t mod=(modrm>>6)&3,rm=modrm&7;
    if(mod!=3&&rm==4){if(i>=n)return false;uint8_t sib=B(i++);uint8_t base=sib&7;if(mod==0&&base==5)i+=4;else if(mod==1)i+=1;else if(mod==2)i+=4;}
    else {if(mod==0&&rm==5)i+=4;else if(mod==1)i+=1;else if(mod==2)i+=4;}
    return i==n;
}

static void ResolveNaturalCaller(uintptr_t callee,Capture& c){
    if(!c.ret||!InMainModule(c.ret))return;uintptr_t call=0;NaturalCallKind kind=NaturalCallKind::Unknown;
    if(c.ret>=g_base+5&&Readable(c.ret-5,5)){uint8_t op=0;int32_t rel=0;SafeRead(c.ret-5,op);SafeRead(c.ret-4,rel);if(op==0xE8){uintptr_t dst=c.ret+static_cast<int64_t>(rel);if(dst==callee){call=c.ret-5;kind=NaturalCallKind::Rel32;}}}
    if(!call){for(size_t len=2;len<=15&&c.ret>=g_base+len;++len){uintptr_t cand=c.ret-len;if(ParseIndirectCallEndingAt(cand,c.ret)){call=cand;kind=NaturalCallKind::Indirect;}}}
    if(!call)return;c.callSite=call;c.callKind=kind;uintptr_t fb=0,fe=0;if(ResolveContainingFunction(call,fb,fe)){c.callerFunction=fb;c.callerFunctionEnd=fe;}
}

static const char* NaturalCallKindName(NaturalCallKind k){switch(k){case NaturalCallKind::Rel32:return "call_rel32";case NaturalCallKind::Indirect:return "call_indirect";default:return "unknown";}}

static void SnapshotMemory(uintptr_t p, MemSnap& m){
    m={}; m.base=p; if(!p)return;
    for(uint32_t off=0;off<ARG_SNAP_BYTES;off+=8){ uintptr_t q=0; if(!SafeRead(p+off,q))break; memcpy(m.bytes+off,&q,sizeof(q)); m.validBytes=off+8; }
}

template<class T> static bool SnapRead(const MemSnap& m,uint32_t off,T& out){
    if(off+sizeof(T)>m.validBytes)return false; memcpy(&out,m.bytes+off,sizeof(T)); return true;
}

struct BPState { uintptr_t target=0; uint8_t original=0; PVOID veh=nullptr; volatile LONG installed=0,rearmTid=0,count=0; Capture caps[MAX_CAP]{}; };
static BPState g_bp{};

static bool PatchByte(uintptr_t a,uint8_t b){ DWORD old=0;if(!VirtualProtect(reinterpret_cast<void*>(a),1,PAGE_EXECUTE_READWRITE,&old))return false;*reinterpret_cast<volatile uint8_t*>(a)=b;FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void*>(a),1);DWORD tmp=0;VirtualProtect(reinterpret_cast<void*>(a),1,old,&tmp);return true; }

static LONG CALLBACK Veh(PEXCEPTION_POINTERS info){
    if(!info||!info->ExceptionRecord||!info->ContextRecord||!InterlockedCompareExchange(&g_bp.installed,0,0)) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c=info->ContextRecord; DWORD tid=GetCurrentThreadId(); DWORD code=info->ExceptionRecord->ExceptionCode;
    if(code==EXCEPTION_BREAKPOINT && reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress)==g_bp.target){
        LONG n=InterlockedIncrement(&g_bp.count)-1;
        if(n>=0&&n<MAX_CAP){ Capture& x=g_bp.caps[n]; x={};x.tid=tid;x.rcx=c->Rcx;x.rdx=c->Rdx;x.r8=c->R8;x.r9=c->R9;x.rsp=c->Rsp;SafeRead(c->Rsp,x.ret);for(int i=0;i<STACK_QWORDS;++i)SafeRead(c->Rsp+8ull*(i+1),x.stack[i]); SnapshotMemory(x.rcx,x.rcxMem);SnapshotMemory(x.rdx,x.rdxMem);SnapshotMemory(x.r8,x.r8Mem);SnapshotMemory(x.r9,x.r9Mem);ResolveNaturalCaller(g_bp.target,x); }
        PatchByte(g_bp.target,g_bp.original); InterlockedExchange(&g_bp.rearmTid,static_cast<LONG>(tid)); c->Rip=g_bp.target; c->EFlags|=0x100u; return EXCEPTION_CONTINUE_EXECUTION;
    }
    if(code==EXCEPTION_SINGLE_STEP && static_cast<DWORD>(InterlockedCompareExchange(&g_bp.rearmTid,0,0))==tid){ PatchByte(g_bp.target,0xCC);InterlockedExchange(&g_bp.rearmTid,0);c->EFlags&=~0x100u;return EXCEPTION_CONTINUE_EXECUTION; }
    return EXCEPTION_CONTINUE_SEARCH;
}

static bool InstallBP(uintptr_t target){
    if(!target||!Readable(target,1)||!IsExecutable(target)) return false; uint8_t o=0;if(!SafeRead(target,o)||o==0xCC)return false;
    g_bp={};g_bp.target=target;g_bp.original=o;g_bp.veh=AddVectoredExceptionHandler(1,Veh);if(!g_bp.veh)return false;InterlockedExchange(&g_bp.installed,1);if(!PatchByte(target,0xCC)){InterlockedExchange(&g_bp.installed,0);RemoveVectoredExceptionHandler(g_bp.veh);g_bp={};return false;}return true;
}
static void ClearCaps(){InterlockedExchange(&g_bp.count,0);memset(g_bp.caps,0,sizeof(g_bp.caps));}
static void UninstallBP(){ if(InterlockedCompareExchange(&g_bp.installed,0,0)) PatchByte(g_bp.target,g_bp.original);InterlockedExchange(&g_bp.installed,0);if(g_bp.veh)RemoveVectoredExceptionHandler(g_bp.veh);g_bp={}; }
static std::vector<Capture> Snapshot(){ LONG n=InterlockedCompareExchange(&g_bp.count,0,0);if(n<0)n=0;if(n>MAX_CAP)n=MAX_CAP;return std::vector<Capture>(g_bp.caps,g_bp.caps+n); }

struct LocVal { std::string loc; uintptr_t v=0; };
static std::vector<LocVal> Locations(const Capture& c){
    std::vector<LocVal> v={{"RCX",c.rcx},{"RDX",c.rdx},{"R8",c.r8},{"R9",c.r9}};
    for(int i=0;i<STACK_QWORDS;++i){ std::ostringstream s;s<<"STACK+0x"<<std::hex<<std::uppercase<<(8*(i+1));v.push_back({s.str(),c.stack[i]}); }
    return v;
}

static const MemSnap* SnapForReg(const Capture& c,const std::string& reg){
    if(reg=="RCX")return &c.rcxMem; if(reg=="RDX")return &c.rdxMem; if(reg=="R8")return &c.r8Mem; if(reg=="R9")return &c.r9Mem; return nullptr;
}

static std::vector<std::pair<std::string,uintptr_t>> PointerVariants(const Capture& c,const LocVal& lv){
    std::vector<std::pair<std::string,uintptr_t>> out; out.push_back({lv.loc,lv.v});
    if(const MemSnap* m=SnapForReg(c,lv.loc)){
        uintptr_t q0=0,q1=0;
        if(SnapRead(*m,0,q0)&&q0)out.push_back({"*("+lv.loc+"+0x0)",q0});
        if(SnapRead(*m,8,q1)&&q1)out.push_back({"*("+lv.loc+"+0x8)",q1});
    } else {
        uintptr_t d=0;if(lv.v&&SafeRead(lv.v,d)&&d)out.push_back({"*("+lv.loc+")",d});
    }
    return out;
}

static std::set<std::string> MatchPointer(const std::vector<Capture>& caps, uintptr_t expected){
    std::set<std::string> out;if(!expected)return out;
    for(auto& c:caps) for(auto& lv:Locations(c)) for(auto& pv:PointerVariants(c,lv)) if(pv.second==expected) out.insert(pv.first);
    return out;
}

static bool LooksCreatureLike(uintptr_t obj,const Profile& p){
    if(!obj||!Readable(obj,8))return false; uintptr_t vt=0;if(!SafeRead(obj,vt)||!vt)return false;
    bool vtKnown=(p.monsterVtableRva&&vt==g_base+p.monsterVtableRva)||(p.playerVtableRva&&vt==g_base+p.playerVtableRva)||(p.npcVtableRva&&vt==g_base+p.npcVtableRva)||(p.localPlayerVtableRva&&vt==g_base+p.localPlayerVtableRva);
    uint32_t x=0,y=0;uint8_t z=0;bool pos=p.x&&p.y&&p.z&&SafeRead(obj+p.x,x)&&SafeRead(obj+p.y,y)&&SafeRead(obj+p.z,z)&&x>1000&&y>1000&&z<32;
    return vtKnown||pos;
}

static std::set<std::string> MatchItem(const std::vector<Capture>& caps,const Profile& p,uint32_t expected){
    std::set<std::string> out;
    for(auto& c:caps) for(auto& lv:Locations(c)) for(auto& pv:PointerVariants(c,lv)){ uint32_t id=0;if(ReadItemId(pv.second,p,id)&&id==expected)out.insert(pv.first); }
    return out;
}

static bool SnapHasPosition(const MemSnap& m,uint32_t x,uint32_t y,uint32_t z,int& off){
    for(int o=0;o<=0x20;o+=4){uint32_t a=0,b=0,cz=0;uint8_t z8=0;if(!SnapRead(m,o,a)||!SnapRead(m,o+4,b))continue;if(SnapRead(m,o+8,cz)&&a==x&&b==y&&cz==z){off=o;return true;}if(SnapRead(m,o+8,z8)&&a==x&&b==y&&z8==z){off=o;return true;}}return false;
}

static bool MemoryHasPosition(uintptr_t p,uint32_t x,uint32_t y,uint32_t z,int& off){
    if(!p)return false;
    for(int o=0;o<=0x20;o+=4){ uint32_t a=0,b=0,cz=0;uint8_t z8=0; if(SafeRead(p+o,a)&&SafeRead(p+o+4,b)){ if(SafeRead(p+o+8,cz)&&a==x&&b==y&&cz==z){off=o;return true;} if(SafeRead(p+o+8,z8)&&a==x&&b==y&&z8==z){off=o;return true;} } }
    return false;
}

static std::set<std::string> MatchPosition(const std::vector<Capture>& caps,uint32_t x,uint32_t y,uint32_t z){
    std::set<std::string> out;
    for(auto& c:caps)for(auto& lv:Locations(c)){
        int off=0;
        if(const MemSnap* m=SnapForReg(c,lv.loc)){
            if(SnapHasPosition(*m,x,y,z,off))out.insert(lv.loc+"->Position(+"+Hex(off)+")");
            uintptr_t child=0;if(SnapRead(*m,0,child)&&child&&MemoryHasPosition(child,x,y,z,off))out.insert("*("+lv.loc+"+0x0)->Position(+"+Hex(off)+")");
        } else if(MemoryHasPosition(lv.v,x,y,z,off)) out.insert(lv.loc+"->Position(+"+Hex(off)+")");
    }
    return out;
}

static std::set<std::string> MatchScalarPair(const std::vector<Capture>& a,uint32_t av,const std::vector<Capture>& b,uint32_t bv){
    std::set<std::string> ma,mb;
    for(auto& c:a)for(auto& lv:Locations(c))if(static_cast<uint32_t>(lv.v)==av)ma.insert(lv.loc);
    for(auto& c:b)for(auto& lv:Locations(c))if(static_cast<uint32_t>(lv.v)==bv)mb.insert(lv.loc);
    std::set<std::string> out;std::set_intersection(ma.begin(),ma.end(),mb.begin(),mb.end(),std::inserter(out,out.begin()));return out;
}

static bool StartsWithAt(uintptr_t p,const std::string& s){ if(!p||s.empty()||!Readable(p,s.size()))return false; return memcmp(reinterpret_cast<const void*>(p),s.data(),s.size())==0; }
static std::set<std::string> MatchString(const std::vector<Capture>& caps,const std::string& phrase){
    std::set<std::string> out;
    for(auto& c:caps)for(auto& lv:Locations(c)){
        const auto* snap=SnapForReg(c,lv.loc);
        // Short strings can live in the caller's temporary stack wrapper.
        // Match their bytes at entry, before ENTER allows that frame to vanish.
        if(snap&&snap->validBytes>=phrase.size()){
            if(!phrase.empty()&&memcmp(snap->bytes,phrase.data(),phrase.size())==0)out.insert(lv.loc+"->char*");
        }else if(StartsWithAt(lv.v,phrase))out.insert(lv.loc+"->char*");
        uintptr_t d=0;const bool hasPointer=snap?SnapRead(*snap,0,d):(lv.v&&SafeRead(lv.v,d));
        if(hasPointer&&StartsWithAt(d,phrase))out.insert("*("+lv.loc+")->char*");
    }
    return out;
}

static std::set<std::string> Intersect(const std::set<std::string>& a,const std::set<std::string>& b){std::set<std::string> o;std::set_intersection(a.begin(),a.end(),b.begin(),b.end(),std::inserter(o,o.begin()));return o;}
static std::string Join(const std::set<std::string>& s){std::ostringstream o;bool f=true;for(auto& x:s){if(!f)o<<", ";f=false;o<<x;}return o.str();}

struct AbiCandidate { std::string path; int score=0; std::vector<std::string> evidence; };
struct AbiResult {
    std::string key,name,status="pending"; uintptr_t rva=0; bool tested=false,machine=false,thread=false,callable=false;
    bool runtimeHit=false, callerCandidateReady=false; DWORD naturalThreadId=0;
    bool naturalCallerObserved=false, callerSiteValidated=false; int callerSamples=0;
    uintptr_t naturalCallSiteRva=0,naturalReturnRva=0,naturalCallerFunctionRva=0,naturalCallerFunctionEndRva=0;
    std::string naturalCallKind,callSiteWindow,callerProloguePrefix;
    std::vector<Capture>a,b; std::vector<std::pair<std::string,std::string>> mappings;
    std::vector<AbiCandidate> candidates; std::string notes;
};

struct LookOracle {
    bool ready=false; uintptr_t rva=0; std::string path; uint32_t idA=0,idB=0; int score=0;
};
static LookOracle g_lookOracle{};

static std::string HexBytes(uintptr_t a,size_t n){
    std::ostringstream o;for(size_t i=0;i<n;++i){uint8_t b=0;if(i)o<<" ";if(!SafeRead(a+i,b)){o<<"??";continue;}o<<std::uppercase<<std::hex<<std::setw(2)<<std::setfill('0')<<(unsigned)b;}return o.str();
}

static void FinalizeCallerEvidence(AbiResult& r){
    std::map<uintptr_t,int> total,aCount,bCount;std::map<uintptr_t,uintptr_t> callerBegin,callerEnd,retRva;std::map<uintptr_t,NaturalCallKind> kinds;
    auto add=[&](const std::vector<Capture>& v,std::map<uintptr_t,int>& side){for(const auto& c:v){if(!c.callSite||!InMainModule(c.callSite))continue;++total[c.callSite];++side[c.callSite];callerBegin[c.callSite]=c.callerFunction;callerEnd[c.callSite]=c.callerFunctionEnd;retRva[c.callSite]=InMainModule(c.ret)?c.ret-g_base:0;kinds[c.callSite]=c.callKind;}};
    add(r.a,aCount);add(r.b,bCount);
    if(total.empty()){r.naturalCallerObserved=false;r.callerSiteValidated=false;r.callerCandidateReady=false;return;}
    uintptr_t best=0;int bestScore=-1;for(auto& [site,count]:total){int score=count*10+(aCount[site]?20:0)+(bCount[site]?20:0);if(score>bestScore){bestScore=score;best=site;}}
    r.naturalCallerObserved=true;r.callerSamples=total[best];r.naturalCallSiteRva=best-g_base;r.naturalReturnRva=retRva[best];
    if(callerBegin[best])r.naturalCallerFunctionRva=callerBegin[best]-g_base;if(callerEnd[best])r.naturalCallerFunctionEndRva=callerEnd[best]-g_base;
    r.naturalCallKind=NaturalCallKindName(kinds[best]);r.callerSiteValidated=aCount[best]>0&&bCount[best]>0;
    uintptr_t windowStart=best>=g_base+12?best-12:best;r.callSiteWindow=HexBytes(windowStart,24);if(callerBegin[best])r.callerProloguePrefix=HexBytes(callerBegin[best],16);
    r.callerCandidateReady=r.machine&&r.naturalThreadId&&r.callerSiteValidated;
}

static std::string SnapJson(const MemSnap& m){
    uintptr_t q0=0,q1=0;SnapRead(m,0,q0);SnapRead(m,8,q1);std::ostringstream j;
    const auto bytes=std::min<uint32_t>(m.validBytes,ARG_SNAP_BYTES);
    j<<"{\"base\":\""<<Hex(m.base)<<"\",\"validBytes\":"<<bytes<<",\"q0\":\""<<Hex(q0)<<"\",\"q1\":\""<<Hex(q1)<<"\",\"bytesHex\":\"";
    for(uint32_t i=0;i<bytes;++i)j<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<unsigned>(m.bytes[i]);
    j<<"\"}";return j.str();
}
static std::string CaptureJson(const std::vector<Capture>& v){
    std::ostringstream j;j<<"[";size_t n=std::min<size_t>(v.size(),8);
    for(size_t i=0;i<n;++i){
        if(i)j<<",";const auto& c=v[i];
        j<<"{\"threadId\":"<<c.tid
         <<",\"RCX\":\""<<Hex(c.rcx)<<"\""
         <<",\"RDX\":\""<<Hex(c.rdx)<<"\""
         <<",\"R8\":\""<<Hex(c.r8)<<"\""
         <<",\"R9\":\""<<Hex(c.r9)<<"\""
         <<",\"returnAddress\":\""<<Hex(c.ret)<<"\""
         <<",\"returnRva\":\""<<(InMainModule(c.ret)?Hex(c.ret-g_base):"0x0")<<"\""
         <<",\"callSiteRva\":\""<<(c.callSite?Hex(c.callSite-g_base):"0x0")<<"\""
         <<",\"callerFunctionRva\":\""<<(c.callerFunction?Hex(c.callerFunction-g_base):"0x0")<<"\""
         <<",\"callerFunctionEndRva\":\""<<(c.callerFunctionEnd?Hex(c.callerFunctionEnd-g_base):"0x0")<<"\""
         <<",\"callKind\":\""<<NaturalCallKindName(c.callKind)<<"\""
         <<",\"argSnapshots\":{\"RCX\":"<<SnapJson(c.rcxMem)<<",\"RDX\":"<<SnapJson(c.rdxMem)<<",\"R8\":"<<SnapJson(c.r8Mem)<<",\"R9\":"<<SnapJson(c.r9Mem)<<"},\"stack\":[";
        for(int k=0;k<STACK_QWORDS;++k){if(k)j<<",";j<<"\""<<Hex(c.stack[k])<<"\"";}j<<"]}";
    }
    j<<"]";return j.str();
}

static std::string CandidateJson(const std::vector<AbiCandidate>& v){
    std::ostringstream j; j<<"[";
    for(size_t i=0;i<v.size();++i){ if(i)j<<","; j<<"{\"path\":\""<<EscapeJson(v[i].path)<<"\",\"score\":"<<v[i].score<<",\"evidence\":["; for(size_t k=0;k<v[i].evidence.size();++k){if(k)j<<",";j<<"\""<<EscapeJson(v[i].evidence[k])<<"\"";} j<<"]}"; }
    j<<"]"; return j.str();
}

static std::string CandidateReviewJson(const std::vector<AbiCandidate>& candidates){
    std::vector<AbiCandidate> ranked=candidates;
    std::sort(ranked.begin(),ranked.end(),[](const auto& a,const auto& b){return a.score==b.score?a.path<b.path:a.score>b.score;});
    constexpr int closeGap=15;
    const auto gap=ranked.size()>1?static_cast<int64_t>(ranked[0].score)-ranked[1].score:0;
    const bool close=ranked.size()>1&&gap<closeGap;
    std::ostringstream j;j<<"{\"policy\":\"score_is_ranking_only_not_abi_proof\",\"candidateCount\":"<<ranked.size()
        <<",\"closeScoreThreshold\":"<<closeGap<<",\"bestRunnerUpGap\":"<<gap
        <<",\"needsDisambiguation\":"<<(close?"true":"false")<<",\"status\":\""
        <<(ranked.empty()?"no_ranked_candidates":(close?"close_candidates_need_new_stimulus":"ranked_candidates_need_contract_validation"))<<"\",\"ranked\":[";
    for(size_t i=0;i<ranked.size();++i){if(i)j<<",";const auto delta=static_cast<int64_t>(ranked[0].score)-ranked[i].score;
        j<<"{\"path\":\""<<EscapeJson(ranked[i].path)<<"\",\"score\":"<<ranked[i].score<<",\"rank\":"<<i+1
            <<",\"deltaFromBest\":"<<delta<<",\"closeToBest\":"<<(delta<closeGap?"true":"false")<<"}";}
    j<<"],\"nextStimulus\":\""<<(close?"Change one semantic argument at a time and collect a third natural sample; keep all alternative paths.":"Validate the typed argument contract, return/ownership and active call; ranking alone cannot enable the function.")<<"\"}";
    return j.str();
}

static std::string ValidationProgressJson(const AbiResult& r){
    bool ownedReturn=false;for(const auto& method:pipe_discovery::methods)if(r.key==method.key)ownedReturn=method.ownedReturn;
    std::ostringstream j;
    j<<"{\"schema\":\"crome-abi-validation-progress-v1\",\"argumentEvidenceMeasured\":"<<(r.machine?"true":"false")
        <<",\"naturalThreadEvidenceMeasured\":"<<(r.naturalThreadId&&!r.a.empty()&&!r.b.empty()?"true":"false")
        <<",\"callerEvidenceMeasured\":"<<(r.callerSiteValidated?"true":"false")
        <<",\"typedArgumentsComplete\":false,\"returnContractValidated\":false,\"ownershipValidated\":false,\"activeCallValidated\":false"
        <<",\"requiresOwnedReturn\":"<<(ownedReturn?"true":"false")
        <<",\"contractComplete\":false,\"remainingChecks\":[\"typed_argument_contract\",\"return_or_void_contract\"";
    if(ownedReturn)j<<",\"returned_object_membership\",\"reference_increment_and_release\"";
    if(!r.naturalThreadId||r.a.empty()||r.b.empty())j<<",\"natural_thread_evidence\"";
    if(!r.callerSiteValidated)j<<",\"natural_caller_evidence\"";
    j<<",\"game_thread_dispatch\",\"active_call_result\"],\"savedPointersAreHistorical\":true,\"refreshObjectsBeforeActiveTest\":true}";
    return j.str();
}

static std::string FrozenCandidateReplayJson(const AbiResult& r);
static std::string ResultJson(const AbiResult& r){
    std::ostringstream j;
    j<<"{\n"
     <<"        \"name\": \""<<EscapeJson(r.name)<<"\",\n"
     <<"        \"rva\": \""<<Hex(r.rva)<<"\",\n"
     <<"        \"status\": \""<<EscapeJson(r.status)<<"\",\n"
     <<"        \"captureMechanism\": \"temporary_int3_veh_passive\",\n"
     <<"        \"samplesA\": "<<r.a.size()<<",\n"
     <<"        \"samplesB\": "<<r.b.size()<<",\n"
     <<"        \"runtimeRvaObserved\": "<<(r.runtimeHit?"true":"false")<<",\n"
     <<"        \"confirmedMappings\": [";
    for(size_t i=0;i<r.mappings.size();++i){if(i)j<<",";j<<"{\"semantic\":\""<<EscapeJson(r.mappings[i].first)<<"\",\"path\":\""<<EscapeJson(r.mappings[i].second)<<"\"}";}
    j<<"],\n"
     <<"        \"evidenceCandidates\": "<<CandidateJson(r.candidates)<<",\n"
     <<"        \"contractComplete\": false,\n"
     <<"        \"activeCallValidated\": false,\n"
     <<"        \"candidateReview\": "<<CandidateReviewJson(r.candidates)<<",\n"
     <<"        \"frozenCandidateReplay\": "<<FrozenCandidateReplayJson(r)<<",\n"
     <<"        \"validationProgress\": "<<ValidationProgressJson(r)<<",\n"
     <<"        \"captureScope\": {\"processId\":"<<GetCurrentProcessId()<<",\"moduleBase\":\""<<Hex(g_base)<<"\",\"purpose\":\"historical_evidence_not_live_call_arguments\"},\n"
     <<"        \"captureStorage\": {\"storedA\":"<<std::min<size_t>(r.a.size(),8)<<",\"storedB\":"<<std::min<size_t>(r.b.size(),8)<<",\"limitPerStimulus\":8,\"truncated\":"<<(r.a.size()>8||r.b.size()>8?"true":"false")<<"},\n"
     <<"        \"machineAbiValidated\": "<<(r.machine?"true":"false")<<",\n"
     <<"        \"naturalThreadObserved\": "<<(r.naturalThreadId?"true":"false")<<",\n"
     <<"        \"naturalThreadId\": "<<r.naturalThreadId<<",\n"
     <<"        \"threadValidated\": "<<(r.thread?"true":"false")<<",\n"
     <<"        \"naturalCallerObserved\": "<<(r.naturalCallerObserved?"true":"false")<<",\n"
     <<"        \"callerSiteValidated\": "<<(r.callerSiteValidated?"true":"false")<<",\n"
     <<"        \"callerSamples\": "<<r.callerSamples<<",\n"
     <<"        \"naturalCallKind\": \""<<EscapeJson(r.naturalCallKind)<<"\",\n"
     <<"        \"naturalCallSiteRva\": \""<<Hex(r.naturalCallSiteRva)<<"\",\n"
     <<"        \"naturalReturnRva\": \""<<Hex(r.naturalReturnRva)<<"\",\n"
     <<"        \"naturalCallerFunctionRva\": \""<<Hex(r.naturalCallerFunctionRva)<<"\",\n"
     <<"        \"naturalCallerFunctionEndRva\": \""<<Hex(r.naturalCallerFunctionEndRva)<<"\",\n"
     <<"        \"callSiteWindow\": \""<<EscapeJson(r.callSiteWindow)<<"\",\n"
     <<"        \"callerProloguePrefix\": \""<<EscapeJson(r.callerProloguePrefix)<<"\",\n"
     <<"        \"callerCandidateReady\": "<<(r.callerCandidateReady?"true":"false")<<",\n"
     <<"        \"callable\": "<<"false"<<",\n"
     <<"        \"notes\": \""<<EscapeJson(r.notes)<<"\",\n"
     <<"        \"capturesA\": "<<CaptureJson(r.a)<<",\n"
     <<"        \"capturesB\": "<<CaptureJson(r.b)<<"\n"
     <<"      }";
    return j.str();
}

static uintptr_t RawLocation(const Capture& c,const std::string& loc){
    if(loc=="RCX")return c.rcx; if(loc=="RDX")return c.rdx; if(loc=="R8")return c.r8; if(loc=="R9")return c.r9;
    if(loc.rfind("STACK+0x",0)==0){ try{ unsigned off=std::stoul(loc.substr(8),nullptr,16); if(off>=8 && off<=STACK_QWORDS*8 && off%8==0) return c.stack[off/8-1]; }catch(...){} }
    return 0;
}

static bool FrozenPointerPath(const Capture& c,const std::string& path,uintptr_t& out){
    out=0;
    if(path.rfind("*(",0)==0&&path.size()>3&&path.back()==')'){
        auto reg=path.substr(2,path.size()-3);uint32_t offset=0;
        const auto plus=reg.find("+0x");
        if(plus!=std::string::npos){
            try{size_t used=0;const auto value=std::stoull(reg.substr(plus+1),&used,0);
                if(used!=reg.size()-plus-1||value>ARG_SNAP_BYTES)return false;offset=static_cast<uint32_t>(value);
            }catch(...){return false;}
            reg.resize(plus);
        }
        const auto* snap=SnapForReg(c,reg);
        return snap&&SnapRead(*snap,offset,out);
    }
    if(path.rfind("STACK+0x",0)==0){
        try{size_t used=0;const auto offset=std::stoull(path.substr(6),&used,0);
            if(used!=path.size()-6||offset<8||offset>STACK_QWORDS*8||offset%8)return false;
        }catch(...){return false;}
        out=RawLocation(c,path);return true;
    }
    if(path=="RCX"||path=="RDX"||path=="R8"||path=="R9"){out=RawLocation(c,path);return true;}
    return false;
}

static std::string FrozenCandidateReplayJson(const AbiResult& r){
    std::ostringstream j;j<<"{\"policy\":\"compare_recorded_values_without_dereferencing_old_addresses\",\"activeValidation\":false,\"paths\":[";
    for(size_t i=0;i<r.candidates.size();++i){
        if(i)j<<",";const auto& path=r.candidates[i].path;size_t recorded=0,missing=0;
        j<<"{\"path\":\""<<EscapeJson(path)<<"\",\"sampleValues\":[";bool first=true;
        for(const auto* captures:{&r.a,&r.b})for(const auto& c:*captures){
            if(!first)j<<",";first=false;uintptr_t value=0;
            if(FrozenPointerPath(c,path,value)){++recorded;j<<"\""<<Hex(value)<<"\"";}else{++missing;j<<"null";}
        }
        j<<"],\"recordedValues\":"<<recorded<<",\"missingValues\":"<<missing<<"}";
    }
    j<<"]}";return j.str();
}

static void ShowSavedCandidateReview(const Profile& p){
    const auto previousFp=ExtractNamedObject(ExtractNamedObject(p.raw,"abiCatalog"),"fingerprint");
    if(!DiscoveryFingerprintMatches(p)||!pipe_discovery::exactFingerprint(ParseNum(FlatJsonString(previousFp,"timestamp")),ParseNum(FlatJsonString(previousFp,"sizeOfImage")),ParseNum(FlatJsonString(previousFp,"textHashFNV1A64")),p.timestamp,p.sizeOfImage,p.textHash))return;
    try{
        const auto root=crome::json::parse(p.raw);const auto* abi=root.get("abiCatalog");
        const auto* observations=abi?abi->get("lastEvidenceSession"):nullptr;
        if(!observations||!observations->isObject())return;
        for(const auto& [key,obj]:observations->object()){
            const auto* rva=obj.get("rva");const auto current=DiscoveredRva(p,key);
            if(!current||!rva||!rva->isString()||ParseNum(rva->string())!=current)continue;
            const auto* candidates=obj.get("evidenceCandidates");if(!candidates||!candidates->isArray()||candidates->array().empty())continue;
            std::vector<AbiCandidate> alternatives;
            for(const auto& candidate:candidates->array()){
                const auto* path=candidate.get("path");const auto* score=candidate.get("score");
                if(!path||!path->isString()||!score||!score->isNumber()||score->number()<-1000000||score->number()>1000000)continue;
                alternatives.push_back({path->string(),static_cast<int>(score->number()),{}});
            }
            const auto review=crome::json::parse(CandidateReviewJson(alternatives));
            std::cout<<"[RESUME ABI] "<<key<<": alternativas salvas="<<alternatives.size()
                <<" proximas="<<(review.get("needsDisambiguation")->boolean()?"SIM; variar um argumento e coletar nova amostra":"NAO; concluir contrato e chamada ativa")<<"\n";
        }
    }catch(const std::exception& e){std::cout<<"[RESUME ABI] Revisao diagnostica indisponivel: "<<e.what()<<"\n";}
}

static bool EvalPointerPath(const Capture& c,const std::string& path,uintptr_t& out){
    out=0;
    if(path.size()>3 && path.rfind("*(",0)==0 && path.back()==')'){
        std::string inner=path.substr(2,path.size()-3); uint32_t off=0; std::string reg=inner;
        size_t plus=inner.find("+0x"); if(plus!=std::string::npos){reg=inner.substr(0,plus);try{off=(uint32_t)std::stoul(inner.substr(plus+1),nullptr,16);}catch(...){return false;}}
        if(const MemSnap* m=SnapForReg(c,reg)){uintptr_t v=0;if(SnapRead(*m,off,v)&&v){out=v;return true;}}
        uintptr_t base=RawLocation(c,reg);return base&&SafeRead(base+off,out)&&out;
    }
    out=RawLocation(c,path); return out!=0;
}

static int PathPreference(const std::string& p,const std::string& semantic){
    int s=0; bool stack=p.rfind("STACK+",0)==0 || p.rfind("*(STACK+",0)==0; if(stack)s-=25; else s+=15;
    if(p=="RCX") s += semantic=="this"?40:-20;
    if(p=="RDX" || p=="*(RDX)") s+=20;
    if(p.rfind("*(",0)==0) s+=5;
    return s;
}

static DWORD ConsistentThread(const std::vector<Capture>& a,const std::vector<Capture>& b){
    DWORD tid=0; for(auto* v:{&a,&b}) for(auto& c:*v){ if(!c.tid)continue; if(!tid)tid=c.tid; else if(tid!=c.tid)return 0; } return tid;
}

static std::set<std::string> MatchKnownGame(const std::vector<Capture>& caps,const Profile& p){ return MatchPointer(caps,KnownGameAddress(p)); }

static std::vector<AbiCandidate> RankPointerPaths(const std::set<std::string>& paths,const std::string& semantic,bool twoSamples=true){
    std::vector<AbiCandidate> out; for(auto& p:paths){ AbiCandidate c; c.path=p; c.score=50+PathPreference(p,semantic); c.evidence.push_back(twoSamples?"same path matched two runtime samples":"runtime pointer match"); if(!p.empty()&&p[0]!='S'&&p.rfind("*(STACK+",0)!=0)c.evidence.push_back("register-backed path"); out.push_back(c); }
    std::sort(out.begin(),out.end(),[](auto&a,auto&b){return a.score>b.score;}); return out;
}

static bool CaptureTwo(uintptr_t rva,const std::string& promptA,const std::string& promptB,std::vector<Capture>&a,std::vector<Capture>&b,int retries=1){
    uintptr_t addr=g_base+rva;if(!InstallBP(addr)){std::cout<<"[ABI][FAIL] Nao foi possivel instalar breakpoint em "<<Hex(rva)<<".\n";return false;}
    std::cout<<"[ABI] Breakpoint temporario instalado @ "<<Hex(rva)<<". Nenhuma funcao interna sera chamada pelo Builder.\n";
    auto one=[&](const std::string& prompt,std::vector<Capture>& dst,const char* label){
        for(int i=1;i<=retries;++i){ ClearCaps();Wait(prompt);Sleep(100);dst=Snapshot();std::cout<<"[ABI] Capturas "<<label<<": "<<dst.size()<<"\n";if(!dst.empty())return true;if(i<retries)std::cout<<"[RETRY] A RVA nao disparou. Repita a acao real no cliente.\n";}return false; };
    bool okA=one(promptA,a,"A"), okB=one(promptB,b,"B"); UninstallBP(); return okA||okB;
}

static std::map<std::string,uint32_t> ItemPaths(const std::vector<Capture>& caps,const Profile& p){
    std::map<std::string,uint32_t> out;
    for(auto& c:caps) for(auto& lv:Locations(c)) for(auto& pv:PointerVariants(c,lv)){ uint32_t id=0; if(ReadItemId(pv.second,p,id))out[pv.first]=id; }
    return out;
}

struct ActionEntry { std::string key,name;uintptr_t rva=0; };
using LookEntry=ActionEntry;
static std::vector<ActionEntry> ActionEntries(const Profile& p,const std::string& family){
    std::vector<ActionEntry> entries;
    for(const auto& group:pipe_discovery::actionFamilies){
        if(family!=group.key)continue;
        for(const auto* key:group.functionKeys){
            const auto rva=DiscoveredRva(p,key);if(!rva)continue;
            for(const auto& method:pipe_discovery::methods)
                if(std::string(key)==method.key){entries.push_back({key,method.semantic,rva});break;}
        }
    }
    return entries;
}
static std::vector<LookEntry> LookEntries(const Profile& p){return ActionEntries(p,"look");}
using ActionCaptureFn=bool(*)(uintptr_t,const std::string&,const std::string&,std::vector<Capture>&,std::vector<Capture>&,int);
using LookCaptureFn=ActionCaptureFn;
static bool CaptureActionSamples(const Profile& p,const std::string& family,const std::string& promptA,const std::string& promptB,ActionEntry& selected,std::vector<Capture>& a,std::vector<Capture>& b,ActionCaptureFn capture=CaptureTwo){
    for(const auto& entry:ActionEntries(p,family)){
        selected=entry;a.clear();b.clear();
        std::cout<<"[ACTION DISCOVERY] "<<family<<": candidato da build atual "<<entry.name<<" @ "<<Hex(entry.rva)<<"\n";
        if(capture(entry.rva,promptA,promptB,a,b,2)&&!a.empty()&&!b.empty())return true;
        if(!a.empty()||!b.empty()){
            std::cout<<"[ACTION DISCOVERY][PENDING] Captura parcial; repita esta funcao. Amostras de funcoes diferentes nao serao combinadas.\n";
            return false;
        }
        std::cout<<"[ACTION DISCOVERY] Sem chamadas observadas nesta funcao. Proximo candidato sera testado separadamente, se existir.\n";
    }
    return false;
}
static bool CaptureLookSamples(const Profile& p,LookEntry& selected,std::vector<Capture>& a,std::vector<Capture>& b,LookCaptureFn capture=CaptureTwo){
    return CaptureActionSamples(p,"look","De LOOK em um ITEM A qualquer e pressione ENTER...\n","De LOOK em um ITEM B DIFERENTE e pressione ENTER...\n",selected,a,b,capture);
}
static bool CaptureLookRaw(const Profile& p,const std::string& prompt,std::vector<Capture>& caps){
    const uintptr_t rva=g_lookOracle.ready?g_lookOracle.rva:0;
    const auto entries=LookEntries(p);
    if(!rva||std::none_of(entries.begin(),entries.end(),[&](const LookEntry& e){return e.rva==rva;}))return false;
    uintptr_t addr=g_base+rva; if(!InstallBP(addr))return false; ClearCaps(); Wait(prompt); Sleep(100); caps=Snapshot(); UninstallBP(); return !caps.empty();
}

static bool EstablishLookOracle(const Profile& p,AbiResult* result=nullptr,LookCaptureFn capture=CaptureTwo){
    if(g_lookOracle.ready)return true;
    if(LookEntries(p).empty()||!p.itemIdOffset||!p.itemVtableRva){std::cout<<"[LOOK ASSIST] Profile sem Look descoberto/Item layout suficiente.\n";return false;}
    std::cout<<"\n============================================================\nLOOK ASSISTANT - AUTO ID / THING ORACLE\n============================================================\n";
    std::cout<<"Voce NAO precisa saber nenhum ID. Use o LOOK normal do cliente em dois ITENS diferentes.\n";
    std::vector<Capture>a,b;LookEntry selected;
    const bool captured=CaptureLookSamples(p,selected,a,b,capture);
    const uintptr_t rva=selected.rva;
    if(result){result->key=selected.key;result->name=selected.name;result->rva=rva;result->a=a;result->b=b;result->runtimeHit=!a.empty()||!b.empty();}
    if(!captured){if(result){result->status=result->runtimeHit?"captured_runtime_partial":"rva_not_observed_runtime";result->notes="Look candidates captured separately; no complete pair of natural calls observed.";}return false;}
    auto pa=ItemPaths(a,p),pb=ItemPaths(b,p); std::vector<AbiCandidate> cand;
    for(auto& [path,idA]:pa){ auto it=pb.find(path); if(it==pb.end()||!idA||!it->second||idA==it->second)continue; AbiCandidate c; c.path=path; c.score=70+PathPreference(path,"thing"); c.evidence={"valid Item vtable in sample A","valid Item vtable in sample B","item ID changed across samples"}; if(path=="*(RDX+0x0)"||path=="*(RDX)")c.score+=80; else if(path=="RDX")c.score+=20; if(path.rfind("STACK+",0)==0||path.rfind("*(STACK+",0)==0)c.score-=40; cand.push_back(c); }
    std::sort(cand.begin(),cand.end(),[](auto&a,auto&b){return a.score>b.score;});
    if(cand.empty()){std::cout<<"[LOOK ASSIST][PENDING] Nao houve caminho Item unico/dinamico.\n";if(result){result->a=a;result->b=b;result->runtimeHit=!a.empty()||!b.empty();result->status="captured_runtime_ambiguous";result->notes="No dynamic item path survived two arbitrary Look samples.";}return false;}
    auto best=cand.front(); uint32_t ida=pa[best.path],idb=pb[best.path];
    bool strong=cand.size()==1 || best.score>=cand[1].score+15;
    std::cout<<"[LOOK ASSIST] Melhor caminho: "<<best.path<<" score="<<best.score<<" IDs observados="<<ida<<" / "<<idb<<"\n";
    if(!strong){std::cout<<"[LOOK ASSIST][PENDING] Caminho de Item ambiguo; assistente nao sera utilizado.\n";}
    if(result){result->a=a;result->b=b;result->runtimeHit=true;result->mappings.push_back({"Thing*/Item* argument",best.path});result->candidates=cand;result->machine=strong;result->status=strong?"confirmed_runtime_primary_argument":"strong_candidate_runtime_primary_argument";result->notes="Look oracle learned without user-supplied IDs; two different runtime Item IDs observed.";result->naturalThreadId=ConsistentThread(a,b);result->callerCandidateReady=strong;}
    if(!strong)return false;
    g_lookOracle={true,rva,best.path,ida,idb,best.score};
    return true;
}

struct ThingObs { uintptr_t ptr=0; uint32_t id=0,count=0; std::string path; };
static bool CaptureLookThing(const Profile& p,const std::string& prompt,ThingObs& o){
    if(!EstablishLookOracle(p,nullptr))return false; std::vector<Capture> caps; if(!CaptureLookRaw(p,prompt,caps))return false;
    for(auto& c:caps){ uintptr_t v=0;if(!EvalPointerPath(c,g_lookOracle.path,v))continue;uint32_t id=0;if(ReadItemId(v,p,id)){o.ptr=v;o.id=id;o.path=g_lookOracle.path;ReadItemCount(v,p,o.count);if(!o.count)o.count=1;std::cout<<"[LOOK ASSIST] Thing="<<Hex(o.ptr)<<" ID="<<o.id<<" count="<<o.count<<" via "<<o.path<<"\n";return true;} }
    std::cout<<"[LOOK ASSIST][PENDING] O caminho aprendido nao produziu Item valido nesta captura.\n";return false;
}

static bool CaptureTargetSample(const Profile& p,bool attack,uintptr_t distinctFrom,const std::string& prompt,std::vector<Capture>& caps,uintptr_t& target){
    uintptr_t off=attack?p.gameAttackTarget:p.gameFollowTarget; target=0;
    for(int attempt=1;attempt<=3;++attempt){ ClearCaps();Wait(prompt);Sleep(100);caps=Snapshot();target=ReadGameTarget(p,off);
        std::cout<<"[ABI] target="<<Hex(target)<<" captures="<<caps.size()<<" attempt="<<attempt<<"/3\n";
        if(!target){std::cout<<"[RETRY] Target esta NULL. Mantenha a acao ativa antes de pressionar ENTER.\n";continue;}
        if(distinctFrom&&target==distinctFrom){std::cout<<"[RETRY] A segunda amostra precisa ser outra criatura.\n";continue;}
        if(caps.empty()){std::cout<<"[RETRY] Target valido, mas a RVA nao disparou. Repita a acao.\n";continue;}
        return true;
    } return false;
}

static AbiResult TestAttackFollow(const Profile& p,bool attack){
    AbiResult r;r.key=attack?"gameAttack":"gameFollow";r.name=attack?"Game::attack":"Game::follow";r.rva=attack?p.fnGameAttack:p.fnGameFollow;r.tested=true;
    if(!r.rva){r.status="missing_rva";r.notes="BuildExplorer ainda nao forneceu RVA.";return r;}
    std::cout<<"\n============================================================\nABI TEST - "<<r.name<<" / EVIDENCE FUSION\n============================================================\n";
    uintptr_t addr=g_base+r.rva;if(!InstallBP(addr)){r.status="hook_failed";r.notes="Nao foi possivel instalar breakpoint temporario.";return r;}
    std::cout<<"[ABI] Breakpoint temporario instalado @ "<<Hex(r.rva)<<".\n";
    Wait("Deixe SEM target/follow e pressione ENTER...\n");
    uintptr_t ta=0,tb=0; std::string pa=attack?"Ataque a CRIATURA A e MANTENHA o target; depois ENTER...\n":"Use FOLLOW na CRIATURA A e MANTENHA o follow; depois ENTER...\n";
    std::string pb=attack?"Cancele e ataque uma CRIATURA B DIFERENTE; mantenha o target e ENTER...\n":"Cancele e use FOLLOW numa CRIATURA B DIFERENTE; mantenha o follow e ENTER...\n";
    bool oka=CaptureTargetSample(p,attack,0,pa,r.a,ta),okb=CaptureTargetSample(p,attack,ta,pb,r.b,tb);UninstallBP();
    r.runtimeHit=!r.a.empty()||!r.b.empty(); if(!oka||!okb){r.status=r.runtimeHit?"captured_runtime_partial":"rva_not_observed_runtime";r.notes="Nao foi possivel obter duas amostras validas/distintas. Evidencia preservada; repita apenas esta funcao depois.";return r;}
    auto both=Intersect(MatchPointer(r.a,ta),MatchPointer(r.b,tb)); r.candidates=RankPointerPaths(both,"creature");
    auto ga=Intersect(MatchKnownGame(r.a,p),MatchKnownGame(r.b,p)); bool thisOk=ga.count("RCX")!=0; if(thisOk)r.mappings.push_back({"Game* this","RCX"});
    bool wrapper=false; std::string controlPath,rawPath,wrapperReg;
    for(auto& cand:r.candidates){
        if(cand.path=="*(RDX+0x8)"){wrapper=true;controlPath=cand.path;rawPath="*(RDX+0x0)";wrapperReg="RDX";break;}
        if(cand.path=="*(R8+0x8)"){wrapper=true;controlPath=cand.path;rawPath="*(R8+0x0)";wrapperReg="R8";break;}
    }
    if(wrapper){
        bool rawOk=true; for(auto* vv:{&r.a,&r.b})for(auto& c:*vv){uintptr_t raw=0;if(!EvalPointerPath(c,rawPath,raw)||!LooksCreatureLike(raw,p)){rawOk=false;break;}}
        r.mappings.push_back({"CreaturePtr argument wrapper",wrapperReg});
        r.mappings.push_back({"Creature* pointee",rawPath});
        r.mappings.push_back({"shared_ptr control evidence",controlPath});
        r.naturalThreadId=ConsistentThread(r.a,r.b);r.machine=thisOk&&rawOk;r.callerCandidateReady=r.machine&&r.naturalThreadId;
        r.status=r.machine?"confirmed_runtime_shared_ptr_abi":"strong_candidate_runtime_shared_ptr_abi";
        r.notes="Target field matched the shared_ptr control pointer (+0x8) while the first qword of the same captured wrapper resolved to a Creature-like object. This avoids misclassifying a caller stack alias as the ABI argument.";
    } else {
        if(!r.candidates.empty())r.mappings.push_back({"Creature target evidence",r.candidates.front().path});
        r.naturalThreadId=ConsistentThread(r.a,r.b);bool strong=!r.candidates.empty()&&(r.candidates.size()==1||r.candidates[0].score>=r.candidates[1].score+15);r.machine=strong&&thisOk;r.callerCandidateReady=r.machine&&r.naturalThreadId;r.status=r.machine?"confirmed_runtime_machine_abi":(!r.candidates.empty()?"strong_candidate_runtime_abi":"captured_runtime_ambiguous");
        r.notes="Two distinct live target samples used. If the target field is a shared_ptr control pointer, V1.3 recognizes +0x8 wrapper evidence before considering stack aliases.";
    }
    if(r.naturalThreadId)std::cout<<"[THREAD] Natural calls observed on TID "<<r.naturalThreadId<<".\n";
    return r;
}

static AbiResult TestLook(const Profile& p){
    AbiResult r;r.key="gameLook";r.name="Game::look";r.tested=true;
    const auto entries=LookEntries(p);if(entries.empty()){r.status="missing_rva";return r;}
    r.key=entries.front().key;r.name=entries.front().name;r.rva=entries.front().rva;
    EstablishLookOracle(p,&r); return r;
}

static AbiResult TestMove(const Profile& p){
    AbiResult r;r.key="gameMove";r.name="Game::move";r.rva=p.fnGameMove;r.tested=true;if(!r.rva){r.status="missing_rva";return r;}
    std::cout<<"\n============================================================\nABI TEST - Game::move / LOOK ASSISTED\n============================================================\n";
    ThingObs A,B; if(!CaptureLookThing(p,"De LOOK no ITEM A que voce vai mover; depois ENTER...\n",A)){r.status="look_assist_failed";return r;}
    uint32_t ax=0,ay=0,az=0;ReadPosition(p,ax,ay,az);
    uintptr_t addr=g_base+r.rva;if(!InstallBP(addr)){r.status="hook_failed";return r;}ClearCaps();Wait("Mova o ITEM A para o CHAO exatamente sob seu personagem; depois ENTER...\n");Sleep(100);r.a=Snapshot();UninstallBP();
    std::cout<<"[ABI] Capturas A: "<<r.a.size()<<" destino="<<ax<<","<<ay<<","<<az<<"\n";
    Wait("Agora ande para OUTRO SQM no mesmo andar. Pressione ENTER quando estiver parado...\n");
    if(!CaptureLookThing(p,"De LOOK no ITEM B que voce vai mover (pode ser outro item); depois ENTER...\n",B)){r.status="look_assist_failed";return r;}
    uint32_t bx=0,by=0,bz=0;ReadPosition(p,bx,by,bz);
    if(!InstallBP(addr)){r.status="hook_failed";return r;}ClearCaps();Wait("Mova o ITEM B para o CHAO exatamente sob seu personagem; depois ENTER...\n");Sleep(100);r.b=Snapshot();UninstallBP();
    std::cout<<"[ABI] Capturas B: "<<r.b.size()<<" destino="<<bx<<","<<by<<","<<bz<<"\n";r.runtimeHit=!r.a.empty()||!r.b.empty();
    if(r.a.empty()||r.b.empty()){r.status="rva_not_observed_runtime";r.notes="A acao manual nao passou pela RVA em uma ou ambas amostras. Nao inventei ABI.";return r;}
    auto mi=Intersect(MatchPointer(r.a,A.ptr),MatchPointer(r.b,B.ptr));auto mp=Intersect(MatchPosition(r.a,ax,ay,az),MatchPosition(r.b,bx,by,bz));auto mc=MatchScalarPair(r.a,A.count,r.b,B.count);auto ga=Intersect(MatchKnownGame(r.a,p),MatchKnownGame(r.b,p));
    std::string itemPath=mi.count("*(RDX+0x0)")?"*(RDX+0x0)":(mi.size()==1?*mi.begin():""); std::string posPath=mp.count("R8->Position(+0x0)")?"R8->Position(+0x0)":(mp.size()==1?*mp.begin():""); std::string countPath=mc.count("R9")?"R9":(mc.size()==1?*mc.begin():"");
    if(!itemPath.empty())r.mappings.push_back({"ThingPtr source pointee",itemPath}); if(!posPath.empty())r.mappings.push_back({"destination Position*",posPath}); if(!countPath.empty())r.mappings.push_back({"count scalar",countPath}); if(ga.count("RCX"))r.mappings.push_back({"Game* this","RCX"});
    r.naturalThreadId=ConsistentThread(r.a,r.b);r.machine=!itemPath.empty()&&!posPath.empty()&&ga.count("RCX");r.callerCandidateReady=r.machine&&r.naturalThreadId;r.status=r.machine?"confirmed_runtime_arguments":"captured_runtime_partial";
    r.notes="V1.3 snapshots argument pointees at breakpoint time, so temporary ThingPtr/Position references remain analyzable after the call. Source model prefers RDX=ThingPtr&, R8=Position&, R9=count when runtime evidence agrees.";return r;
}

static AbiResult TestUse(const Profile& p){
    AbiResult r;r.key="gameUse";r.name="Game::use";r.rva=p.fnGameUse;r.tested=true;if(!r.rva){r.status="missing_rva";return r;}
    std::cout<<"\n============================================================\nABI TEST - Game::use / LOOK ASSISTED\n============================================================\n";
    ThingObs A,B;if(!CaptureLookThing(p,"De LOOK no ITEM A que voce vai USAR; depois ENTER...\n",A)){r.status="look_assist_failed";return r;}
    uintptr_t addr=g_base+r.rva;if(!InstallBP(addr)){r.status="hook_failed";return r;}ClearCaps();Wait("Use o MESMO ITEM A uma vez e pressione ENTER...\n");Sleep(100);r.a=Snapshot();UninstallBP();
    if(!CaptureLookThing(p,"De LOOK no ITEM B que voce vai USAR (se possivel diferente); depois ENTER...\n",B)){r.status="look_assist_failed";return r;}
    if(!InstallBP(addr)){r.status="hook_failed";return r;}ClearCaps();Wait("Use o MESMO ITEM B uma vez e pressione ENTER...\n");Sleep(100);r.b=Snapshot();UninstallBP();r.runtimeHit=!r.a.empty()||!r.b.empty();
    if(r.a.empty()||r.b.empty()){r.status="rva_not_observed_runtime";r.notes="Use manual nao passou pela RVA em uma ou ambas amostras.";return r;}
    auto mi=Intersect(MatchPointer(r.a,A.ptr),MatchPointer(r.b,B.ptr));auto ga=Intersect(MatchKnownGame(r.a,p),MatchKnownGame(r.b,p));std::string itemPath=mi.count("*(RDX+0x0)")?"*(RDX+0x0)":(mi.size()==1?*mi.begin():"");if(!itemPath.empty())r.mappings.push_back({"ThingPtr source pointee",itemPath});if(ga.count("RCX"))r.mappings.push_back({"Game* this","RCX"});r.naturalThreadId=ConsistentThread(r.a,r.b);r.machine=!itemPath.empty()&&ga.count("RCX");r.callerCandidateReady=r.machine&&r.naturalThreadId;r.status=r.machine?"confirmed_runtime_primary_argument":"captured_runtime_ambiguous";r.notes="Source item comes from Look oracle. Temporary ThingPtr wrapper memory is snapshotted at the breakpoint; RDX wrapper evidence is preferred when it matches the reference prototype.";return r;
}

static AbiResult TestUseWith(const Profile& p){
    AbiResult r;r.key="gameUseWith";r.name="Game::useWith";r.rva=p.fnGameUseWith;r.tested=true;if(!r.rva){r.status="missing_rva";return r;}
    std::cout<<"\n============================================================\nABI TEST - Game::useWith / LOOK ASSISTED\n============================================================\n";
    std::cout<<"Escolha qualquer ferramenta/item que realmente tenha USE WITH. Voce nao precisa saber o ID.\n";
    ThingObs sa,ta,sb,tb;if(!CaptureLookThing(p,"De LOOK na FERRAMENTA/ITEM fonte do teste A; ENTER...\n",sa)){r.status="look_assist_failed";return r;}if(!CaptureLookThing(p,"De LOOK no ALVO exato do USE WITH A; ENTER...\n",ta)){r.status="look_assist_failed";return r;}
    uintptr_t addr=g_base+r.rva;if(!InstallBP(addr)){r.status="hook_failed";return r;}ClearCaps();Wait("Agora execute o USE WITH A normalmente no cliente; depois ENTER...\n");Sleep(100);r.a=Snapshot();UninstallBP();
    if(!CaptureLookThing(p,"De LOOK na FERRAMENTA/ITEM fonte do teste B (pode ser a mesma); ENTER...\n",sb)){r.status="look_assist_failed";return r;}if(!CaptureLookThing(p,"De LOOK em um ALVO B diferente, se possivel; ENTER...\n",tb)){r.status="look_assist_failed";return r;}
    if(!InstallBP(addr)){r.status="hook_failed";return r;}ClearCaps();Wait("Execute o USE WITH B normalmente no cliente; depois ENTER...\n");Sleep(100);r.b=Snapshot();UninstallBP();r.runtimeHit=!r.a.empty()||!r.b.empty();
    if(r.a.empty()&&r.b.empty()){r.status="rva_not_observed_runtime";r.notes="Game::useWith candidate was not hit by the real UI action. Keep RVA evidence but do not classify ABI; a later trace stage should inspect caller/callee siblings.";return r;}
    auto src=Intersect(MatchPointer(r.a,sa.ptr),MatchPointer(r.b,sb.ptr));auto trg=Intersect(MatchPointer(r.a,ta.ptr),MatchPointer(r.b,tb.ptr));auto ga=Intersect(MatchKnownGame(r.a,p),MatchKnownGame(r.b,p));std::string srcPath=src.count("*(RDX+0x0)")?"*(RDX+0x0)":(src.size()==1?*src.begin():"");std::string trgPath=trg.count("*(R8+0x0)")?"*(R8+0x0)":(trg.size()==1?*trg.begin():"");if(!srcPath.empty())r.mappings.push_back({"source ItemPtr pointee",srcPath});if(!trgPath.empty())r.mappings.push_back({"target ThingPtr pointee",trgPath});if(ga.count("RCX"))r.mappings.push_back({"Game* this","RCX"});r.naturalThreadId=ConsistentThread(r.a,r.b);r.machine=!srcPath.empty()&&ga.count("RCX");r.callerCandidateReady=r.machine&&r.naturalThreadId;r.status=r.machine?(!trgPath.empty()?"confirmed_runtime_arguments":"confirmed_runtime_primary_argument"):"captured_runtime_partial";r.notes="V1.3 snapshots temporary shared_ptr wrappers at breakpoint time. Reference prototype expects RDX=ItemPtr&, R8=ThingPtr&; runtime pointees are required before promotion.";return r;
}

static AbiResult TestTalk(const Profile& p,ActionCaptureFn capture=CaptureTwo){
    AbiResult r;r.key="gameTalk";r.name="Game::talk";r.tested=true;
    if(ActionEntries(p,"talk").empty()){r.status="discovery_required";r.notes="No unique Talk-family method discovered for this build.";return r;}
    std::cout<<"\n[TALK] Selecione o canal Default e envie as duas frases pelo chat normal. Mantenha o mesmo canal e modo nas duas amostras.\n";
    const std::string a="ABI_TEST_ALPHA",b="ABI_TEST_BETA";ActionEntry selected;
    const bool captured=CaptureActionSamples(p,"talk","No jogo fale exatamente: "+a+" ; depois ENTER...\n","No jogo fale exatamente: "+b+" ; depois ENTER...\n",selected,r.a,r.b,capture);
    r.key=selected.key;r.name=selected.name;r.rva=selected.rva;r.runtimeHit=!r.a.empty()||!r.b.empty();
    if(!captured){r.status=r.runtimeHit?"captured_runtime_partial":"rva_not_observed_runtime";r.notes="Talk methods captured separately; no complete natural pair observed.";return r;}
    const auto m=Intersect(MatchString(r.a,a),MatchString(r.b,b));const auto ga=Intersect(MatchKnownGame(r.a,p),MatchKnownGame(r.b,p));
    if(m.size()==1)r.mappings.push_back({"text/string argument",*m.begin()});
    if(ga.count("RCX"))r.mappings.push_back({"Game* this","RCX"});
    r.naturalThreadId=ConsistentThread(r.a,r.b);r.machine=m.size()==1&&ga.count("RCX")&&r.naturalThreadId;
    r.callerCandidateReady=r.machine;r.status=r.machine?"confirmed_runtime_text_argument":"captured_runtime_ambiguous";
    r.notes="Two distinct phrases matched on the observed method only. Talk mode/channel arguments and active-call validation remain pending; no ABI borrowed from the other Talk method.";
    return r;
}

struct PosObs { std::string path; uint32_t x=0,y=0,z=0; int score=0; };
static std::map<std::string,PosObs> PlausiblePositions(const std::vector<Capture>& caps,uint32_t px,uint32_t py,uint32_t pz){
    std::map<std::string,PosObs> out; for(auto& c:caps)for(auto& lv:Locations(c)){
        auto accept=[&](uint32_t x,uint32_t y,uint32_t z,const std::string& k,int baseScore){if(x<1000||y<1000||z>=32)return;if(px&&py&&(std::llabs((long long)x-px)>64||std::llabs((long long)y-py)>64))return;PosObs po{k,x,y,z,baseScore};if(z==pz)po.score+=10;out[k]=po;};
        if(const MemSnap* m=SnapForReg(c,lv.loc)){
            for(int o=0;o<=0x20;o+=4){uint32_t x=0,y=0,z32=0;uint8_t z8=0;if(!SnapRead(*m,o,x)||!SnapRead(*m,o+4,y))continue;uint32_t z=99;if(SnapRead(*m,o+8,z32)&&z32<32)z=z32;else if(SnapRead(*m,o+8,z8)&&z8<32)z=z8;else continue;accept(x,y,z,lv.loc+"->Position(+"+Hex(o)+")",50+PathPreference(lv.loc,"position"));}
        } else {
            uintptr_t q=lv.v;if(q)for(int o=0;o<=0x20;o+=4){uint32_t x=0,y=0,z32=0;uint8_t z8=0;if(!SafeRead(q+o,x)||!SafeRead(q+o+4,y))continue;uint32_t z=99;if(SafeRead(q+o+8,z32)&&z32<32)z=z32;else if(SafeRead(q+o+8,z8)&&z8<32)z=z8;else continue;accept(x,y,z,lv.loc+"->Position(+"+Hex(o)+")",50+PathPreference(lv.loc,"position"));}
        }
    }
    return out;
}

static AbiResult TestLocalPlayerAutoWalk(const Profile& p){
    AbiResult r;r.key="localPlayerAutoWalk";r.name="LocalPlayer::autoWalk";r.rva=p.fnLocalPlayerAutoWalk;r.tested=true;if(!r.rva){r.status="missing_rva";return r;}
    std::cout<<"\n============================================================\nABI TEST - LocalPlayer::autoWalk\n============================================================\nNAO use setas/WASD. Clique em um SQM caminhavel 4-8 SQMs distante para o cliente fazer AUTO-WALK.\n";
    uint32_t px=0,py=0,pz=0;ReadPosition(p,px,py,pz);if(!CaptureTwo(r.rva,"Clique em um destino A distante e, quando o personagem iniciar o auto-walk, ENTER...\n","Clique em um destino B DIFERENTE e, quando iniciar o auto-walk, ENTER...\n",r.a,r.b,3)){r.status="rva_not_observed_runtime";return r;}r.runtimeHit=!r.a.empty()||!r.b.empty();if(r.a.empty()||r.b.empty()){r.status="captured_runtime_partial";r.notes="Only one auto-walk sample hit the RVA.";return r;}
    auto ma=PlausiblePositions(r.a,px,py,pz),mb=PlausiblePositions(r.b,px,py,pz);std::vector<AbiCandidate> cand;for(auto& [k,a]:ma){auto it=mb.find(k);if(it==mb.end())continue;if(a.x==it->second.x&&a.y==it->second.y&&a.z==it->second.z)continue;AbiCandidate c{k,a.score+20,{"plausible Position in sample A","same path plausible in sample B","destination changed"}};if(k.find("RDX")!=std::string::npos)c.score+=20;cand.push_back(c);}std::sort(cand.begin(),cand.end(),[](auto&a,auto&b){return a.score>b.score;});r.candidates=cand;
    uintptr_t lp=ResolveLocalPlayer(p);auto lpA=MatchPointer(r.a,lp),lpB=MatchPointer(r.b,lp);auto lpb=Intersect(lpA,lpB);if(lpb.count("RCX"))r.mappings.push_back({"LocalPlayer* this","RCX"});if(!cand.empty())r.mappings.push_back({"destination Position*",cand.front().path});r.naturalThreadId=ConsistentThread(r.a,r.b);bool strong=!cand.empty()&&(cand.size()==1||cand[0].score>=cand[1].score+15);r.machine=strong&&lpb.count("RCX");r.callerCandidateReady=r.machine&&r.naturalThreadId;r.status=r.machine?"confirmed_runtime_machine_abi":(!cand.empty()?"strong_candidate_runtime_abi":"captured_runtime_raw");r.notes="Click-to-walk samples used; manual single-step movement is intentionally rejected as evidence.";return r;
}

static AbiResult TestGameAutoWalk(const Profile& p){
    AbiResult r;r.key="gameAutoWalk";r.name="Game::autoWalk";r.rva=p.fnGameAutoWalk;r.tested=true;if(!r.rva){r.status="missing_rva";return r;}
    std::cout<<"\n============================================================\nABI TEST - Game::autoWalk RAW CHAIN EVIDENCE\n============================================================\nNAO use setas. Clique em destinos distantes para gerar auto-walk real.\n";
    if(!CaptureTwo(r.rva,"Clique em um destino A 4-8 SQMs distante; ENTER quando iniciar...\n","Clique em um destino B diferente; ENTER quando iniciar...\n",r.a,r.b,3)){r.status="rva_not_observed_runtime";return r;}r.runtimeHit=!r.a.empty()||!r.b.empty();auto ga=Intersect(MatchKnownGame(r.a,p),MatchKnownGame(r.b,p));if(ga.count("RCX"))r.mappings.push_back({"Game* this","RCX"});r.naturalThreadId=ConsistentThread(r.a,r.b);r.status=r.runtimeHit?"captured_runtime_raw_autowalk":"rva_not_observed_runtime";r.notes="Game::autoWalk carries a non-trivial direction vector; V1.3 records natural calls/thread and preserves argument snapshots for the dedicated vector-layout stage.";return r;
}


static AbiResult TestTileReturnEntry(const Profile& p,const char* key,const char* name) {
    AbiResult r; r.key=key; r.name=name; r.rva=DiscoveredRva(p,key); r.tested=true;
    if(!r.rva) { r.status="discovery_required"; return r; }
    if(!CaptureTwo(r.rva,"Arraste um item do chao normalmente no tile A; depois ENTER...\n",
                         "Arraste outro item no tile B; depois ENTER...\n",r.a,r.b,2)) {
        r.status="rva_not_observed_runtime"; return r;
    }
    r.runtimeHit=!r.a.empty() || !r.b.empty();
    r.naturalThreadId=ConsistentThread(r.a,r.b);
    r.status="captured_entry_return_pending";
    r.notes="Passive x64 entry snapshots only. This/hidden return lowering, returned ThingPtr, same-tile membership and ownership balance must be measured before ABI promotion. The x86 ECX/RET4 contract is never imported.";
    return r;
}

static std::string ExistingFunctionObject(const std::string& raw,const std::string& key){std::string abi=ExtractNamedObject(raw,"abiCatalog");std::string f=ExtractNamedObject(abi,"functions");return ExtractNamedObject(f,key);}

static int StatusStrength(const std::string& s){
    if(s.find("confirmed_runtime")!=std::string::npos)return 5;
    if(s.find("strong_candidate")!=std::string::npos)return 4;
    if(s.find("captured_runtime_partial")!=std::string::npos)return 3;
    if(s=="captured_entry_return_pending")return 2;
    if(s.find("captured_runtime")!=std::string::npos)return 2;
    if(s.find("rva_not_observed")!=std::string::npos)return 1;
    return 0;
}

static std::string BuildAbiCatalog(const Profile& p,const Fingerprint& fp,const std::map<std::string,AbiResult>& results){
    std::vector<std::string> keys;
    for(const auto& spec:pipe_discovery::methods) keys.emplace_back(spec.key);
    std::ostringstream j;j<<"{\n    \"schema\": \"otclab-abi-catalog-v4\",\n    \"schemaVersion\": 4,\n    \"builder\": \"OTCLab_ABIBuilder_V1_3_X64_AUTOCALIBRATOR_CALLER_EVIDENCE\",\n    \"policy\": \"guided_runtime_autocalibration_with_natural_caller_resolution\",\n    \"fingerprint\": {\"timestamp\":\""<<Hex(fp.timestamp)<<"\",\"sizeOfImage\":\""<<Hex(fp.sizeOfImage)<<"\",\"textHashFNV1A64\":\""<<Hex(fp.textHash)<<"\"},\n    \"functions\": {\n";
    bool first=true;
    for(auto& key:keys){
        auto it=results.find(key);std::string existing=ExistingFunctionObject(p.raw,key),obj;
        const auto discovered=DiscoveredRva(p,key);
        const auto priorFp=ExtractNamedObject(ExtractNamedObject(p.raw,"abiCatalog"),"fingerprint");
        const bool sameEvidenceBuild=pipe_discovery::exactFingerprint(ParseNum(FlatJsonString(priorFp,"timestamp")),ParseNum(FlatJsonString(priorFp,"sizeOfImage")),ParseNum(FlatJsonString(priorFp,"textHashFNV1A64")),fp.timestamp,fp.sizeOfImage,fp.textHash);
        if(!sameEvidenceBuild || !discovered || ParseNum(FlatJsonString(existing,"rva"))!=discovered) existing.clear();
        if(!existing.empty()) obj=ForcePassive(existing);
        if(it!=results.end()&&it->second.tested){
            std::string fresh=ResultJson(it->second), oldStatus=FlatJsonString(existing,"status");
            if(existing.empty() || StatusStrength(it->second.status)>=StatusStrength(oldStatus)) obj=fresh;
        }
        if(obj.empty()) { AbiResult pending; pending.key=key; pending.name=key; pending.rva=discovered; pending.status=discovered?"abi_validation_required":"discovery_required"; pending.notes="Discovered address is a candidate only. Complete ABI, return/ownership, thread and active call validation remain required."; obj=ResultJson(pending); }
        obj=ForcePassive(obj);
        if(!first)j<<",\n";first=false;j<<"      \""<<key<<"\": "<<obj;
    }
    j<<"\n    },\n    \"lastEvidenceSession\": {\n";
    // Retain the latest inconclusive observations as well as the strongest
    // contract. Skipping a test must not erase its alternative candidates.
    first=true;
    const auto previousSession=ExtractNamedObject(ExtractNamedObject(p.raw,"abiCatalog"),"lastEvidenceSession");
    const auto previousFp=ExtractNamedObject(ExtractNamedObject(p.raw,"abiCatalog"),"fingerprint");
    const bool sameSessionBuild=pipe_discovery::exactFingerprint(ParseNum(FlatJsonString(previousFp,"timestamp")),ParseNum(FlatJsonString(previousFp,"sizeOfImage")),ParseNum(FlatJsonString(previousFp,"textHashFNV1A64")),fp.timestamp,fp.sizeOfImage,fp.textHash);
    for(const auto& key:keys){
        std::string observation;const auto it=results.find(key);
        if(it!=results.end()&&it->second.tested)observation=ResultJson(it->second);
        else if(sameSessionBuild){
            auto prior=ExtractNamedObject(previousSession,key);const auto current=DiscoveredRva(p,key);
            if(current&&ParseNum(FlatJsonString(prior,"rva"))==current)observation=ForcePassive(prior);
        }
        if(observation.empty())continue;
        if(!first)j<<",\n";first=false;j<<"      \""<<key<<"\": "<<observation;
    }
    j<<"\n    },\n";
    j<<"    \"evidencePolicy\": {\"neverDowngradeStrongerExistingEvidence\":true,\"userRole\":\"generate_runtime_transition_only\",\"profileFactsAreReused\":true},\n";
    j<<"    \"callabilityPolicy\": {\"requiresMachineAbiValidated\":true,\"requiresNaturalThreadEvidence\":true,\"requiresCallerSiteValidated\":true,\"requiresActiveCallValidation\":true}\n  }";return j.str();
}

static bool SaveProfile(Profile& p,const Fingerprint& fp,const std::map<std::string,AbiResult>& results){
    std::string raw=ReadText(p.path);if(raw.empty())return false;
    fs::path bak=p.path;bak += L".pre_abi.bak.json";std::error_code ec;if(!fs::exists(bak,ec))WriteText(bak,raw);
    std::string cat=BuildAbiCatalog(p,fp,results);if(!ReplaceOrInsertRootObject(raw,"abiCatalog",cat))return false;
    if(!WriteText(p.path,raw))return false;p.raw=raw;return true;
}

static bool MatchSignaturePrefix(uintptr_t rva,const std::string& sig,int maxBytes=16){
    if(!rva||sig.empty())return false;std::istringstream ss(sig);std::string tok;int i=0,fixed=0,good=0;while(ss>>tok&&i<maxBytes){if(tok=="??"||tok=="?"){++i;continue;}unsigned v=0;try{v=(unsigned)std::stoul(tok,nullptr,16);}catch(...){break;}uint8_t cur=0;if(!SafeRead(g_base+rva+i,cur))return false;++fixed;if(cur==(uint8_t)v)++good;++i;}return fixed>=6&&good==fixed;
}
static void ProfileCrossCheck(const Profile& p){
    struct E{const char* n;uintptr_t r;const std::string* s;};std::vector<E> v={{"attack",p.fnGameAttack,&p.sigGameAttack},{"follow",p.fnGameFollow,&p.sigGameFollow},{"look",p.fnGameLook,&p.sigGameLook},{"move",p.fnGameMove,&p.sigGameMove},{"use",p.fnGameUse,&p.sigGameUse},{"useWith",p.fnGameUseWith,&p.sigGameUseWith},{"autoWalk",p.fnGameAutoWalk,&p.sigGameAutoWalk},{"lpAutoWalk",p.fnLocalPlayerAutoWalk,&p.sigLocalPlayerAutoWalk}};int checked=0,ok=0;std::cout<<"[PROFILE CROSS-CHECK] function prologue prefixes:\n";for(auto&e:v){if(!e.r||e.s->empty())continue;++checked;bool m=MatchSignaturePrefix(e.r,*e.s);if(m)++ok;std::cout<<"  "<<e.n<<" @ "<<Hex(e.r)<<" : "<<(m?"MATCH":"DIFF")<<"\n";}std::cout<<"[PROFILE CROSS-CHECK] matched "<<ok<<"/"<<checked<<" known function prefixes.\n";
}

#include "CompletionSession.hpp"

static void RunAbiSession(){
    LogStage("session_start; completion_tests_v1; no_breakpoint_installed");
    std::cout<<"============================================================\n       OTCLab ABI Builder V1.3 X64 - AUTOCALIBRATOR / CALLER EVIDENCE\n============================================================\nRecebe o BuildProfile pronto e reutiliza Game/Target/Item/Look/Position/RVAs como evidencias.\nO usuario gera somente a acao real no jogo; IDs e ponteiros sao lidos automaticamente quando possivel.\nEtapa inicial passiva; conclusao oferece chamadas ativas guiadas no thread natural. INT3+VEH sao temporarios e reversiveis.\nEvidencia fraca nunca sobrescreve uma evidencia anterior mais forte.\nO jogador apenas executa a acao real; CALL site/caller sao descobertos automaticamente e gravados no JSON.\nV1.3 preserva snapshots e resolve automaticamente return RVA, CALL site e funcao caller natural via .pdata.\n============================================================\n";
    LogStage("fingerprint_begin");
    Fingerprint fp{};if(!GetFingerprint(fp)){std::cout<<"[STOP] Falha no fingerprint.\n";Wait("ENTER...");return;}
    std::cout<<"Timestamp : "<<Hex(fp.timestamp)<<"\nSizeImage : "<<Hex(fp.sizeOfImage)<<"\nTextHash  : "<<Hex(fp.textHash)<<"\n";
    LogStage("fingerprint_ok; profile_search_begin");
    auto paths=FindProfiles();LogStage("profile_search_done");struct Cand{fs::path path;Profile p;int score;};std::vector<Cand> cs;
    for(auto& x:paths){LogStage("profile_parse_begin");Profile p=ParseProfile(x);LogStage("profile_parse_done");if(!p.timestamp||!p.sizeOfImage)continue;if(!pipe_discovery::exactFingerprint(p.timestamp,p.sizeOfImage,p.textHash,fp.timestamp,fp.sizeOfImage,fp.textHash) || !DiscoveryFingerprintMatches(p)) continue;int s=16;{s+=ProfileEvidenceScore(p);cs.push_back({x,p,s});}}
    if(cs.empty()){std::cout<<"[STOP] Nenhum profile com fingerprint exata e pipeDiscovery atual foi encontrado.\nCopie o JSON gerado pelo BuildExplorer para a pasta profiles ao lado da DLL.\n";Wait("ENTER...");return;}
    std::sort(cs.begin(),cs.end(),[](auto&a,auto&b){return a.score>b.score;});
    std::cout<<"\nProfiles compatíveis:\n";for(size_t i=0;i<cs.size();++i)std::cout<<"  #"<<i<<" score="<<cs[i].score<<" "<<DisplayPath(cs[i].path)<<"\n";
    size_t pick=0;if(cs.size()>1){if(cs[0].score>cs[1].score){std::cout<<"[AUTO PROFILE] Selecionado #0 por conter evidencias ABI mais fortes/mais novas.\n";}else{pick=AskU32("Escolha profile",0);if(pick>=cs.size())pick=0;}}Profile p=cs[pick].p;
    std::cout<<"\n[PROFILE] "<<DisplayPath(p.path)<<"\nItem ID off="<<Hex(p.itemIdOffset)<<" Item VTable RVA="<<Hex(p.itemVtableRva)<<"\n";
    LogStage("profile_selected; prologue_cross_check_begin");
    ProfileCrossCheck(p); // Diagnostic only; discovery and fingerprint gates remain mandatory.
    ShowSavedCandidateReview(p);
    LogStage("initialization_complete; waiting_for_user; no_breakpoint_installed");
    std::map<std::string,AbiResult> results;
    if(p.fnGameAttack&&AskYes("Testar ABI Game::attack com evidencias do profile?",true))results["gameAttack"]=TestAttackFollow(p,true);
    if(p.fnGameFollow&&AskYes("Testar ABI Game::follow com evidencias do profile?",true))results["gameFollow"]=TestAttackFollow(p,false);
    if((p.fnGameDetailedLook||p.fnGameLook)&&AskYes("Calibrar Look Assistant automatico (sem digitar IDs)?",true)){auto r=TestLook(p);results[r.key]=r;}
    if(p.fnGameMove&&AskYes("Testar ABI Game::move com Look Assistant?",true))results["gameMove"]=TestMove(p);
    if(p.fnGameUse&&AskYes("Testar ABI Game::use com Look Assistant?",true))results["gameUse"]=TestUse(p);
    if(p.fnGameUseWith&&AskYes("Testar ABI Game::useWith com Look Assistant?",true))results["gameUseWith"]=TestUseWith(p);
    if(!ActionEntries(p,"talk").empty()&&AskYes("Testar ABI do chat normal (Talk/TalkChannel descobertos)?",false)){auto r=TestTalk(p);results[r.key]=r;}
    if(p.fnLocalPlayerAutoWalk&&AskYes("Testar ABI LocalPlayer::autoWalk por CLICK-TO-WALK?",true))results["localPlayerAutoWalk"]=TestLocalPlayerAutoWalk(p);
    if(p.fnGameAutoWalk&&AskYes("Capturar cadeia Game::autoWalk por CLICK-TO-WALK?",false))results["gameAutoWalk"]=TestGameAutoWalk(p);
    if(DiscoveredRva(p,"tileGetTopMoveThing") && AskYes("Capturar entrada Tile::getTopMoveThing (retorno permanece pendente)?",true))
        results["tileGetTopMoveThing"]=TestTileReturnEntry(p,"tileGetTopMoveThing","Tile::getTopMoveThing");
    if(DiscoveredRva(p,"tileGetTopUseThing") && AskYes("Capturar entrada Tile::getTopUseThing (retorno permanece pendente)?",false))
        results["tileGetTopUseThing"]=TestTileReturnEntry(p,"tileGetTopUseThing","Tile::getTopUseThing");
    for(auto& [k,r]:results)FinalizeCallerEvidence(r);
    std::cout<<"\n============================================================\nABI SESSION SUMMARY\n============================================================\n";
    for(auto& [k,r]:results){
        std::cout<<std::left<<std::setw(24)<<r.name<<" : "<<r.status<<" mappings="<<r.mappings.size()<<"\n";
        for(auto&m:r.mappings)std::cout<<"    "<<m.first<<" -> "<<m.second<<"\n";
        if(r.naturalCallerObserved){
            std::cout<<"    caller site -> "<<Hex(r.naturalCallSiteRva)<<" ("<<r.naturalCallKind<<")";
            if(r.naturalCallerFunctionRva)std::cout<<" function="<<Hex(r.naturalCallerFunctionRva);
            std::cout<<" samples="<<r.callerSamples<<" validated="<<(r.callerSiteValidated?"YES":"NO")<<"\n";
        }else std::cout<<"    caller site -> unresolved\n";
    }
    if(SaveProfile(p,fp,results))std::cout<<"\n[SAVE][OK] ABI catalog gravado no MESMO JSON:\n"<<DisplayPath(p.path)<<"\nBackup preservado: "<<DisplayPath(p.path)<<".pre_abi.bak.json\n";else std::cout<<"\n[SAVE][FAIL] Nao consegui atualizar JSON.\n";
    RunCompletionTests(p,fp,results);
    std::cout<<"\n[ABI COMPLETION] Retorno/refcount, vetor e chamadas ativas sao etapas separadas. Resultados ficam em abiCompletion; testes incompletos e replay de holders existentes nao habilitam construcao independente de argumentos.\n";
    Wait("ENTER para encerrar a sessao ABI Builder...\n");
}

static DWORD WINAPI MainThread(LPVOID){
    OpenStartupLog();
    HANDLE input=INVALID_HANDLE_VALUE,output=INVALID_HANDLE_VALUE;
    bool ownsConsole=false;
    try {
        LogStage("console_attach_begin");
        ownsConsole=AllocConsole()!=FALSE;
        output=CreateFileW(L"CONOUT$",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        input=CreateFileW(L"CONIN$",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        if(input==INVALID_HANDLE_VALUE||output==INVALID_HANDLE_VALUE){
            LogStage("console_open_failed; session_aborted_without_hooks");
        }else{
            ScopedConsoleStreams streams(input,output);
            if(ownsConsole){SetConsoleCP(CP_UTF8);SetConsoleOutputCP(CP_UTF8);}
            LogStage("console_ready; shared_CRT_stdio_unchanged");
            if(ownsConsole)SetConsoleTitleA("OTCLab ABI Builder V1.3 X64 - Startup Diagnostic");
            RunAbiSession();
        }
    }catch(const std::exception& e){LogStage("session_exception");LogStage(e.what());}
    catch(...){LogStage("session_unknown_cpp_exception");}
    // Do not unload code while a capture is still armed.
    if(InterlockedCompareExchange(&g_bp.installed,0,0) || completion_probe::state.installed){
        LogStage("capture_still_installed; DLL_retained_to_avoid_unsafe_unload");
        return 0;
    }
    if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);
    if(output!=INVALID_HANDLE_VALUE)CloseHandle(output);
    if(ownsConsole)FreeConsole();
    LogStage("session_finished; completion_handler_retained_if_used");
    if(g_startupLog!=INVALID_HANDLE_VALUE){CloseHandle(g_startupLog);g_startupLog=INVALID_HANDLE_VALUE;}
    if(completion_probe::retainedHandler)return 0; // Delayed INT3 handler lives until client shutdown.
    FreeLibraryAndExitThread(g_self,0);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){g_self=h;DisableThreadLibraryCalls(h);HANDLE t=CreateThread(nullptr,0,MainThread,nullptr,0,nullptr);if(t)CloseHandle(t);}return TRUE;}
