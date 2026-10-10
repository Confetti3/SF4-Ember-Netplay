#pragma once
#include "../common/UpdateChannel.hxx"
#include <algorithm>
#include <optional>

namespace sf4e { namespace ui {
// What switching from one update channel to another means, for its confirm.
// The channels run Stable, Beta, Nightly, oldest builds first, so a move to an
// earlier one goes back.
enum class ChannelMove { None, Forward, ToNightly, GoesBack };
inline ChannelMove ClassifyChannelMove(updates::UpdateChannel from,updates::UpdateChannel to) {
    if(from==to)return ChannelMove::None;
    if(to==updates::UpdateChannel::Nightly)return ChannelMove::ToNightly;
    return static_cast<int>(to)<static_cast<int>(from)?ChannelMove::GoesBack:ChannelMove::Forward;
}
// The updater's channel row. Left and Right pick a channel without saving
// anything, stopping at Stable and at Nightly, so Beta to Stable never passes
// through Nightly. Select applies the pick through a confirm. A pick belongs
// to the saved channel it was made from: once the switch is saved, or fails,
// or the saved channel changes some other way, the row shows the saved one.
struct ChannelPick {
    std::optional<updates::UpdateChannel> picked;
    updates::UpdateChannel basedOn=updates::UpdateChannel::Stable;
    // The confirmed pick is being saved and checked; the row keeps showing it until that ends.
    bool applying=false;
    updates::UpdateChannel Shown(updates::UpdateChannel saved) const { return picked&&basedOn==saved?*picked:saved; }
    // A pick other than the saved channel is waiting for Select.
    bool Pending(updates::UpdateChannel saved) const { return Shown(saved)!=saved; }
    void Step(updates::UpdateChannel saved,int delta) {
        const int last=static_cast<int>(updates::UpdateChannel::Nightly);
        const int next=(std::max)(0,(std::min)(last,static_cast<int>(Shown(saved))+(delta>0?1:delta<0?-1:0)));
        picked=static_cast<updates::UpdateChannel>(next);basedOn=saved;
        if(*picked==saved)picked.reset();
    }
    // Called once a frame before the row is built; busy is the service worker's.
    void Settle(updates::UpdateChannel saved,bool busy) {
        if(picked&&(basedOn!=saved||(applying&&!busy))){picked.reset();applying=false;}
        else if(!picked)applying=false;
    }
};
} }
