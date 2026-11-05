#include "../Source/OTCLab_ABIBuilder_V1_3_X64_AUTOCALIBRATOR_CALLER_EVIDENCE.cpp"

static void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}

int main(){
    try{
        const auto dir=fs::temp_directory_path()/(L"ABI_\u00c1rea_"+std::to_wstring(GetCurrentProcessId()))/L"\u00c1rea de Trabalho";
        fs::create_directories(dir);
        const auto file=dir/L"perfil_a\u00e7\u00e3o.JSON";
        const std::string raw=R"({"migratorState":{"timestamp":"0x1234","sizeOfImage":"0x20000","textHashFNV1A64":"0x5678"},"pipeDiscovery":{"schema":"crome-pipe-discovery-x64-v1","architecture":"x64","fingerprint":{"timestamp":"0x1234","sizeOfImage":"0x20000","textHashFNV1A64":"0x5678"},"functions":{"gameMove":{"rva":"0x1000","status":"unique_bind_candidate","candidateCount":1}}}})";
        Check(WriteText(file,raw),"Unicode profile write failed");
        const auto paths=FindProfilesInDirectories({dir,dir,dir/L"missing"});
        Check(paths.size()==1&&paths.front()==file,"Unicode search or dedup failed");
        auto profile=ParseProfile(paths.front());
        Check(profile.path==file&&profile.fnGameMove==0x1000,"Unicode profile parse failed");
        Check(DisplayPath(file).find("\xC3\x81rea de Trabalho")!=std::string::npos,"UTF-8 path display failed");
        Fingerprint fp{};fp.timestamp=0x1234;fp.sizeOfImage=0x20000;fp.textHash=0x5678;
        Check(SaveProfile(profile,fp,{}),"Unicode ABI save failed");
        auto backup=file;backup+=L".pre_abi.bak.json";
        Check(ReadText(backup)==raw,"Unicode backup lost");
        Check(!ExtractNamedObject(ReadText(file),"abiCatalog").empty(),"Unicode ABI catalog missing");
        g_self=GetModuleHandleW(nullptr);
        wchar_t module[32768]{};Check(GetModuleFileNameW(g_self,module,32768)!=0,"test module path missing");
        Check(DllDir()==fs::path(module).parent_path(),"wide DLL directory resolution failed");
        fs::remove(backup);fs::remove(file);fs::remove(dir);fs::remove(dir.parent_path());
        std::cout<<"PASS: Unicode directory search, profile read/save/backup, exact discovery and UTF-8 display\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
