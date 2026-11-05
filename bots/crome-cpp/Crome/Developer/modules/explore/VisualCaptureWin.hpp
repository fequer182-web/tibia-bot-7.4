#pragma once
#include "VisualTiles.hpp"
#include <windows.h>
#include <wincodec.h>
#include <winhttp.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <wrl/client.h>

namespace crome::visual {
inline HWND clientWindow(DWORD pid){
    struct Search{DWORD pid;HWND result{};} search{pid};
    EnumWindows([](HWND hwnd,LPARAM data)->BOOL{
        auto& s=*reinterpret_cast<Search*>(data);DWORD p{};GetWindowThreadProcessId(hwnd,&p);
        if(p==s.pid&&IsWindowVisible(hwnd)&&!GetWindow(hwnd,GW_OWNER)){s.result=hwnd;return FALSE;}return TRUE;
    },reinterpret_cast<LPARAM>(&search));return search.result;
}
inline Image capture(HWND target,RECT area,HWND overlay,std::string& error){
    error.clear();RECT client{};
    if(!target||IsIconic(target)||GetAncestor(GetForegroundWindow(),GA_ROOT)!=target){error="Deixe o cliente em primeiro plano.";return {};}
    DWORD affinity{};
    if(GetWindowDisplayAffinity(target,&affinity)&&affinity){error="Captura protegida pelo cliente.";return {};}
    GetClientRect(target,&client);
    int width=area.right-area.left,height=area.bottom-area.top;
    if(area.left<0||area.top<0||area.right>client.right||area.bottom>client.bottom||width<32||height<32||width>2048||height>2048){error="Area invalida; calibre novamente.";return {};}
    POINT origin{area.left,area.top};ClientToScreen(target,&origin);
    RECT screen{origin.x,origin.y,origin.x+width,origin.y+height},other{},intersection{};
    if(overlay&&IsWindowVisible(overlay)&&GetWindowRect(overlay,&other)&&IntersectRect(&intersection,&screen,&other)){error="Mova o Crome para fora do mapa.";return {};}
    HDC desktop=GetDC(nullptr),memory=CreateCompatibleDC(desktop);void* bits{};
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;
    info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    HBITMAP bitmap=CreateDIBSection(desktop,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    HGDIOBJ previous=bitmap&&memory?SelectObject(memory,bitmap):nullptr;
    Image image;
    if(previous&&bits&&BitBlt(memory,0,0,width,height,desktop,origin.x,origin.y,SRCCOPY)){
        image={width,height,{}};image.pixels.resize(static_cast<std::size_t>(width)*height);
        auto* raw=static_cast<const unsigned char*>(bits);
        for(std::size_t i=0;i<image.pixels.size();++i)image.pixels[i]={raw[i*4+2],raw[i*4+1],raw[i*4],255};
    }
    if(previous)SelectObject(memory,previous);if(bitmap)DeleteObject(bitmap);if(memory)DeleteDC(memory);if(desktop)ReleaseDC(nullptr,desktop);
    if(!hasDetail(image)){error="Captura vazia/uniforme; reconhecimento suspenso.";return {};}
    return image;
}
inline bool saveBmp(const std::filesystem::path& path,const Image& image){
    if(!image.valid())return false;
    BITMAPFILEHEADER file{};BITMAPINFOHEADER info{};
    file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info);file.bfSize=file.bfOffBits+image.pixels.size()*4;
    info.biSize=sizeof(info);info.biWidth=image.width;info.biHeight=-image.height;info.biPlanes=1;info.biBitCount=32;
    std::ofstream out(path,std::ios::binary);if(!out)return false;
    out.write(reinterpret_cast<const char*>(&file),sizeof(file));out.write(reinterpret_cast<const char*>(&info),sizeof(info));
    for(auto p:image.pixels){unsigned char bytes[]={p.b,p.g,p.r,p.a};out.write(reinterpret_cast<char*>(bytes),4);}return bool(out);
}
inline Image loadImage(const std::filesystem::path& path){
    using Microsoft::WRL::ComPtr;
    HRESULT init=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    struct ComScope{HRESULT init;~ComScope(){if(SUCCEEDED(init))CoUninitialize();}} scope{init};
    ComPtr<IWICImagingFactory> factory;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;ComPtr<IWICFormatConverter> convert;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)))||
       FAILED(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder))||
       FAILED(decoder->GetFrame(0,&frame))||FAILED(factory->CreateFormatConverter(&convert))||
       FAILED(convert->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))return {};
    UINT w{},h{};if(FAILED(convert->GetSize(&w,&h))||w<1||h<1||w>2048||h>2048)return {};
    Image image{int(w),int(h),{}};image.pixels.resize(static_cast<std::size_t>(w)*h);
    static_assert(sizeof(Pixel)==4);
    if(FAILED(convert->CopyPixels(nullptr,w*4,UINT(image.pixels.size()*4),reinterpret_cast<BYTE*>(image.pixels.data()))))return {};
    return normalized(image);
}
inline std::vector<unsigned char> catalogGet(const std::wstring& route){
    struct Http{HINTERNET handle{};~Http(){if(handle)WinHttpCloseHandle(handle);}};
    Http session{WinHttpOpen(L"Crome-Visual74/1.0",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,nullptr,nullptr,0)};
    if(!session.handle)throw std::runtime_error("Nao foi possivel abrir consulta.");
    WinHttpSetTimeouts(session.handle,4000,4000,6000,6000);
    Http connection{WinHttpConnect(session.handle,L"tibiadata.bytewizards.de",443,0)};
    Http request{WinHttpOpenRequest(connection.handle,L"GET",route.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE)};
    DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if(!request.handle)throw std::runtime_error("Falha HTTP.");
    WinHttpSetOption(request.handle,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects));
    if(!WinHttpSendRequest(request.handle,WINHTTP_NO_ADDITIONAL_HEADERS,0,nullptr,0,0,0)||!WinHttpReceiveResponse(request.handle,nullptr))throw std::runtime_error("Consulta indisponivel; use referencias locais.");
    DWORD status{},size=sizeof(status);WinHttpQueryHeaders(request.handle,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&size,nullptr);
    if(status!=200)throw std::runtime_error("Catalogo HTTP "+std::to_string(status));
    std::vector<unsigned char> body;unsigned char buffer[16384];DWORD count{};
    for(;;){if(!WinHttpReadData(request.handle,buffer,sizeof(buffer),&count))throw std::runtime_error("Leitura HTTP interrompida.");if(!count)break;
        if(body.size()+count>8*1024*1024)throw std::runtime_error("Referencia grande demais.");body.insert(body.end(),buffer,buffer+count);}
    return body;
}
inline std::wstring encodedName(const std::string& name){
    const wchar_t hex[]=L"0123456789ABCDEF";std::wstring result;
    for(unsigned char c:name){if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.')result+=wchar_t(c);
        else{result+=L'%';result+=hex[c>>4];result+=hex[c&15];}}
    return result;
}
}
