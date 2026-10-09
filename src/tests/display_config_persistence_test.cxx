#include "../common/DisplayConfigOverride.hxx"
#include "../Dimps/Dimps__Platform.hxx"
#include "test_support.hxx"
#include <cstring>
#include <stdexcept>
using namespace sf4e::display;
int main() {
    alignas(4) unsigned char storage[0x200]{};
    auto* main=reinterpret_cast<Dimps::Platform::Main*>(storage);
    const NativeDisplayValues original{1280,720,1,60.f};
    auto& width=*Dimps::Platform::Main::GetDisplayWidth(main);
    auto& height=*Dimps::Platform::Main::GetDisplayHeight(main);
    auto& refresh=*Dimps::Platform::Main::GetDisplayRefresh(main);
    auto& fullscreen=*Dimps::Platform::Main::GetDisplayFullscreen(main);
    // Brightness ApplyAll copied the overridden 13-dword device block to Main.
    width=2560;height=1440;refresh=144;fullscreen=0;
    storage[0x100]=7; // An unrelated option remains unchanged.
    const auto write=[&]{
        CHECK(width==1280&&height==720&&refresh==60&&fullscreen==1);
        CHECK(storage[0x100]==7);
        CHECK(*reinterpret_cast<std::uint32_t*>(storage+0xe8)==1280);
        CHECK(*reinterpret_cast<std::uint32_t*>(storage+0xec)==720);
        CHECK(*reinterpret_cast<float*>(storage+0xf0)==60);
        CHECK(*reinterpret_cast<std::uint32_t*>(storage+0x10c)==1);
    };
    for(int path=0;path<2;++path){ // Graphics-options save and shutdown write.
        { DisplayConfigWriteScope scope(width,height,refresh,fullscreen,original);write(); }
        CHECK(width==2560&&height==1440&&refresh==144&&fullscreen==0&&storage[0x100]==7);
    }
    try{DisplayConfigWriteScope scope(width,height,refresh,fullscreen,original);write();throw std::runtime_error("writer");}
    catch(const std::runtime_error&){}
    CHECK(width==2560&&height==1440&&refresh==144&&fullscreen==0);
    return 0;
}
