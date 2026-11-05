// Additional READ-ONLY searches. Included after all existing discovery stages.
// This module never changes DbProfile or promotes its own hypotheses.
struct ExtraType {
    std::string name;std::vector<uintptr_t> vtables,objects;std::size_t hits{};
};
struct ExtraScan {std::vector<ExtraType> types;std::size_t bytes{};bool partial{};};

bool ExtraReadable(DWORD protection){
    return otclab::extra::readable(protection);
}
bool ExtraBytes(uintptr_t address,std::size_t requested,std::vector<std::uint8_t>& bytes){
    return otclab::extra::snapshot(address,requested,bytes);
}
ExtraScan ExtraRttiObjects(){
    ExtraScan out;
    const char* names[]={"LocalPlayer","Player","Monster","Npc","Item","Container","Tile","Map","Game"};
    std::map<uintptr_t,std::size_t> needles;
    for(const char* name:names){
        ExtraType type;type.name=name;type.vtables=FindMsvcVtablesByExactRttiName(std::string(".?AV")+name+"@@");
        for(auto vt:type.vtables)needles[vt]=out.types.size();out.types.push_back(std::move(type));
    }
    if(needles.empty())return out;
    SYSTEM_INFO info{};GetSystemInfo(&info);uintptr_t address=reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const auto maximum=reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);auto deadline=GetTickCount64()+15000;
    while(address<maximum){
        if(GetTickCount64()>deadline||out.bytes>=2ull*1024*1024*1024){out.partial=true;break;}
        MEMORY_BASIC_INFORMATION mbi{};if(!VirtualQuery(reinterpret_cast<void*>(address),&mbi,sizeof(mbi)))break;
        auto base=reinterpret_cast<uintptr_t>(mbi.BaseAddress),end=base+mbi.RegionSize;if(end<=address)break;
        if(mbi.State==MEM_COMMIT&&mbi.Type==MEM_PRIVATE&&ExtraReadable(mbi.Protect)){
            for(auto cursor=base;cursor<end;cursor+=65536){
                if(GetTickCount64()>deadline||out.bytes>=2ull*1024*1024*1024){out.partial=true;break;}
                std::vector<std::uint8_t> bytes;if(!ExtraBytes(cursor,std::min<std::size_t>(65536,end-cursor),bytes)){out.partial=true;continue;}
                out.bytes+=bytes.size();
                for(std::size_t off=0;off+8<=bytes.size();off+=8){
                    uintptr_t value{};std::memcpy(&value,bytes.data()+off,8);auto it=needles.find(value);if(it==needles.end())continue;
                    auto& type=out.types[it->second];++type.hits;if(type.objects.size()<256)type.objects.push_back(cursor+off);
                }
            }
        }
        address=end;
    }
    std::cout<<"[EXTRA RTTI] "<<out.bytes/(1024*1024)<<" MB; cobertura="<<(out.partial?"PARCIAL":"address_space_end")<<"\n";
    for(const auto& type:out.types)std::cout<<"  "<<type.name<<": "<<type.vtables.size()<<" vtable(s), "<<type.hits<<" ocorrencias candidatas.\n";
    return out;
}
std::string ExtraTypesJson(const ExtraScan& scan){
    std::ostringstream out;out<<"{\"status\":\"candidate_only\",\"partial\":"<<(scan.partial?"true":"false")<<",\"bytesRead\":"<<scan.bytes<<",\"types\":[";
    bool comma=false;for(const auto& type:scan.types){if(comma)out<<',';comma=true;
        out<<"{\"name\":\""<<EscapeJson(type.name)<<"\",\"occurrences\":"<<type.hits<<",\"vtableRvas\":[";
        for(std::size_t i=0;i<type.vtables.size();++i){if(i)out<<',';out<<'"'<<HexU64(type.vtables[i]-g_exeBase)<<'"';}
        out<<"],\"sampleObjects\":[";for(std::size_t i=0;i<type.objects.size();++i){if(i)out<<',';out<<'"'<<HexPtr(type.objects[i])<<'"';}out<<"]}";
    }out<<"]}";return out.str();
}
std::string ExtraPathsJson(const otclab::extra::SearchResult& search){
    std::ostringstream out;out<<"{\"bounded\":"<<(search.bounded?"true":"false")<<",\"nodes\":"<<search.nodes<<",\"paths\":[";
    for(std::size_t i=0;i<search.paths.size();++i){if(i)out<<',';const auto& p=search.paths[i];
        out<<"{\"rootRva\":\""<<HexU64(p.root-g_exeBase)<<"\",\"target\":\""<<HexPtr(p.target)<<"\",\"terminalOwner\":\""<<HexPtr(p.terminalOwner)<<"\",\"offsets\":[";
        for(std::size_t j=0;j<p.offsets.size();++j){if(j)out<<',';out<<'"'<<HexU64(p.offsets[j])<<'"';}out<<"]}";
    }out<<"]}";return out.str();
}
otclab::extra::Reader ExtraPrivateReader(uintptr_t root){
    return [root](uintptr_t address,std::size_t requested,std::vector<std::uint8_t>& bytes){
        MEMORY_BASIC_INFORMATION mbi{};
        if(!VirtualQuery(reinterpret_cast<void*>(address),&mbi,sizeof(mbi)))return false;
        if(address!=root&&mbi.Type!=MEM_PRIVATE)return false;
        return ExtraBytes(address,requested,bytes);
    };
}
std::string ExtraInventory(const DbProfile& original,const ExtraScan& types){
    auto copy=original;const auto player=ResolveProfileLocalPlayerForDiscovery(copy);std::set<uintptr_t> targets;
    for(const auto& type:types.types)if(type.name=="Item")targets.insert(type.objects.begin(),type.objects.end());
    if(!player||targets.empty())return "{\"status\":\"no_item_anchors\"}";
    auto deadline=GetTickCount64()+3000;
    auto result=otclab::extra::pointerPaths(player,targets,ExtraPrivateReader(player),192,2,[&]{return GetTickCount64()>deadline;});
    // This is a LocalPlayer-relative diagnostic, not a module-global resolver.
    std::ostringstream out;out<<"{\"status\":\"reachable_items_not_confirmed_inventory\",\"player\":\""<<HexPtr(player)<<"\",\"bounded\":"<<(result.bounded?"true":"false")<<",\"paths\":[";
    for(std::size_t i=0;i<result.paths.size();++i){if(i)out<<',';out<<"{\"item\":\""<<HexPtr(result.paths[i].target)<<"\",\"offsets\":[";
        for(std::size_t j=0;j<result.paths[i].offsets.size();++j){if(j)out<<',';out<<'"'<<HexU64(result.paths[i].offsets[j])<<'"';}out<<"]}";}
    out<<"]}";std::cout<<"[EXTRA INVENTORY] "<<result.paths.size()<<" caminhos LP -> Item; posse/slots continuam pendentes.\n";return out.str();
}
std::string ExtraNumeric(const DbProfile& original,const char* name){
    std::cout<<"\n[EXTRA "<<name<<"] Acrescenta u16/u32/u64/f32/f64 e escalas de capacidade.\n";
    if(!AskYesNo((std::string("Consegue alterar ")+name+" sem andar/relogar agora? [S/N]: ").c_str()))return "{\"status\":\"skipped\"}";
    auto copy=original;const auto player=ResolveProfileLocalPlayerForDiscovery(copy);if(!player)return "{\"status\":\"player_unavailable\"}";
    const auto position=ReadDiscoveredPlayerPosition(player,copy);
    double oldValue=AskNumber((std::string(name)+" atual EXATO na tela: ").c_str());std::vector<std::uint8_t> before;
    if(!ExtraBytes(player,0x2400,before))return "{\"status\":\"snapshot_failed\"}";
    WaitEnter((std::string("Altere ")+name+" no jogo, mantendo o SQM. Depois ENTER...").c_str());
    double newValue=AskNumber((std::string(name)+" novo EXATO na tela: ").c_str());std::vector<std::uint8_t> after;
    if(!std::isfinite(oldValue)||!std::isfinite(newValue)||oldValue<0||newValue<0)return "{\"status\":\"invalid_display_values\"}";
    const auto current=ResolveProfileLocalPlayerForDiscovery(copy);const auto newPosition=ReadDiscoveredPlayerPosition(current,copy);
    if(current!=player||position.x!=newPosition.x||position.y!=newPosition.y||position.z!=newPosition.z)return "{\"status\":\"identity_or_position_changed\"}";
    if(!ExtraBytes(player,0x2400,after))return "{\"status\":\"snapshot_failed\"}";
    auto candidates=otclab::extra::transitions(before,after,oldValue,newValue);
    if(std::string(name)!="Capacity")std::erase_if(candidates,[](const auto& c){return c.scale!=1;});
    const auto firstCount=candidates.size();
    bool thirdObserved=false;double thirdValue=0;std::vector<std::uint8_t> third;
    if(!candidates.empty()&&AskYesNo("Validar com uma TERCEIRA medida diferente, no mesmo SQM? [S/N]: ")){
        WaitEnter("Altere o valor novamente (ou restaure o inicial). Depois ENTER...");
        thirdValue=AskNumber("Terceiro valor EXATO na tela: ");
        const auto lastPlayer=ResolveProfileLocalPlayerForDiscovery(copy);const auto lastPosition=ReadDiscoveredPlayerPosition(lastPlayer,copy);
        if(lastPlayer!=player||lastPosition.x!=position.x||lastPosition.y!=position.y||lastPosition.z!=position.z)return "{\"status\":\"third_identity_or_position_changed\"}";
        if(!std::isfinite(thirdValue)||thirdValue<0||otclab::extra::valuesClose(thirdValue,newValue))return "{\"status\":\"invalid_third_value\"}";
        if(!ExtraBytes(player,0x2400,third))return "{\"status\":\"third_snapshot_failed\"}";
        otclab::extra::filterObservation(candidates,third,thirdValue);thirdObserved=true;
    }
    std::cout<<"[EXTRA "<<name<<"] "<<firstCount<<" hipoteses iniciais; "<<candidates.size()<<" restantes; terceira medida="<<(thirdObserved?"SIM":"PENDENTE")<<". Inteiro/decimal/escalas preservados.\n";
    std::ostringstream out;out<<"{\"status\":\"candidate_only\",\"initialHypotheses\":"<<firstCount<<",\"thirdObserved\":"<<(thirdObserved?"true":"false")<<",\"displayThird\":"<<(thirdObserved?std::to_string(thirdValue):"null")<<",\"displayBefore\":"<<oldValue<<",\"displayAfter\":"<<newValue<<",\"hypotheses\":[";
    for(std::size_t i=0;i<candidates.size();++i){if(i)out<<',';const auto& c=candidates[i];
        out<<"{\"offset\":\""<<HexU64(c.offset)<<"\",\"type\":\""<<otclab::extra::storageName(c.storage)<<"\",\"displayScale\":"<<c.scale<<",\"displayMode\":\""<<c.displayMode<<"\",\"rawBefore\":"<<c.before<<",\"rawAfter\":"<<c.after;
        if(thirdObserved)out<<",\"rawThird\":"<<otclab::extra::value(third,c.offset,c.storage);out<<"}";
    }out<<"]}";return out.str();
}
bool BMRecoverTorchReadEvidence(const BuildInfo& build,DbProfile& p){
    wchar_t module[32768]{};if(!GetModuleFileNameW(g_dllModule,module,32768))return false;
    const auto directory=std::filesystem::path(module).parent_path();std::error_code error;
    std::set<std::size_t> matched;
    for(const auto& entry:std::filesystem::directory_iterator(directory,error)){
        if(error)break;const auto name=entry.path().filename().wstring();
        if(name.find(L"OTCLab_SearchEvidence_")!=0||entry.path().extension()!=L".json")continue;
        const auto size=entry.file_size(error);if(error||size>4*1024*1024){error.clear();continue;}
        std::ifstream input(entry.path());std::ostringstream data;data<<input.rdbuf();const auto json=data.str();
        if(ParseProfileNumber(FlatJsonString(json,"timestamp"))!=build.timestamp||ParseProfileNumber(FlatJsonString(json,"textHash"))!=build.textHash)continue;
        auto cycles=V39ExtractNamedJsonObject(json,"lightCycles");if(cycles.empty())continue;
        std::string compact;for(unsigned char c:cycles)if(c!=' '&&c!='\r'&&c!='\n'&&c!='\t')compact+=char(c);
        if(compact.find("\"stages\":[\"baseline\",\"torch\",\"baseline\"")==std::string::npos)continue;
        auto at=cycles.find("\"hypotheses\"");if(at==std::string::npos)continue;
        while((at=cycles.find('{',at))!=std::string::npos){const auto end=cycles.find('}',at);if(end==std::string::npos)break;
            const auto row=cycles.substr(at,end-at+1);at=end+1;
            if(FlatJsonString(row,"type")!="f32")continue;
            const auto off=ParseProfileNumber(FlatJsonString(row,"offset"));if(!off||off>0x2400-4)continue;
            auto valuesAt=row.find("\"values\"");if(valuesAt==std::string::npos)continue;valuesAt=row.find('[',valuesAt);if(valuesAt==std::string::npos)continue;
            std::string numbers=row.substr(valuesAt+1);for(auto& c:numbers)if(c==',')c=' ';
            std::istringstream values(numbers);double a{},b{},c{};if(!(values>>a>>b>>c)||!otclab::extra::lightCycle({a,b,c})||b<=a||b>255)continue;
            // Require a float load with the same displacement in this build's code.
            bool foundLoad=false;
            for(uintptr_t code=g_textBegin;code<g_textEnd&&!foundLoad;){
                std::vector<std::uint8_t> block;const auto requested=std::min<std::size_t>(0x10000,g_textEnd-code);
                if(!ExtraBytes(code,requested,block)||block.empty())break;
                for(std::size_t i=0;i+9<=block.size();++i){const auto* bytes=block.data()+i;
                    if(bytes[0]!=0xF3)continue;unsigned op=(bytes[1]>=0x40&&bytes[1]<=0x4F)?2:1;
                    if(bytes[op]!=0x0F||bytes[op+1]!=0x10||(bytes[op+2]&0xC0)!=0x80||(bytes[op+2]&7)==4)continue;
                    int32_t displacement{};std::memcpy(&displacement,bytes+op+3,4);
                    if(displacement==static_cast<int32_t>(off)){matched.insert(off);foundLoad=true;break;}
                }
                code+=block.size()>8?block.size()-8:block.size();
            }
        }
    }
    if(matched.size()!=1)return false;
    p.playerLightReadOffset=*matched.begin();p.playerLightReadType="f32";p.playerLightReadStatus="confirmed_runtime_torch_read";
    std::cout<<"[LIGHT TYPED RESUME] Evidencia de tocha desta fingerprint + leitura float no codigo: +"<<HexU64(p.playerLightReadOffset)<<" f32. Escrita ainda sera testada.\n";return true;
}
std::string ExtraLightCycles(DbProfile& original,bool includeSpells=false){
    std::cout<<"\n[EXTRA LIGHT] Apenas leitura. Voce aplica/remove a luz no jogo.\n"
             <<"Cor, intensidade e alcance NAO serao assumidos pelo valor numerico.\n";
    if(!AskYesNo("Consegue acender/remover a tocha e restaurar a luz inicial? [S/N]: "))return "{\"status\":\"skipped\"}";
    const bool lux=includeSpells&&AskYesNo("Incluir Utevo Lux? [S/N] (N deixa pendente): ");
    const bool gran=includeSpells&&AskYesNo("Incluir Utevo Gran Lux? [S/N] (N deixa pendente): ");
    auto copy=original;uintptr_t player=0;MapPosition pos{};
    std::vector<const char*> instructions={"SEM luz de tocha/magia. Escolha o SQM e pare. ENTER...","Acenda/equipe a TOCHA, no mesmo SQM. ENTER...","Apague/remova a tocha. Restaure a luz inicial. ENTER..."};
    std::vector<const char*> stageNames={"baseline","torch","baseline"};
    if(lux){instructions.push_back("Use UTEVO LUX, no mesmo SQM. ENTER...");instructions.push_back("Espere acabar Utevo Lux e restaure a luz inicial. ENTER...");stageNames.push_back("lux");stageNames.push_back("baseline");}
    if(gran){instructions.push_back("Use UTEVO GRAN LUX, no mesmo SQM. ENTER...");instructions.push_back("Espere acabar a magia e restaure a luz inicial. ENTER...");stageNames.push_back("gran_lux");stageNames.push_back("baseline");}
    std::vector<std::vector<std::uint8_t>> snapshots;
    for(auto instruction:instructions){
        WaitEnter(instruction);if(!AskYesNo("Estado solicitado pronto? [S/N] (N encerra como pendente): "))return "{\"status\":\"incomplete_cycle\"}";
        auto current=ResolveProfileLocalPlayerForDiscovery(copy);auto now=ReadDiscoveredPlayerPosition(current,copy);
        if(!current||!MapPositionPlausible(now))return "{\"status\":\"player_unavailable\"}";
        if(snapshots.empty()){player=current;pos=now;std::cout<<"[EXTRA LIGHT] Inicio registrado APOS preparacao: "<<pos.x<<','<<pos.y<<','<<pos.z<<".\n";}
        if(current!=player||now.x!=pos.x||now.y!=pos.y||now.z!=pos.z){std::cout<<"[EXTRA LIGHT] Personagem/SQM mudou durante o ciclo. Pendente.\n";return "{\"status\":\"identity_or_position_changed\"}";}
        std::vector<std::uint8_t> bytes;if(!ExtraBytes(player,0x2400,bytes))return "{\"status\":\"snapshot_failed\"}";snapshots.push_back(std::move(bytes));
    }
    std::ostringstream out;out<<"{\"status\":\"correlation_only\",\"semanticsValidated\":false,\"writeValidated\":false,\"luxObserved\":"<<(lux?"true":"false")<<",\"granLuxObserved\":"<<(gran?"true":"false")<<",\"stages\":[";
    for(std::size_t i=0;i<stageNames.size();++i){if(i)out<<',';out<<'"'<<stageNames[i]<<'"';}out<<"],\"hypotheses\":[";
    unsigned count=0;
    std::vector<std::pair<std::size_t,otclab::extra::Storage>> readCandidates;
    for(auto type:{otclab::extra::Storage::U8,otclab::extra::Storage::U16,otclab::extra::Storage::U32,otclab::extra::Storage::U64,otclab::extra::Storage::F32,otclab::extra::Storage::F64}){
        auto size=snapshots[0].size();for(const auto& s:snapshots)size=std::min(size,s.size());
        for(std::size_t off=0;off+otclab::extra::width(type)<=size;off+=otclab::extra::width(type)){
            std::vector<double> values;for(const auto& s:snapshots)values.push_back(otclab::extra::value(s,off,type));
            if(!otclab::extra::lightCycle(values))continue;
            readCandidates.push_back({off,type});
            if(count++)out<<',';out<<"{\"offset\":\""<<HexU64(off)<<"\",\"type\":\""<<otclab::extra::storageName(type)<<"\",\"values\":[";
            for(std::size_t i=0;i<values.size();++i){if(i)out<<',';out<<values[i];}out<<"]}";
            std::cout<<"  #"<<(count-1)<<" +"<<HexU64(off)<<" "<<otclab::extra::storageName(type)<<" : ";for(auto n:values)std::cout<<n<<' ';std::cout<<"(correlacao; significado pendente)\n";
        }
    }
    out<<"]}";std::cout<<"[EXTRA LIGHT] "<<count<<" hipoteses. Nenhuma escrita habilitada.\n";
    if(!readCandidates.empty()){
        const auto selected=AskNumber("Campo COMPLETO validado para leitura (numero acima; -1 manter candidatos): ");
        if(std::isfinite(selected)&&selected>=0&&selected<readCandidates.size()&&std::floor(selected)==selected&&
           AskYesNo("A luz visivel acompanhou tocha ON/OFF e este e o campo completo (nao parte de outro tipo)? [S/N]: ")){
            const auto& c=readCandidates[static_cast<std::size_t>(selected)];
            original.playerLightReadOffset=c.first;original.playerLightReadType=otclab::extra::storageName(c.second);
            original.playerLightReadStatus="confirmed_runtime_torch_read";
            std::cout<<"[LIGHT READ SAVED] +"<<HexU64(c.first)<<" "<<original.playerLightReadType<<". So leitura; nao exige magia e nao habilita escrita.\n";
        }
    }
    return out.str();
}
std::set<uintptr_t> ExtraCurrentTiles(const DbProfile& original,MapPosition& position){
    auto copy=original;auto player=ResolveProfileLocalPlayerForDiscovery(copy);std::set<uintptr_t> targets;
    if(!player||!copy.tileVtableRva)return targets;
    position=ReadDiscoveredPlayerPosition(player,copy);if(!MapPositionPlausible(position))return targets;
    auto candidates=FindTileCandidatesByKnownVtableReadOnly(copy,player,position);
    const auto nowPlayer=ResolveProfileLocalPlayerForDiscovery(copy);const auto nowPosition=ReadDiscoveredPlayerPosition(nowPlayer,copy);
    if(nowPlayer!=player||nowPosition.x!=position.x||nowPosition.y!=position.y||nowPosition.z!=position.z)return targets;
    for(const auto& c:candidates)if(c.vectorOffset==copy.tileThingsBegin&&c.vtable==g_exeBase+copy.tileVtableRva)targets.insert(c.objectBase);
    return targets;
}
std::string ExtraMapPaths(const DbProfile& original){
    if(!original.tileVtableRva)return "{\"status\":\"tile_rtti_pending\"}";
    std::vector<uintptr_t> roots;
    if(original.mapRva)roots.push_back(g_exeBase+original.mapRva);
    if(g_v49DirectState.mapObjectRva){auto bind=g_exeBase+g_v49DirectState.mapObjectRva;if(std::find(roots.begin(),roots.end(),bind)==roots.end())roots.push_back(bind);}
    if(roots.empty())return "{\"status\":\"no_map_roots\"}";
    std::cout<<"\n[EXTRA MAP] Testando "<<roots.size()<<" donos separadamente. Nenhum substitui mapRva.\n";
    WaitEnter("Fique PARADO no SQM A. ENTER para buscar caminhos adicionais...");
    MapPosition a{};auto firstTargets=ExtraCurrentTiles(original,a);if(firstTargets.empty())return "{\"status\":\"no_tile_A\"}";
    std::vector<otclab::extra::SearchResult> first;
    for(auto root:roots){auto deadline=GetTickCount64()+4000;
        auto result=otclab::extra::pointerPaths(root,firstTargets,ExtraPrivateReader(root),256,4,[&]{return GetTickCount64()>deadline;});
        std::cout<<"  Root RVA="<<HexU64(root-g_exeBase)<<" A: "<<result.paths.size()<<" caminhos; nodes="<<result.nodes<<"; "<<(result.bounded?"LIMITE/PARCIAL":"fim da busca delimitada")<<".\n";first.push_back(std::move(result));}
    WaitEnter("Ande para OUTRO SQM no MESMO andar e pare. ENTER para snapshot B...");
    MapPosition b{};auto secondTargets=ExtraCurrentTiles(original,b);
    if(a.z!=b.z||(a.x==b.x&&a.y==b.y)||secondTargets.empty())return "{\"status\":\"invalid_position_B\"}";
    std::ostringstream out;out<<"{\"status\":\"candidate_only_no_lookup_promotion\",\"gameStateAnchorRva\":\""<<HexU64(original.gameRva)<<"\",\"gameBindOwnerRva\":\""<<HexU64(g_v49DirectState.gameObjectBaseRva)<<"\",\"roots\":[";
    for(std::size_t i=0;i<roots.size();++i){if(i)out<<',';auto deadline=GetTickCount64()+4000;
        auto second=otclab::extra::pointerPaths(roots[i],secondTargets,ExtraPrivateReader(roots[i]),256,4,[&]{return GetTickCount64()>deadline;});
        unsigned repeated=0;for(const auto& p:first[i].paths)for(const auto& q:second.paths)if(otclab::extra::sameParentSignature(p,q))++repeated;
        std::cout<<"  Root RVA="<<HexU64(roots[i]-g_exeBase)<<" B: "<<second.paths.size()<<"; nodes="<<second.nodes<<"; "<<(second.bounded?"LIMITE/PARCIAL":"fim da busca delimitada")<<"; pares de prefixo repetido="<<repeated<<" (candidatos).\n";
        out<<"{\"source\":\""<<(roots[i]==g_exeBase+original.mapRva?"runtime_center_anchor":"current_build_getTile_bind")<<"\",\"rootRva\":\""<<HexU64(roots[i]-g_exeBase)<<"\",\"repeatedParentPairs\":"<<repeated<<",\"A\":"<<ExtraPathsJson(first[i])<<",\"B\":"<<ExtraPathsJson(second)<<"}";
    }out<<"]}";return out.str();
}
void RunSupplementarySearches(const BuildInfo& build,DbProfile& original){
    const auto previousOffset=original.playerLightReadOffset;const auto previousType=original.playerLightReadType;const auto previousStatus=original.playerLightReadStatus;
    std::ostringstream sections;bool comma=false;auto add=[&](const char* name,const std::string& json){if(comma)sections<<',';comma=true;sections<<'"'<<name<<"\":"<<json;};
    std::cout<<"\n================ BUSCAS EXTRAS READ-ONLY ================\n"
             <<"Candidatos preservam campos atuais. Tocha pode adicionar leitura opcional ao perfil.\n"
             <<"1 RTTI + itens ligados ao Player | 2 Map -> Tile ampliado\n"
             <<"3 Capacity com tipos/escalas | 4 Todas | 5 Mana com outros tipos\n"
             <<"6 Luz: somente TOCHA (suficiente para leitura)\n"
             <<"7 Luz: testes adicionais opcionais de magias\n"
             <<"0 Continuar sem extras\n";
    auto choice=AskNumber("Busca extra: ");if(choice!=1&&choice!=2&&choice!=3&&choice!=4&&choice!=5&&choice!=6&&choice!=7)return;
    if(choice==1||choice==4){auto scan=ExtraRttiObjects();add("rtti",ExtraTypesJson(scan));add("inventoryReachability",ExtraInventory(original,scan));}
    if(choice==2||choice==4)add("mapReachability",ExtraMapPaths(original));
    if(choice==3||choice==4){if(!original.capacity)add("capacity",ExtraNumeric(original,"Capacity"));else add("capacity","{\"status\":\"existing_field_preserved\"}");}
    if(choice==5||choice==4)add("mana",ExtraNumeric(original,"Mana"));
    if(choice==6||choice==7)add("lightCycles",ExtraLightCycles(original,choice==7));
    wchar_t module[32768]{};if(!GetModuleFileNameW(g_dllModule,module,32768)){std::cout<<"[EXTRA] Falha ao localizar pasta do relatorio.\n";return;}
    auto directory=std::filesystem::path(module).parent_path();
    auto file=directory/("OTCLab_SearchEvidence_"+HexU64(build.timestamp).substr(2)+"_"+HexU64(build.textHash).substr(2)+"_R4_"+std::to_string(GetTickCount64())+".json");
    const bool changed=previousOffset!=original.playerLightReadOffset||previousType!=original.playerLightReadType||previousStatus!=original.playerLightReadStatus;
    std::ofstream output(file);output<<"{\"schema\":\"otclab-additional-search-v1\",\"profileModified\":"<<(changed?"true":"false")<<",\"callable\":false,\"timestamp\":\""<<HexU64(build.timestamp)<<"\",\"textHash\":\""<<HexU64(build.textHash)<<"\",\"sections\":{"<<sections.str()<<"}}";output.close();
    std::cout<<(output?"[EXTRA SAVE] Relatorio SearchEvidence salvo ao lado da DLL.\n":"[EXTRA SAVE] Falha ao salvar relatorio.\n");
}
