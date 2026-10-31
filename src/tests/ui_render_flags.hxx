#pragma once
// Country flags: before the name on the member cards and the table card, on
// the player card, and the country picker with a typed letter and a search.
// Members with long names and long country names, one Hidden. The countries
// are taken off again so the later shots are unchanged.
#include "ui_render_support.hxx"
#include "../ui/ApplicationShell.hxx"
#include "../ui/SelectionArt.hxx"
#include "../common/Country.hxx"
#include "../common/Localization.hxx"
#include <chrono>
#include <thread>
#include <utility>
#include <vector>
// `rebuild` applies the theme, so the names noted on screen reach the atlas.
template<class Draw,class Page,class Rebuild>
void ShootFlags(sf4e::ui::ApplicationShell& shell,sf4e::ui::ShellView& view,sf4e::ui::SelectionArt* art,const Draw& draw,const Page& page,const Rebuild& rebuild) {
    using namespace sf4e;using namespace ui;
    if(art){
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!art->Flag(CountryCode::Parsed("JP")).texture&&!art->Flag(CountryCode::Parsed("JP")).missing&&std::chrono::steady_clock::now()<deadline){
            art->Pump();std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const auto flag=art->Flag(CountryCode::Parsed("JP"));
        Require(flag.texture&&flag.width==countries::CellWidth&&flag.height==countries::CellHeight,"The flag atlas did not load at its own size");
    }
    // Member 2 has the long name; Saint Vincent and Bosnia have long country names; member 4 is Hidden.
    const char* codes[]={"JP","VC","BR","","BA","GB","US","KR","FR","DE","MX","CA","AU","ZA","NP","AE"};
    std::vector<CountryCode> before;
    for(std::size_t i=0;i<view.room.members.size();++i){before.push_back(view.room.members[i].country);view.room.members[i].country=CountryCode::Parsed(i<16?codes[i]:"");}
    page("room");draw("flags-room");
    page("room-members");draw("flags-members");
    // The player card shows the country the preference resolves to.
    const auto savedPreferences=view.preferences;const auto savedAutomatic=view.automaticCountry;const bool savedEdit=view.canEditPreferences;
    view.automaticCountry=CountryCode::Parsed("CA");view.preferences.country=CountryPreference::Automatic();
    page("home");draw("flags-player-card");
    view.canEditPreferences=true;
    // Country names are drawn as names are: noted on one frame (once the list
    // has scrolled to its focus), in the atlas on the next.
    // The earlier shots' player text has filled the noted glyphs; retire it first.
    SetUserGlyphRebuildInterval(std::chrono::milliseconds(0));
    SetUserGlyphRetention(std::chrono::milliseconds(0));std::this_thread::sleep_for(std::chrono::milliseconds(2));rebuild();
    SetUserGlyphRetention(std::chrono::milliseconds(10000));
    page("tab-player/1");rebuild();draw("flags-settings");
    page(CountryPicker::Screen);rebuild();draw("flags-country-picker");
    // Lettered, the picker is a board that opens on Automatic; a language with
    // too many initials for a grid keeps the list, which opens on its search.
    Require(shell.Navigation().Focus()=="country-auto"||shell.Navigation().Focus()=="country-search","The country picker did not open on its first row");
    // A typed letter goes to the first country that starts with it: here the
    // first character of Namibia's name in the language on show.
    const std::string namibia=loc::T("country.NA");
    const auto lead=static_cast<unsigned char>(namibia[0]);
    const int length=lead<0x80?1:lead>=0xF0?4:lead>=0xE0?3:2;
    unsigned typed=length==1?lead:lead&(0xFFu>>(length+1));
    for(int i=1;i<length;++i)typed=(typed<<6)|(static_cast<unsigned char>(namibia[i])&0x3Fu);
    ImGui::GetIO().AddInputCharacter(typed);draw(nullptr,0,2);rebuild();draw("flags-country-letter");
    Require(shell.Navigation().Focus().compare(0,8,"country-")==0&&shell.Navigation().Focus().size()==10,"A typed letter did not reach a country");
    SetUserGlyphRebuildInterval(std::chrono::milliseconds(250));
    view.preferences=savedPreferences;view.automaticCountry=savedAutomatic;view.canEditPreferences=savedEdit;
    for(std::size_t i=0;i<view.room.members.size();++i)view.room.members[i].country=before[i];
    page("room");
}
