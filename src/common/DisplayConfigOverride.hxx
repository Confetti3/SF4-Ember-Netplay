#pragma once
#include <cstdint>
namespace sf4e { namespace display {
struct NativeDisplayValues {
    std::uint32_t width=0,height=0,fullscreen=0;
    float refresh=0;
};
class DisplayConfigWriteScope {
public:
    DisplayConfigWriteScope(std::uint32_t& width,std::uint32_t& height,float& refresh,std::uint32_t& fullscreen,
        const NativeDisplayValues& requested)
      : width_(width),height_(height),fullscreen_(fullscreen),refresh_(refresh),
        previous_{width,height,fullscreen,refresh} {
        width_=requested.width;height_=requested.height;refresh_=requested.refresh;fullscreen_=requested.fullscreen;
    }
    ~DisplayConfigWriteScope() {
        width_=previous_.width;height_=previous_.height;refresh_=previous_.refresh;fullscreen_=previous_.fullscreen;
    }
    DisplayConfigWriteScope(const DisplayConfigWriteScope&)=delete;
    DisplayConfigWriteScope& operator=(const DisplayConfigWriteScope&)=delete;
private:
    std::uint32_t &width_,&height_,&fullscreen_; float& refresh_;
    NativeDisplayValues previous_;
};
} }
