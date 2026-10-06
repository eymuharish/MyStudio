#include "telestudio/media/CaptureCatalogC.h"
#include "telestudio/media/DesktopCaptureSource.h"
#include <vector>
#include <stdexcept>
#include <iostream>
namespace telestudio::testing {
struct SyntheticCatalogEntry { CaptureItemInfo item; bool primary=false; };
void ConfigureCaptureCatalog(std::vector<SyntheticCatalogEntry>,bool,bool);
}
int main() {
    try {
        using namespace telestudio;
        auto check=[](bool value){if(!value) throw std::runtime_error("camera C ABI contract");};
        testing::ConfigureCaptureCatalog({{{"stable-id",CaptureItemKind::Monitor,"Friendly camera",1,1},false}},false,false);
        uint32_t count=0,bytes=0; uint64_t stamp=0;
        check(EssaCameraCatalogQueryV1(1,0,nullptr,0,nullptr,0,&count,&bytes,&stamp)==ESSA_CAPTURE_BUFFER_TOO_SMALL);
        check(count==1 && stamp!=0);
        std::vector<EssaCaptureCatalogItemV1> items(count); std::vector<uint8_t> blob(bytes);
        check(EssaCameraCatalogQueryV1(1,stamp,items.data(),count,blob.data(),bytes,&count,&bytes,&stamp)==ESSA_CAPTURE_OK);
        check(items[0].kind==3 && items[0].width==0 && items[0].height==0);
        check(std::string(blob.begin(),blob.begin()+9)=="stable-id");
        const auto previous=stamp;
        testing::ConfigureCaptureCatalog({},false,false);
        check(EssaCameraCatalogQueryV1(1,previous,nullptr,0,nullptr,0,&count,&bytes,&stamp)==ESSA_CAPTURE_CATALOG_CHANGED);
        check(EssaCameraCatalogQueryV1(1,0,nullptr,0,nullptr,0,&count,&bytes,&stamp)==ESSA_CAPTURE_OK && count==0);
        check(EssaCaptureCatalogAbiVersion()==1);
        std::cout<<"camera catalog synthetic contract passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what();return 1;}
}
