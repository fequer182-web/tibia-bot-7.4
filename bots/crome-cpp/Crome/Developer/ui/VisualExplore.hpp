// Included after the UI's player/look state definitions.
#include "modules/explore/VisualCaptureWin.hpp"
#include <future>
#include <chrono>

namespace visual_ui {
using namespace crome::visual;
struct Recognition {std::string build;std::uint64_t epoch{};std::vector<Match> matches;};
struct Trainer {
    bool running{},loaded{},insertDown{};int calibration{},columns{15},rows{11},anchorCol{7},anchorRow{5},selected{};
    RECT area{};int clientWidth{},clientHeight{};DWORD pid{};
    std::uint64_t nextCapture{},stableSince{},nextSave{};
    std::string key,status="Calibre a area do mapa.",bankStatus;Image frame;
    Observation previous{},frameObservation{};bool havePrevious{},haveFramePosition{};
    std::vector<Reference> bank;std::vector<Match> matches;
    nlohmann::json records=nlohmann::json::object();
    GLuint texture{};char label[96]="",query[96]="Rope";int clientId{},kind{};
    std::future<std::string> download;
    std::future<Recognition> recognition;std::uint64_t frameEpoch{};
};
static Trainer state;
static std::filesystem::path folder(){return g_baseDir/"visual74"/state.key;}
static std::string locationKey(Location p){return std::to_string(p.x)+","+std::to_string(p.y)+","+std::to_string(p.z);}
static std::string buildKey(){
    const auto* p=g_session?g_session->profile():nullptr;if(!p)return {};
    if(p->build.textHash.empty())return {};
    std::uint64_t hash=14695981039346656037ull;
    for(unsigned char c:p->build.textHash){hash^=c;hash*=1099511628211ull;}
    return "74_"+std::to_string(p->build.timestamp)+"_"+std::to_string(p->build.sizeOfImage)+"_"+std::to_string(hash);
}
static void clearFrame(){++state.frameEpoch;state.frame={};state.matches.clear();state.haveFramePosition=false;}
static void save(){
    if(state.key.empty())return;
    std::error_code ec;std::filesystem::create_directories(folder()/"samples",ec);
    nlohmann::json data={{"schema","crome-visual74-v1"},{"build",state.key},{"area",{state.area.left,state.area.top,state.area.right,state.area.bottom}},
        {"clientWidth",state.clientWidth},{"clientHeight",state.clientHeight},{"columns",state.columns},{"rows",state.rows},
        {"anchorCol",state.anchorCol},{"anchorRow",state.anchorRow},{"observations",state.records}};
    const auto path=folder()/"learning.json",temp=folder()/"learning.tmp";
    std::ofstream file(temp);file<<data.dump(2);file.close();
    if(!file||!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))state.status="Falha ao salvar aprendizado.";
}
static void loadBank(){
    ++state.frameEpoch;state.matches.clear();state.bank.clear();unsigned failed=0;
    if(state.key.empty())return;
    std::error_code ec;std::filesystem::create_directories(folder()/"samples",ec);
    std::filesystem::create_directories(g_baseDir/"visual74"/"catalog",ec);
    for(const auto& dir:{folder()/"samples",g_baseDir/"visual74"/"catalog"}){
        for(const auto& entry:std::filesystem::directory_iterator(dir,ec)){
            if(state.bank.size()>=512)break;
            if(entry.path().extension()!=L".json")continue;
            try {
                std::ifstream file(entry.path());nlohmann::json j;file>>j;
                const auto name=j.value("image",std::string());
                if(name.empty()||std::filesystem::path(name).filename()!=std::filesystem::path(name))continue;
                auto image=loadImage(dir/std::filesystem::u8path(name));if(!image.valid()){++failed;continue;}
                state.bank.push_back({j.value("name",std::string("Referencia")),j.value("origin",std::string("local")),
                    j.value("category",std::string("unknown")),j.value("clientId",0u),std::move(image)});
            } catch(...){++failed;}
        }
    }
    state.bankStatus=std::to_string(state.bank.size())+" referencias; "+std::to_string(failed)+" indisponiveis.";
}
static void setBuild(){
    auto key=buildKey();if(key==state.key&&state.loaded)return;
    if(!state.key.empty())save();clearFrame();state.running=false;state.calibration=0;state.havePrevious=false;
    state.key=key;state.loaded=true;state.area={};state.records=nlohmann::json::object();
    state.clientWidth=state.clientHeight=0;
    if(key.empty()){state.status="Aguardando profile da build.";return;}
    try {
        std::ifstream file(folder()/"learning.json");nlohmann::json j;if(file){file>>j;
            if(j.value("build",std::string())==key){
                auto a=j.at("area");if(a.is_array()&&a.size()==4)state.area={a[0].get<LONG>(),a[1].get<LONG>(),a[2].get<LONG>(),a[3].get<LONG>()};
                state.clientWidth=j.value("clientWidth",0);state.clientHeight=j.value("clientHeight",0);
                state.columns=std::clamp(j.value("columns",15),3,31);state.rows=std::clamp(j.value("rows",11),3,31);
                state.anchorCol=std::clamp(j.value("anchorCol",7),0,state.columns-1);state.anchorRow=std::clamp(j.value("anchorRow",5),0,state.rows-1);
                if(j.contains("observations")&&j["observations"].is_object())state.records=j["observations"];
            }
        }
    }catch(...){state.status="Aprendizado invalido; calibre novamente.";}
    loadBank();
}
static void upload(){
    if(!state.frame.valid())return;
    if(!state.texture)glGenTextures(1,&state.texture);
    glBindTexture(GL_TEXTURE_2D,state.texture);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,state.frame.width,state.frame.height,0,GL_RGBA,GL_UNSIGNED_BYTE,state.frame.pixels.data());
}
static void record(const Observation& current){
    if(!state.havePrevious){state.previous=current;state.havePrevious=true;state.stableSince=current.time;return;}
    auto event=movementEvidence(state.previous,current);
    if(!(current.position==state.previous.position)){
        state.stableSince=current.time;
    }
        if(event!=Event::None&&state.records.size()<50000){
            const auto key=locationKey(current.position);auto& record=state.records[key];
            if(!record.is_object())record=nlohmann::json::object();
            if(!(current.position==state.previous.position))record["visits"]=record.value("visits",0)+1;
            if(event==Event::Transition){
                auto& source=state.records[locationKey(state.previous.position)];if(!source.is_object())source=nlohmann::json::object();
                source["transitionTo"]=key;source["transitionType"]="unknown";
            }
            if(event==Event::DamageCandidate){record["damageCandidate"]=state.previous.health-current.health;record["hazardType"]="unknown";}
            if(event==Event::StateChangeCandidate){record["newStateBitsCandidate"]=current.states&~state.previous.states;record["conditionType"]="unknown";}
            record["evidence"]="observed_movement_not_walkability_confirmation";
        }
    state.previous=current;
}
static std::string importCatalog(std::string name,std::filesystem::path dir){
    try {
        auto bytes=catalogGet(L"/api/v1/items/"+encodedName(name));auto j=nlohmann::json::parse(bytes);
        if(j.contains("data")&&j["data"].is_object())j=j["data"].get<nlohmann::json>();
        if(!j.is_object()||!j.contains("images")||!j["images"].is_array())return "Catalogo sem imagem para esse nome.";
        std::error_code ec;std::filesystem::create_directories(dir,ec);int count=0;
        for(const auto& asset:j["images"]){
            int id=asset.value("assetId",0);if(id<=0||count>=1)continue;
            auto image=catalogGet(L"/api/v1/assets/"+std::to_wstring(id));
            const auto file="wiki_"+std::to_string(id)+".img";
            std::ofstream output(dir/file,std::ios::binary);output.write(reinterpret_cast<const char*>(image.data()),image.size());output.close();
            nlohmann::json metadata={{"name",j.value("name",name)},{"image",file},{"origin","TibiaData/Wiki - versao visual nao confirmada 7.4"},
                {"category","unknown"},{"clientId",0},{"catalogId",j.value("id",0)},{"source","https://tibiadata.bytewizards.de/api/v1/items/"+name}};
            std::ofstream meta(dir/("wiki_"+std::to_string(id)+".json"));meta<<metadata.dump(2);++count;
        }
        return std::to_string(count)+" imagens baixadas. Confira a versao 7.4.";
    }catch(const std::exception& error){return error.what();}
}
static Recognition recognize(Image frame,int columns,int rows,std::vector<Reference> bank,std::string build,std::uint64_t epoch){
    Recognition result{std::move(build),epoch,{}};
    for(int row=0;row<rows;++row)for(int col=0;col<columns;++col)result.matches.push_back(match(cell(frame,columns,rows,col,row),bank));
    return result;
}
static void receiveRecognition(){
    if(!state.recognition.valid()||state.recognition.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)return;
    auto result=state.recognition.get();
    if(result.build!=state.key||result.epoch!=state.frameEpoch||result.matches.size()!=static_cast<std::size_t>(state.columns*state.rows)||!state.frame.valid())return;
    state.matches=std::move(result.matches);unsigned known=0;
    for(int index=0;index<int(state.matches.size());++index){
        auto m=state.matches[index];if(!m.accepted||m.index<0||m.index>=int(state.bank.size()))continue;++known;
        if(!state.running)continue;
        auto key=locationKey(worldCell(state.frameObservation.position,index%state.columns,index/state.columns,state.anchorCol,state.anchorRow));
        if(state.records.size()>=50000&&!state.records.contains(key))continue;
        auto& entry=state.records[key];if(!entry.is_object())entry=nlohmann::json::object();
        const auto& ref=state.bank[m.index];entry["visualSuggestion"]=ref.name;entry["visualConfidence"]=m.score;entry["origin"]=ref.origin;
        if(ref.clientId)entry["manualClientId"]=ref.clientId;
    }
    state.status=std::to_string(known)+" SQMs com referencia; demais desconhecidos.";
}
static void tick(){
    if(g_previewMode)return;
    setBuild();
    if(state.download.valid()&&state.download.wait_for(std::chrono::milliseconds(0))==std::future_status::ready){state.status=state.download.get();loadBank();}
    const DWORD pid=g_ramoniaPid.load()?g_ramoniaPid.load():g_session?g_session->status().pid:0;
    if(pid!=state.pid){state.pid=pid;state.havePrevious=false;state.running=false;state.calibration=0;clearFrame();}
    receiveRecognition();
    HWND target=clientWindow(pid);
    bool insert=(GetAsyncKeyState(VK_INSERT)&0x8000)!=0;
    if(state.calibration&&insert&&!state.insertDown&&target&&GetAncestor(GetForegroundWindow(),GA_ROOT)==target){
        POINT mouse{};GetCursorPos(&mouse);ScreenToClient(target,&mouse);RECT client{};GetClientRect(target,&client);
        if(mouse.x>=0&&mouse.y>=0&&mouse.x<client.right&&mouse.y<client.bottom){
            if(state.calibration==1){state.area.left=mouse.x;state.area.top=mouse.y;state.calibration=2;state.status="INSERT no canto inferior direito do MAPA.";}
            else{state.area.right=mouse.x;state.area.bottom=mouse.y;state.clientWidth=client.right;state.clientHeight=client.bottom;state.calibration=0;
                state.status="Area registrada. Confira grade e centro do player.";save();}
        }
    }
    state.insertDown=insert;
    if(!state.running)return;
    if(state.key.empty()||!g_livePlayer.valid){state.status="Sem posicao validada; aprendizado pausado.";state.havePrevious=false;clearFrame();return;}
    if(!target||IsIconic(target)||GetAncestor(GetForegroundWindow(),GA_ROOT)!=target){
        state.status=state.frame.valid()?"Captura anterior pausada; pode selecionar/aprender o SQM.":"Deixe o cliente em primeiro plano para capturar.";
        state.havePrevious=false;return;
    }
    const auto now=GetTickCount64();
    Observation current{{g_livePlayer.position.x,g_livePlayer.position.y,g_livePlayer.position.z},g_livePlayer.health,g_livePlayer.states,now,g_livePlayer.targetId!=0};
    record(current);
    if(now>=state.nextSave){state.nextSave=now+5000;save();}
    if(now<state.nextCapture)return;state.nextCapture=now+1000;
    if(state.recognition.valid())return;
    if(now-state.stableSince<600){state.status="Aguardando player parar para alinhar os SQMs.";clearFrame();return;}
    RECT client{};if(target)GetClientRect(target,&client);
    if(client.right!=state.clientWidth||client.bottom!=state.clientHeight){state.status="Janela mudou de tamanho; calibre novamente.";clearFrame();return;}
    auto image=capture(target,state.area,g_hWnd,state.status);
    if(!image.valid()){clearFrame();return;}
    ++state.frameEpoch;state.frame=std::move(image);state.frameObservation=current;state.haveFramePosition=true;upload();state.matches.clear();
    state.status="Comparando SQMs em segundo plano...";
    state.recognition=std::async(std::launch::async,recognize,state.frame,state.columns,state.rows,state.bank,state.key,state.frameEpoch);
}
static void draw(const Theme& theme){
    if(g_previewMode&&!state.frame.valid()){
        state.frame={480,352,{}};state.frame.pixels.resize(480*352);
        for(int y=0;y<352;++y)for(int x=0;x<480;++x){unsigned char shade=static_cast<unsigned char>(70+((x/8+y/8)%2)*25);state.frame.pixels[y*480+x]={shade,static_cast<unsigned char>(shade+15),shade,255};}
        state.status="Demonstracao da grade; nao e uma captura do cliente.";upload();
    }
    CardBegin("visual74","Aprendizado visual 7.4",CardIcon::Info,theme.blue,std::max(1.0f,ImGui::GetContentRegionAvail().y),theme);
    if(ImGui::Button("Calibrar mapa")){state.running=false;state.calibration=1;state.status="No jogo: INSERT no canto superior esquerdo do MAPA.";clearFrame();}
    ImGui::SameLine();if(ImGui::Button(state.running?"Parar":"Observar")){state.running=!state.running;state.havePrevious=false;if(!state.running)save();}
    ImGui::SameLine();if(ImGui::Button("Recarregar banco"))loadBank();
    ImGui::TextWrapped("%s",state.status.c_str());
    ImGui::TextDisabled("Captura normal, local. Sem envio de imagens e sem movimento.");
    const auto previousGrid=std::array<int,4>{state.columns,state.rows,state.anchorCol,state.anchorRow};
    ImGui::SetNextItemWidth(45);ImGui::InputInt("Cols",&state.columns,0);ImGui::SameLine();ImGui::SetNextItemWidth(45);ImGui::InputInt("Linhas",&state.rows,0);
    ImGui::SameLine();ImGui::SetNextItemWidth(40);ImGui::InputInt("Player C",&state.anchorCol,0);ImGui::SameLine();ImGui::SetNextItemWidth(40);ImGui::InputInt("L",&state.anchorRow,0);
    state.columns=std::clamp(state.columns,3,31);state.rows=std::clamp(state.rows,3,31);
    state.anchorCol=std::clamp(state.anchorCol,0,state.columns-1);state.anchorRow=std::clamp(state.anchorRow,0,state.rows-1);
    if(previousGrid!=std::array<int,4>{state.columns,state.rows,state.anchorCol,state.anchorRow}){clearFrame();state.havePrevious=false;save();}
    if(state.frame.valid()&&state.texture){
        float width=std::min(480.0f,ImGui::GetContentRegionAvail().x),height=std::min(170.0f,width*state.frame.height/state.frame.width);
        width=height*state.frame.width/state.frame.height;ImVec2 pos=ImGui::GetCursorScreenPos();
        ImGui::Image(static_cast<ImTextureID>(state.texture),ImVec2(width,height));
        if(ImGui::IsItemClicked()){auto mouse=ImGui::GetMousePos();int col=std::clamp(int((mouse.x-pos.x)/width*state.columns),0,state.columns-1);
            int row=std::clamp(int((mouse.y-pos.y)/height*state.rows),0,state.rows-1);state.selected=row*state.columns+col;}
        state.selected=std::clamp(state.selected,0,state.columns*state.rows-1);
        int col=state.selected%state.columns,row=state.selected/state.columns;auto* draw=ImGui::GetWindowDrawList();
        for(int x=1;x<state.columns;++x)draw->AddLine({pos.x+width*x/state.columns,pos.y},{pos.x+width*x/state.columns,pos.y+height},IM_COL32(255,255,255,70));
        for(int y=1;y<state.rows;++y)draw->AddLine({pos.x,pos.y+height*y/state.rows},{pos.x+width,pos.y+height*y/state.rows},IM_COL32(255,255,255,70));
        draw->AddRect({pos.x+width*col/state.columns,pos.y+height*row/state.rows},{pos.x+width*(col+1)/state.columns,pos.y+height*(row+1)/state.rows},IM_COL32(255,200,0,255));
        ImGui::Text("SQM C%d L%d",col,row);
        if(state.haveFramePosition){auto p=worldCell(state.frameObservation.position,col,row,state.anchorCol,state.anchorRow);ImGui::SameLine();ImGui::Text("%d,%d,%d",p.x,p.y,p.z);}
        if(state.haveFramePosition){
            auto p=worldCell(state.frameObservation.position,col,row,state.anchorCol,state.anchorRow);
            const bool ready=g_nativeRuntimeLoaded||g_ramoniaBridgeReady.load();
            ImGui::BeginDisabled(!ready||g_previewMode);
            if(ImGui::SmallButton("Ler IDs do Tile"))RequestInspect(p.x,p.y,p.z);
            ImGui::EndDisabled();
            if(g_inspect.valid&&g_inspect.x==p.x&&g_inspect.y==p.y&&g_inspect.z==p.z){
                ImGui::SameLine();ImGui::TextUnformatted("IDs:");
                for(int id:g_inspect.ids){ImGui::SameLine();ImGui::PushID(id);if(ImGui::SmallButton(std::to_string(id).c_str()))state.clientId=id;ImGui::PopID();}
            }
        }
        if(state.selected<int(state.matches.size())){auto m=state.matches[state.selected];if(m.index>=0&&m.index<int(state.bank.size()))ImGui::TextWrapped("%s: %s (%.0f%%)",m.accepted?"Sugestao":"Incerto",state.bank[m.index].name.c_str(),m.score*100);}
    }else ImGui::TextDisabled("Sem captura valida. Nenhum SQM sera identificado por imagem.");
    ImGui::SetNextItemWidth(145);ImGui::InputText("Nome",state.label,sizeof(state.label));ImGui::SameLine();ImGui::SetNextItemWidth(70);ImGui::InputInt("ID cliente",&state.clientId,0);
    ImGui::SameLine();if(ImGui::SmallButton("ID do Look")&&g_lastLook.valid)state.clientId=g_lastLook.id;
    const char* kinds[]={"Desconhecido","Passagem","Perigo","Transicao","Bloqueado"};ImGui::SetNextItemWidth(110);ImGui::Combo("Tipo manual",&state.kind,kinds,5);
    ImGui::SameLine();
    if(ImGui::Button("Aprender SQM selecionado")&&state.frame.valid()&&!state.key.empty()&&state.label[0]){
        auto sample=normalized(cell(state.frame,state.columns,state.rows,state.selected%state.columns,state.selected/state.columns));
        const auto stem="sample_"+std::to_string(GetTickCount64());std::error_code ec;std::filesystem::create_directories(folder()/"samples",ec);
        if(saveBmp(folder()/"samples"/(stem+".bmp"),sample)){
            nlohmann::json metadata={{"name",std::string(state.label)},{"image",stem+".bmp"},{"origin","Amostra manual desta build 7.4"},
                {"category",kinds[state.kind]},{"clientId",std::clamp(state.clientId,0,65535)},{"mappingEvidence","user_annotation_not_runtime_validation"}};
            std::ofstream file(folder()/"samples"/(stem+".json"));file<<metadata.dump(2);file.close();loadBank();state.status="Amostra salva; classificacao anotada por voce.";
        }else state.status="Falha ao salvar amostra.";
    }
    ImGui::TextWrapped("%s | %zu coordenadas observadas",state.bankStatus.c_str(),state.records.size());
    ImGui::Separator();ImGui::SetNextItemWidth(160);ImGui::InputText("Nome Wiki",state.query,sizeof(state.query));ImGui::SameLine();
    bool busy=state.download.valid();ImGui::BeginDisabled(busy||g_previewMode);
    if(ImGui::Button("Buscar imagem")&&state.query[0]){state.status="Consultando TibiaData...";state.download=std::async(std::launch::async,importCatalog,std::string(state.query),g_baseDir/"visual74"/"catalog");}
    ImGui::EndDisabled();ImGui::TextWrapped("Wiki: referencia; sprite 7.4 e ID do OT precisam de conferencia. Dano nao identifica fogo/energy sozinho.");
    CardEnd();
}
}
