#include "../modules/explore/VisualCaptureWin.hpp"
#include <iostream>
using namespace crome::visual;
int main(int argc,char** argv){try{
    if(argc>1){
        if(argc>2){int id=std::stoi(argv[2]);if(id<=0)throw std::runtime_error("invalid asset");
            auto body=catalogGet(L"/api/v1/assets/"+std::to_wstring(id));
            auto path=std::filesystem::temp_directory_path()/(L"crome_asset_test_"+std::to_wstring(GetCurrentProcessId())+L".img");
            std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(body.data()),body.size());file.close();
            auto image=loadImage(path);std::filesystem::remove(path);
            std::cout<<"Asset bytes: "<<body.size()<<"; WIC decoded: "<<image.valid()<<'\n';return image.valid()?0:1;
        }
        auto body=catalogGet(L"/api/v1/items/Rope");std::cout<<"Catalog HTTP bytes: "<<body.size()<<'\n';
        std::ofstream file("work/crome-ui-review/catalog-response.json",std::ios::binary);file.write(reinterpret_cast<const char*>(body.data()),body.size());return body.empty()?1:0;
    }
    if(encodedName("Fire Field/a?b")!=L"Fire%20Field%2Fa%3Fb")throw std::runtime_error("query name escaping failed");
    std::string error;if(capture(nullptr,{0,0,100,100},nullptr,error).valid()||error.empty())throw std::runtime_error("capture accepted no client");
    Image input{32,32,{}};for(int y=0;y<32;++y)for(int x=0;x<32;++x)input.pixels.push_back({std::uint8_t(x*7),std::uint8_t(y*7),std::uint8_t(x*y),255});
    auto path=std::filesystem::temp_directory_path()/(L"crome_visual_test_"+std::to_wstring(GetCurrentProcessId())+L".bmp");
    if(!saveBmp(path,input))throw std::runtime_error("BMP write failed");auto output=loadImage(path);std::filesystem::remove(path);
    if(!output.valid()||difference(input,output)!=0)throw std::runtime_error("WIC decode changed pixels");
    std::cout<<"PASS: invalid capture rejected, URL encoded, BMP/WIC pixel roundtrip\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
