#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Somente este byte candidato e alterado. Nao ha hooks, janela ou rotinas do bot.
static const uintptr_t PlayerSlotRva=0xE52A48,PlayerVtableRva=0xAE99E0,LightOffset=0xF1;
static HANDLE logFile=INVALID_HANDLE_VALUE;
static_assert(sizeof(void*)==8,"Compile para Windows x64");

static bool Read(uintptr_t address,void* buffer,SIZE_T size){
    SIZE_T got=0;return ReadProcessMemory(GetCurrentProcess(),(const void*)address,buffer,size,&got)&&got==size;
}
static void Log(const char* message){
    if(logFile==INVALID_HANDLE_VALUE)return;
    DWORD written=0;WriteFile(logFile,message,(DWORD)strlen(message),&written,NULL);
    WriteFile(logFile,"\r\n",2,&written,NULL);FlushFileBuffers(logFile);
}
static void LogValue(const char* event,uintptr_t address,unsigned value){
    char line[180];snprintf(line,sizeof(line),"%s address=0x%llX value=%u",event,(unsigned long long)address,value);Log(line);
}
static bool BuildMatches(uintptr_t base){
    IMAGE_DOS_HEADER dos={};IMAGE_NT_HEADERS64 nt={};
    if(!Read(base,&dos,sizeof(dos))||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<64||dos.e_lfanew>0x100000)return false;
    if(!Read(base+dos.e_lfanew,&nt,sizeof(nt))||nt.Signature!=IMAGE_NT_SIGNATURE||nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||
       nt.FileHeader.TimeDateStamp!=0x6AD91836||nt.OptionalHeader.SizeOfImage!=0xEDE000||nt.OptionalHeader.AddressOfEntryPoint!=0x536BD0||
       nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC||!nt.FileHeader.NumberOfSections||nt.FileHeader.NumberOfSections>96)return false;
    uintptr_t sections=base+dos.e_lfanew+FIELD_OFFSET(IMAGE_NT_HEADERS64,OptionalHeader)+nt.FileHeader.SizeOfOptionalHeader;
    for(unsigned index=0;index<nt.FileHeader.NumberOfSections;++index){
        IMAGE_SECTION_HEADER section={};if(!Read(sections+index*sizeof(section),&section,sizeof(section)))return false;
        if(memcmp(section.Name,".text\0",6))continue;
        DWORD size=section.Misc.VirtualSize>section.SizeOfRawData?section.Misc.VirtualSize:section.SizeOfRawData;
        if(section.VirtualAddress!=0x1000||size!=0x95B400||section.VirtualAddress+size>nt.OptionalHeader.SizeOfImage)return false;
        uint64_t hash=0xCBF29CE484222325ULL;BYTE buffer[4096];
        for(DWORD offset=0;offset<size;){DWORD amount=size-offset;if(amount>sizeof(buffer))amount=sizeof(buffer);
            if(!Read(base+section.VirtualAddress+offset,buffer,amount))return false;
            for(DWORD i=0;i<amount;++i){hash^=buffer[i];hash*=0x100000001B3ULL;}offset+=amount;}
        return hash==0xBF68517610291E3AULL;
    }
    return false;
}
struct Sample {uintptr_t player,address;BYTE value;};
static bool Resolve(uintptr_t base,Sample* sample){
    uintptr_t player=0,vtable=0;DWORD x=0,y=0;BYTE z=0,value=0;
    if(!Read(base+PlayerSlotRva,&player,sizeof(player))||!player||!Read(player,&vtable,sizeof(vtable))||vtable!=base+PlayerVtableRva||
       !Read(player+0x30,&x,4)||!Read(player+0x34,&y,4)||!Read(player+0x38,&z,1)||
       !x||x>=65535||!y||y>=65535||z>15||!Read(player+LightOffset,&value,1)||value>31)return false;
    sample->player=player;sample->address=player+LightOffset;sample->value=value;return true;
}
static bool WriteByte(uintptr_t address,BYTE value){
    if(value>31)return false;
    MEMORY_BASIC_INFORMATION region={};
    if(!VirtualQuery((void*)address,&region,sizeof(region))||region.State!=MEM_COMMIT||(region.Protect&PAGE_GUARD)||
       ((region.Protect&255)!=PAGE_READWRITE&&(region.Protect&255)!=PAGE_WRITECOPY))return false;
    SIZE_T written=0;BYTE readback=255;
    return WriteProcessMemory(GetCurrentProcess(),(void*)address,&value,1,&written)&&written==1&&Read(address,&readback,1)&&readback==value;
}
struct LightSession {
    uintptr_t base,player,address;BYTE baseline,lastWritten,desired;bool enabled;
    explicit LightSession(uintptr_t image):base(image),player(0),address(0),baseline(0),lastWritten(0),desired(7),enabled(false){}
    bool Enable(){
        Sample sample={};if(enabled)return true;
        if(!Resolve(base,&sample)){Log("PLAYER_OR_LIGHT_UNAVAILABLE: nenhuma escrita");return false;}
        player=sample.player;address=sample.address;baseline=sample.value;lastWritten=sample.value;enabled=true;
        LogValue("ENABLE_CANDIDATE",address,desired);return Apply();
    }
    bool Apply(){
        if(!enabled)return false;
        Sample sample={};
        if(!Resolve(base,&sample)||sample.player!=player||sample.address!=address){
            Log("PLAYER_CHANGED_OR_UNAVAILABLE: desativado sem escrever no objeto antigo");enabled=false;return false;}
        if(sample.value!=lastWritten)baseline=sample.value;
        if(sample.value==desired)return true;
        if(!WriteByte(address,desired)){Log("WRITE_NOT_CONFIRMED: desativado");enabled=false;return false;}
        lastWritten=desired;LogValue("LIGHT_BYTE_WRITTEN",address,desired);return true;
    }
    bool Disable(){
        if(!enabled)return true;
        enabled=false;Sample sample={};
        if(!Resolve(base,&sample)||sample.player!=player||sample.address!=address){Log("RESTORE_SKIPPED: jogador mudou");return false;}
        if(sample.value!=lastWritten){Log("RESTORE_SKIPPED: valor alterado pelo cliente");return true;}
        bool restored=WriteByte(address,baseline);
        LogValue(restored?"RESTORED":"RESTORE_FAILED",address,baseline);return restored;
    }
};

