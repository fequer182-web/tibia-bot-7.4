#include "ui_shared.hpp"
#include <commctrl.h>
#include <algorithm>
#include <vector>
#include <array>
#include <cwchar>
namespace {
UiBridge bridge;HWND panel=nullptr,parentWindow=nullptr,tabs=nullptr;
HFONT font=nullptr,titleFont=nullptr;HBRUSH background=nullptr,editBrush=nullptr;
std::array<std::vector<HWND>,4> pages;
HWND statusLine,detailLine,hpEdit,cooldownEdit,spellEdit,destEdit,waypointList;
std::array<HWND,4> switches{};bool initialized=false,selfTest=false,preview=false;
uint32_t shownFlags=~uint32_t(0),shownWaypointCount=~uint32_t(0),shownIndex=~uint32_t(0);
std::array<Waypoint,64> shownWaypoints{};int currentPage=0;DWORD started=0;
int scrollY=0;
constexpr int TabId=100,StopId=101,StopTopId=102,AttackId=110,LootId=111,CaveId=112,HealId=113,
    ApplyId=120,AddId=121,ClearId=122,RemoveId=123,LoopId=124;
HWND Control(const wchar_t* cls,const wchar_t* text,int x,int y,int w,int h,int id,DWORD style=0,int page=-1){
    HWND control=CreateWindowExW(cls==std::wstring(L"EDIT")?WS_EX_CLIENTEDGE:0,cls,text,
        WS_CHILD|WS_VISIBLE|style,x,y,w,h,panel,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
    if(control)SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
    if(page>=0)pages[page].push_back(control);
    return control;
}
void Label(const wchar_t* text,int y,int page,int height=30){Control(L"STATIC",text,22,y,318,height,0,SS_LEFT,page);}
HWND Edit(const wchar_t* value,int y,int id,int page,int width=310){
    return Control(L"EDIT",value,22,y,width,25,id,ES_AUTOHSCROLL|WS_TABSTOP,page);
}
std::wstring GetText(HWND h){wchar_t value[256]{};GetWindowTextW(h,value,256);return value;}
void SetStatus(const wchar_t* value){bridge.access([&](UiState& s){Text(s.status,value);});}
void Page(int index){currentPage=index;for(int i=0;i<4;++i)for(auto h:pages[i])ShowWindow(h,i==index?SW_SHOW:SW_HIDE);}
bool Apply(){
    auto number=[](HWND h,unsigned low,unsigned high,unsigned& out){
        auto value=GetText(h);wchar_t* end=nullptr;auto n=wcstoul(value.c_str(),&end,10);
        if(value.empty()||*end||n<low||n>high)return false;
        out=unsigned(n);return true;
    };
    unsigned hp=0,interval=0,dest=0;
    if(!number(hpEdit,1,1000000,hp)||!number(cooldownEdit,1000,60000,interval)||!number(destEdit,0,63,dest)){
        MessageBoxW(panel,L"Confira HP (1 a 1000000), intervalo (1000 a 60000 ms) e bag (0 a 63).",L"Configuracao",MB_OK);return false;}
    auto spell=GetText(spellEdit);
    if(spell.size()>15||std::any_of(spell.begin(),spell.end(),[](wchar_t c){return c<32||c>126;})){
        MessageBoxW(panel,L"A fala aceita de 1 a 15 caracteres ASCII. Deixe vazia para configurar depois.",L"Healing",MB_OK);return false;}
    bridge.access([&](UiState& s){s.hpThreshold=hp;s.healCooldownMs=interval;s.destination=dest;Text(s.healText,spell);});
    return true;
}
void Toggle(unsigned bit){
    if(!Apply())return;
    UiState copy{};bridge.access([&](UiState& s){copy=s;});
    if(!(copy.flags&bit)){
        if(bit==Heal&&copy.healText[0]==0){MessageBoxW(panel,L"Informe a magia na aba Healing antes de ativar.",L"Healing",MB_OK);return;}
        if(bit==Cave&&copy.waypointCount==0){MessageBoxW(panel,L"Adicione pelo menos um waypoint.",L"Cavebot",MB_OK);return;}
        if(GetTickCount()-copy.heartbeat>10000){MessageBoxW(panel,L"Controlador desconectado. Reinicie iniciar_ui.py.",L"Cromin",MB_OK);return;}
    }
    bridge.access([&](UiState& s){s.flags^=bit;});
}
void Refresh(){
    UiState s{};if(!bridge.access([&](UiState& state){
        if(preview)state.heartbeat=GetTickCount();
        if(GetTickCount()-state.heartbeat>10000){state.flags&=~uint32_t(15);Text(state.status,L"Controlador desconectado");}
        s=state;
    }))return;
    SetWindowTextW(statusLine,s.status);SetWindowTextW(detailLine,s.detail);
    if(!initialized){
        SetWindowTextW(hpEdit,std::to_wstring(s.hpThreshold).c_str());
        SetWindowTextW(cooldownEdit,std::to_wstring(s.healCooldownMs).c_str());
        SetWindowTextW(destEdit,std::to_wstring(s.destination).c_str());SetWindowTextW(spellEdit,s.healText);initialized=true;
    }
    if(shownFlags!=s.flags){
        for(int i=0;i<4;++i)SendMessageW(switches[i],BM_SETCHECK,(s.flags&(1u<<i))?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(GetDlgItem(panel,LoopId),BM_SETCHECK,s.flags&Loop?BST_CHECKED:BST_UNCHECKED,0);shownFlags=s.flags;
    }
    if(s.waypointCount<=64&&(shownWaypointCount!=s.waypointCount||shownIndex!=s.waypointIndex||memcmp(shownWaypoints.data(),s.waypoints,sizeof(s.waypoints)))){
        auto selected=SendMessageW(waypointList,LB_GETCURSEL,0,0);SendMessageW(waypointList,LB_RESETCONTENT,0,0);
        for(unsigned i=0;i<s.waypointCount;++i){auto p=s.waypoints[i];
            std::wstring text=(i==s.waypointIndex?L"> ":L"   ")+std::to_wstring(i+1)+L"   "+std::to_wstring(p.x)+L", "+std::to_wstring(p.y)+L", "+std::to_wstring(p.z);
            SendMessageW(waypointList,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));}
        if(selected>=0&&unsigned(selected)<s.waypointCount)SendMessageW(waypointList,LB_SETCURSEL,selected,0);
        shownWaypointCount=s.waypointCount;shownIndex=s.waypointIndex;memcpy(shownWaypoints.data(),s.waypoints,sizeof(s.waypoints));
    }
    RECT area{};if(GetClientRect(parentWindow,&area)){
        int height=std::max(160,std::min(610,int(area.bottom)-16));
        SetWindowPos(panel,HWND_TOP,std::max(0,int(area.right)-390),8,380,height,SWP_NOACTIVATE);
    }
}
LRESULT CALLBACK PanelProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_SIZE:{
        int next=std::min(scrollY,std::max(0,590-int(HIWORD(lp))));
        if(next!=scrollY){ScrollWindowEx(hwnd,0,scrollY-next,nullptr,nullptr,nullptr,nullptr,SW_SCROLLCHILDREN|SW_INVALIDATE|SW_ERASE);scrollY=next;}
        SCROLLINFO info{};info.cbSize=sizeof(info);info.fMask=SIF_RANGE|SIF_PAGE|SIF_POS;
        info.nMax=589;info.nPage=HIWORD(lp);info.nPos=scrollY;SetScrollInfo(hwnd,SB_VERT,&info,TRUE);return 0;}
    case WM_MOUSEWHEEL:
        SendMessageW(hwnd,WM_VSCROLL,GET_WHEEL_DELTA_WPARAM(wp)>0?SB_LINEUP:SB_LINEDOWN,0);return 0;
    case WM_VSCROLL:{
        SCROLLINFO info{};info.cbSize=sizeof(info);info.fMask=SIF_ALL;GetScrollInfo(hwnd,SB_VERT,&info);
        int next=scrollY;
        switch(LOWORD(wp)){case SB_LINEUP:next-=28;break;case SB_LINEDOWN:next+=28;break;
            case SB_PAGEUP:next-=int(info.nPage);break;case SB_PAGEDOWN:next+=int(info.nPage);break;
            case SB_THUMBTRACK:next=info.nTrackPos;break;default:break;}
        next=std::max(0,std::min(next,590-int(info.nPage)));
        ScrollWindowEx(hwnd,0,scrollY-next,nullptr,nullptr,nullptr,nullptr,SW_SCROLLCHILDREN|SW_INVALIDATE|SW_ERASE);
        scrollY=next;SetScrollPos(hwnd,SB_VERT,scrollY,TRUE);return 0;}
    case WM_CREATE:{
        panel=hwnd;font=CreateFontW(-15,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        titleFont=CreateFontW(-23,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        auto title=Control(L"STATIC",L"CROMIN",18,12,200,30,0);SendMessageW(title,WM_SETFONT,reinterpret_cast<WPARAM>(titleFont),TRUE);
        Control(L"BUTTON",L"PARAR",192,14,80,26,StopTopId,WS_TABSTOP);
        Control(L"BUTTON",L"Fechar",280,14,64,26,IDCANCEL,WS_TABSTOP);
        statusLine=Control(L"STATIC",L"Conectando...",18,48,326,23,0);
        detailLine=Control(L"STATIC",L"Todas as automacoes iniciam desligadas.",18,73,326,40,0);
        tabs=Control(WC_TABCONTROLW,L"",14,118,332,27,TabId,TCS_BUTTONS|TCS_FLATBUTTONS);
        const wchar_t* names[]={L"Ataque",L"Loot",L"Cavebot",L"Healing"};
        for(int i=0;i<4;++i){TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=const_cast<wchar_t*>(names[i]);TabCtrl_InsertItem(tabs,i,&item);}
        switches[0]=Control(L"BUTTON",L"Ativar auto ataque",22,161,304,30,AttackId,BS_AUTOCHECKBOX|WS_TABSTOP,0);
        Label(L"Alvo: Rat",207,0);Label(L"Escolhe o mais proximo no mesmo andar.",240,0,42);
        Label(L"Mantem o mesmo alvo durante a perseguicao.",286,0,44);
        Label(L"Comece sem outro ataque ou follow ativo.",336,0,44);
        switches[1]=Control(L"BUTTON",L"Ativar auto loot",22,161,304,30,LootId,BS_AUTOCHECKBOX|WS_TABSTOP,1);
        Label(L"Recolher: moedas (ID 3031)",207,1);Label(L"Bag de destino: numero da janela (0 a 63)",245,1);
        destEdit=Edit(L"0",277,202,1,90);Label(L"Abra a bag 2853 antes de ativar o loot.",320,1,45);
        Label(L"Confere o corpo e a quantidade na bag.",371,1,44);
        switches[2]=Control(L"BUTTON",L"Ativar cavebot",22,161,304,30,CaveId,BS_AUTOCHECKBOX|WS_TABSTOP,2);
        Control(L"BUTTON",L"Repetir rota",22,195,304,26,LoopId,BS_AUTOCHECKBOX|WS_TABSTOP,2);
        waypointList=Control(L"LISTBOX",L"",22,232,310,172,203,LBS_NOTIFY|WS_VSCROLL|WS_BORDER|WS_TABSTOP,2);
        Control(L"BUTTON",L"Adicionar posicao atual",22,414,310,28,AddId,WS_TABSTOP,2);
        Control(L"BUTTON",L"Remover",22,449,148,27,RemoveId,WS_TABSTOP,2);
        Control(L"BUTTON",L"Limpar rota",180,449,152,27,ClearId,WS_TABSTOP,2);
        switches[3]=Control(L"BUTTON",L"Ativar healing por magia",22,161,310,30,HealId,BS_AUTOCHECKBOX|WS_TABSTOP,3);
        Label(L"Usar magia quando HP atual for menor ou igual a",204,3,42);hpEdit=Edit(L"80",248,204,3,90);
        Label(L"HP absoluto (nao porcentagem).",280,3);Label(L"Texto da magia",318,3);spellEdit=Edit(L"",347,205,3);
        SendMessageW(spellEdit,EM_SETLIMITTEXT,15,0);Label(L"Intervalo minimo entre tentativas (ms)",386,3);cooldownEdit=Edit(L"2000",418,206,3,110);
        Control(L"BUTTON",L"Aplicar configuracao",18,497,326,30,ApplyId,WS_TABSTOP);
        Control(L"BUTTON",L"PARAR AUTOMACOES",18,535,326,32,StopId,WS_TABSTOP|BS_DEFPUSHBUTTON);
        Page(0);SetTimer(hwnd,1,200,nullptr);started=GetTickCount();
        bridge.access([&](UiState& s){s.uiReady=1;s.uiClosed=0;});
        return 0;
    }
    case WM_COMMAND:
        switch(LOWORD(wp)){
        case AttackId:Toggle(Attack);break;case LootId:Toggle(Loot);break;case CaveId:Toggle(Cave);break;case HealId:Toggle(Heal);break;
        case LoopId:bridge.access([](UiState& s){s.flags^=Loop;});break;
        case ApplyId:if(Apply())SetStatus(L"Configuracao aplicada");break;
        case StopId:case StopTopId:bridge.access([](UiState& s){s.flags&=~uint32_t(15);Text(s.status,L"Parada solicitada; aguardando chamada atual");});break;
        case AddId:bridge.access([](UiState& s){++s.addSerial;});break;
        case ClearId:bridge.access([](UiState& s){++s.clearSerial;});break;
        case RemoveId:{auto index=SendMessageW(waypointList,LB_GETCURSEL,0,0);if(index!=LB_ERR)bridge.access([&](UiState& s){s.removeIndex=uint32_t(index);++s.removeSerial;});break;}
        case IDCANCEL:SendMessageW(hwnd,WM_CLOSE,0,0);break;
        }return 0;
    case WM_NOTIFY:if(reinterpret_cast<NMHDR*>(lp)->idFrom==TabId&&reinterpret_cast<NMHDR*>(lp)->code==TCN_SELCHANGE){Page(TabCtrl_GetCurSel(tabs));return 0;}break;
    case WM_TIMER:
        Refresh();
        if(selfTest&&GetTickCount()-started>700){
            bool ok=GetParent(hwnd)==parentWindow&&GetDlgItem(hwnd,StopId)&&waypointList&&hpEdit&&spellEdit;
            bridge.access([&](UiState& s){s.uiReady=ok?2:99;});SendMessageW(hwnd,WM_CLOSE,0,0);
        }return 0;
    case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:case WM_CTLCOLORLISTBOX:case WM_CTLCOLOREDIT:{
        HDC dc=reinterpret_cast<HDC>(wp);SetTextColor(dc,RGB(229,232,239));
        COLORREF color=msg==WM_CTLCOLOREDIT||msg==WM_CTLCOLORLISTBOX?RGB(37,43,55):RGB(22,27,36);
        SetBkColor(dc,color);return reinterpret_cast<LRESULT>(msg==WM_CTLCOLOREDIT||msg==WM_CTLCOLORLISTBOX?editBrush:background);}
    case WM_CLOSE:bridge.access([](UiState& s){s.flags&=~uint32_t(15);s.uiClosed=1;});DestroyWindow(hwnd);return 0;
    case WM_DESTROY:bridge.access([](UiState& s){s.flags&=~uint32_t(15);s.uiClosed=1;});KillTimer(hwnd,1);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
BOOL CALLBACK FindParent(HWND hwnd,LPARAM){DWORD pid=0;GetWindowThreadProcessId(hwnd,&pid);
    if(pid==GetCurrentProcessId()&&IsWindowVisible(hwnd)&&GetWindow(hwnd,GW_OWNER)==nullptr){RECT r{};GetClientRect(hwnd,&r);
        RECT current{};if(parentWindow)GetClientRect(parentWindow,&current);
        if(r.right>=400&&r.bottom>=200&&r.right*r.bottom>current.right*current.bottom)parentWindow=hwnd;}return TRUE;}
DWORD WINAPI PanelThread(void*){
    if(!bridge.open())return 1;
    if(!parentWindow)EnumWindows(FindParent,0);
    if(!parentWindow)return 2;
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_TAB_CLASSES};InitCommonControlsEx(&controls);
    background=CreateSolidBrush(RGB(22,27,36));editBrush=CreateSolidBrush(RGB(37,43,55));
    WNDCLASSEXW cls{};cls.cbSize=sizeof(cls);cls.lpfnWndProc=PanelProc;cls.hInstance=GetModuleHandleW(nullptr);
    cls.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));cls.hbrBackground=background;cls.lpszClassName=L"CrominEmbeddedPanelV1";
    if(!RegisterClassExW(&cls))return 3;
    panel=CreateWindowExW(WS_EX_CONTROLPARENT|WS_EX_CLIENTEDGE,cls.lpszClassName,L"Cromin",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_CLIPSIBLINGS|WS_VSCROLL,
                         12,8,380,610,parentWindow,nullptr,cls.hInstance,nullptr);
    if(!panel){UnregisterClassW(cls.lpszClassName,cls.hInstance);return 4;}
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){
        if(!IsDialogMessageW(panel,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
    if(font)DeleteObject(font);
    if(titleFont)DeleteObject(titleFont);
    UnregisterClassW(cls.lpszClassName,cls.hInstance);DeleteObject(background);DeleteObject(editBrush);return 0;
}
}
#ifdef CROMIN_UI_PREVIEW
#include <iostream>
int main(int argc,char** argv){
    preview=true;selfTest=argc==2&&std::string(argv[1])=="--self-test";
    HANDLE mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(UiState),MappingName().c_str());
    if(!mapping)return 1;
    auto data=static_cast<UiState*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(UiState)));
    if(!data)return 2;
    *data={};data->magic=UiMagic;data->version=UiVersion;data->bytes=sizeof(UiState);
    data->heartbeat=GetTickCount();data->hpThreshold=80;data->healCooldownMs=2000;Text(data->status,L"Preview: nenhuma acao no jogo");
    parentWindow=CreateWindowExW(0,L"STATIC",L"Cromin - preview dentro de uma janela de teste",WS_OVERLAPPEDWINDOW|(selfTest?0:WS_VISIBLE),
        CW_USEDEFAULT,CW_USEDEFAULT,980,700,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    auto code=PanelThread(nullptr);bool ok=!code&&data->uiClosed&&(selfTest?data->uiReady==2:true);
    std::cout<<(ok?"PASS":"FAIL")<<": painel filho, abas, controles e IPC "<<sizeof(UiState)<<" bytes\n";
    DestroyWindow(parentWindow);UnmapViewOfFile(data);CloseHandle(mapping);return ok?0:1;
}
#else
DWORD WINAPI Entry(void* module){PanelThread(nullptr);FreeLibraryAndExitThread(static_cast<HMODULE>(module),0);return 0;}
BOOL APIENTRY DllMain(HMODULE module,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){DisableThreadLibraryCalls(module);HANDLE thread=CreateThread(nullptr,0,Entry,module,0,nullptr);if(thread)CloseHandle(thread);else return FALSE;}
    return TRUE;
}
#endif