#ifdef MINI_LIGHT_TEST
int main(){
    // Memoria sintetica propria; este executavel nao abre nem injeta no Antica.
    BYTE* image=(BYTE*)VirtualAlloc(NULL,0xEDE000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    BYTE* object=(BYTE*)VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!image||!object)return 2;
    uintptr_t base=(uintptr_t)image,player=(uintptr_t)object,vtable=base+PlayerVtableRva;
    memcpy(image+PlayerSlotRva,&player,8);memcpy(object,&vtable,8);DWORD x=100,y=100;
    memcpy(object+0x30,&x,4);memcpy(object+0x34,&y,4);object[0x38]=7;object[LightOffset]=3;
    LightSession light(base);bool ok=!BuildMatches(base)&&!light.enabled&&light.Enable()&&object[LightOffset]==7;
    light.desired=10;ok=ok&&light.Apply()&&object[LightOffset]==10&&light.Disable()&&object[LightOffset]==3;
    object[LightOffset]=4;ok=ok&&light.Enable();object[LightOffset]=6;
    ok=ok&&light.Apply()&&light.Disable()&&object[LightOffset]==6;
    ok=ok&&light.Enable();uintptr_t zero=0;memcpy(image+PlayerSlotRva,&zero,8);
    ok=ok&&!light.Apply()&&!light.enabled&&object[LightOffset]==10;
    memcpy(image+PlayerSlotRva,&player,8);object[LightOffset]=255;ok=ok&&!light.Enable();
    object[LightOffset]=5;memcpy(object,&zero,8);ok=ok&&!light.Enable();
    memcpy(object,&vtable,8);object[0x38]=255;ok=ok&&!light.Enable();
    object[0x38]=7;DWORD protection=0;
    VirtualProtect(object,4096,PAGE_READONLY,&protection);ok=ok&&!WriteByte(player+LightOffset,9);
    VirtualProtect(object,4096,protection,&protection);
    VirtualFree(object,0,MEM_RELEASE);VirtualFree(image,0,MEM_RELEASE);
    printf("%s: build, byte unico, restauracao, atualizacao do cliente, jogador trocado, VTable, faixa, andar e pagina somente leitura\n",ok?"PASS":"FAIL");
    return ok?0:1;
}
#else
static bool Down(int key){return (GetAsyncKeyState(key)&0x8000)!=0;}
static DWORD WINAPI Run(void* parameter){
    HMODULE module=(HMODULE)parameter;wchar_t path[32768]={};
    DWORD length=GetModuleFileNameW(module,path,32768);
    if(length&&length<32730){wchar_t* slash=wcsrchr(path,L'\\');if(slash)wcscpy(slash+1,L"Cromin_Luz.log");
        logFile=CreateFileW(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);}
    Log("START: Mini Luz x64; inicia DESLIGADA; F6 ativa/desativa, F7 diminui, F8 aumenta, F9 restaura e descarrega");
    uintptr_t base=(uintptr_t)GetModuleHandleW(NULL);
    if(!BuildMatches(base)){Log("BUILD_REJECTED: nenhuma escrita; descarregando");}
    else{
        LightSession light(base);Log("BUILD_OK: offset candidato +0xF1; escrita/efeito visual ainda requerem teste no Antica");
        const int keys[4]={VK_F6,VK_F7,VK_F8,VK_F9};bool previous[4]={};
        for(int i=0;i<4;++i)previous[i]=Down(keys[i]);
        bool stop=false;
        while(!stop){
            DWORD focusedPid=0;GetWindowThreadProcessId(GetForegroundWindow(),&focusedPid);
            for(int i=0;i<4;++i){bool pressed=Down(keys[i]);
                if(pressed&&!previous[i]&&focusedPid==GetCurrentProcessId()){
                    if(i==0){if(light.enabled)light.Disable();else light.Enable();}
                    else if(i==1&&light.desired>0){--light.desired;LogValue("SELECTED",0,light.desired);if(light.enabled)light.Apply();}
                    else if(i==2&&light.desired<31){++light.desired;LogValue("SELECTED",0,light.desired);if(light.enabled)light.Apply();}
                    else if(i==3)stop=true;
                }previous[i]=pressed;
            }
            if(light.enabled&&!stop)light.Apply();
            Sleep(100);
        }
        light.Disable();Log("EXIT_REQUESTED");
    }
    if(logFile!=INVALID_HANDLE_VALUE){CloseHandle(logFile);logFile=INVALID_HANDLE_VALUE;}
    FreeLibraryAndExitThread(module,0);return 0;
}
BOOL APIENTRY DllMain(HMODULE module,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){DisableThreadLibraryCalls(module);
        HANDLE thread=CreateThread(NULL,0,Run,module,0,NULL);if(!thread)return FALSE;CloseHandle(thread);}
    return TRUE;
}
#endif
