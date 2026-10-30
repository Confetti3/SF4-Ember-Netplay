#include "FighterSelector.hxx"
#include "Theme.hxx"
#include "RoomCards.hxx"
#include <cfloat>
#include <functional>
#include "MenuRows.hxx"
#include "../common/Localization.hxx"
#include "../common/StageCatalog.hxx"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace sf4e { namespace ui {
namespace {
using palette::Ember;
using palette::Ivory;
using palette::Muted;

// Motion icons are vector paths, so they retain their shape at every DPI.
void CommandSymbols(const char* notation) {
    std::istringstream stream(notation);
    std::vector<std::string> tokens;
    for (std::string token; stream >> token;) tokens.push_back(token);
    const float scale = Scale(), unit = 30 * scale, gap = 3 * scale;
    const float available = ImGui::GetContentRegionAvail().x;
    float x = 0, y = 0;
    const auto origin = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    const auto arrow = [&](ImVec2 tip, ImVec2 direction) {
        const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y);
        if (length < .001f) return;
        direction.x /= length; direction.y /= length;
        draw->AddTriangleFilled(tip,
            ImVec2(tip.x - 7 * scale * direction.x + 4 * scale * direction.y, tip.y - 7 * scale * direction.y - 4 * scale * direction.x),
            ImVec2(tip.x - 7 * scale * direction.x - 4 * scale * direction.y, tip.y - 7 * scale * direction.y + 4 * scale * direction.x), Ivory);
    };
    for (auto token : tokens) {
        const bool charge = token[0] == '~';
        if (charge) token.erase(0, 1);
        const bool triple = token == "PPP" || token == "KKK";
        const bool button = token.find('P') != std::string::npos || token.find('K') != std::string::npos;
        const float width = triple ? 2.4f * unit : token == "+" ? .6f * unit : unit;
        if (x > 0 && x + width > available) { x = 0; y += unit + 13 * scale; }
        const ImVec2 p(origin.x + x, origin.y + y), center(p.x + unit / 2, p.y + unit / 2);
        if (button) {
            const int count = triple ? 3 : 1;
            const char* label = token.c_str();
            char single[] = {token[0], 0};
            if (triple) label = single;
            for (int i = 0; i < count; ++i) {
                const ImVec2 c(center.x + i * unit * .7f, center.y);
                draw->AddCircleFilled(c, unit * .44f, IM_COL32(48, 36, 25, 255));
                draw->AddCircle(c, unit * .44f, Ember, 24, 1.5f * scale);
                const auto size = ImGui::CalcTextSize(label);
                draw->AddText(ImVec2(c.x - size.x / 2, c.y - size.y / 2), Ivory, label);
            }
        } else if (token == "+") {
            const auto size = ImGui::CalcTextSize("+");
            draw->AddText(ImVec2(p.x + (width - size.x) / 2, center.y - size.y / 2), Muted, "+");
        } else if (token.size() == 1) {
            const int digit = token[0] - '0';
            const ImVec2 direction(static_cast<float>((digit - 1) % 3 - 1), static_cast<float>(1 - (digit - 1) / 3));
            const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y);
            const ImVec2 d(direction.x / length, direction.y / length);
            const ImVec2 tip(center.x + 11 * scale * d.x, center.y + 11 * scale * d.y);
            draw->AddLine(ImVec2(center.x - 10 * scale * d.x, center.y - 10 * scale * d.y), tip, Ivory, 3 * scale);
            arrow(tip, d);
        } else {
            const float pi = 3.14159265f;
            draw->AddCircleFilled(center, unit * .48f, IM_COL32(47, 58, 59, 255));
            const float begin = token == "236" ? pi : token == "214" || token == "63214" ? 0.f : pi / 2;
            const float end = token == "236" || token == "214" ? pi / 2 : token == "63214" ? pi : -1.3f * pi;
            ImVec2 points[25];
            for (int i = 0; i < 25; ++i) {
                const float angle = begin + (end - begin) * i / 24;
                points[i] = ImVec2(center.x + 11 * scale * std::cos(angle), center.y + 11 * scale * std::sin(angle));
            }
            draw->AddPolyline(points, 25, Ivory, 0, 3 * scale);
            arrow(points[24], ImVec2(points[24].x - points[23].x, points[24].y - points[23].y));
            draw->AddCircleFilled(points[0], 2 * scale, Ember);
        }
        if (charge) {
            // A charge mark over the glyph; a longer translation shrinks to fit it.
            const char* hold = loc::T("selection.hold");
            const float fontSize = (std::min)(9 * scale, 9 * scale * width / (std::max)(1.f, ImGui::GetFont()->CalcTextSizeA(9 * scale, FLT_MAX, 0, hold).x));
            draw->AddText(ImGui::GetFont(), fontSize, ImVec2(p.x, p.y + unit), Ember, hold);
        }
        x += width + gap;
    }
    ImGui::Dummy(ImVec2((std::min)(available, x), y + unit + 12 * scale));
}
void UltraMoveInput(int fighterId, int ultra, int edition) {
    const auto* fighter = selection::FindFighter(fighterId);
    ImGui::TextWrapped("%s", fighter->ultras[ultra]);
    for (const auto& command : selection::UltraCommands(fighterId, ultra, edition)) {
        if (*command.condition) ImGui::TextWrapped("%s", command.condition);
        CommandSymbols(command.symbols);
        // The same input in words, with no left or right in it, for a player facing the other way.
        ImGui::TextDisabled("%s", selection::UltraNotation(command.symbols).c_str());
    }
}
void ImageInRect(const SelectionImage& image, ImVec2 min, ImVec2 max) {
    if (!image.texture || !image.width || !image.height) {
        const char* label=loc::T(image.missing?"selection.preview_unavailable":"selection.preview_loading");
        const auto size=ImGui::CalcTextSize(label);
        const float factor=(std::max)(0.f,(std::min)(1.f,(std::min)((max.x-min.x-8*Scale())/(std::max)(1.f,size.x),
            (max.y-min.y-8*Scale())/(std::max)(1.f,size.y))));
        ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(),ImGui::GetFontSize()*factor,
            ImVec2((min.x+max.x-size.x*factor)*.5f,(min.y+max.y-size.y*factor)*.5f),Muted,label);
        return;
    }
    const float factor = (std::min)((max.x - min.x) / image.width, (max.y - min.y) / image.height);
    const ImVec2 size(image.width * factor, image.height * factor);
    const ImVec2 start(min.x + (max.x - min.x - size.x) / 2, min.y + (max.y - min.y - size.y) / 2);
    ImGui::GetWindowDrawList()->AddImage(image.texture, start, ImVec2(start.x + size.x, start.y + size.y), image.uvMin, image.uvMax);
}
bool ImageCard(const char* id, const char* label, const SelectionImage& image, ImVec2 size, bool selected) {
    ImGui::PushID(id);
    if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.32f, .17f, .08f, 1));
    const bool clicked = ImGui::Button("##choice", size);
    if (selected) ImGui::PopStyleColor();
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    auto* draw = ImGui::GetWindowDrawList();
    const float labelHeight = ImGui::GetTextLineHeight() + 6 * Scale();
    ImageInRect(image, ImVec2(min.x + 2, min.y + 2), ImVec2(max.x - 2, max.y - labelHeight));
    if (!image.texture) {
        const char* message = image.missing ? loc::T("selection.preview_unavailable") : "...";
        const auto text = ImGui::CalcTextSize(message);
        const float messageScale = (std::max)(0.f, (std::min)(1.f, (std::min)(
            (size.x - 8 * Scale()) / (std::max)(1.f, text.x),
            (size.y - labelHeight - 8 * Scale()) / (std::max)(1.f, text.y))));
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * messageScale,
            ImVec2(min.x + (size.x - text.x * messageScale) / 2,
                   min.y + (size.y - labelHeight - text.y * messageScale) / 2), Muted, message);
    }
    draw->AddRectFilled(ImVec2(min.x + 1, max.y - labelHeight), ImVec2(max.x - 1, max.y - 1), IM_COL32(20, 19, 18, 235));
    const auto text = ImGui::CalcTextSize(label);
    const float labelScale = (std::min)(1.f, (size.x - 6 * Scale()) / (std::max)(1.f, text.x));
    draw->PushClipRect(ImVec2(min.x + 3, max.y - labelHeight), ImVec2(max.x - 3, max.y), true);
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * labelScale,
                  ImVec2(min.x + (std::max)(3.f, (size.x - text.x * labelScale) / 2), max.y - labelHeight + 2 * Scale()),
                  selected ? Ember : Ivory, label);
    draw->PopClipRect();
    if (selected) draw->AddRect(min, max, Ember, 2 * Scale(), 0, 2 * Scale());
    // The preview is drawn over the button, so redraw focus above the image.
    if (ImGui::IsItemFocused()) draw->AddRect(ImVec2(min.x + 3, min.y + 3), ImVec2(max.x - 3, max.y - 3), Ivory, 2 * Scale(), 0, 2 * Scale());
    ImGui::PopID();
    return clicked;
}
SelectionImage Missing() { SelectionImage image; image.missing = true; return image; }
int PreviewColor(int fighter,int costume,const selection::Availability& availability,const selection::InstalledContent* installed) {
    const auto colors=selection::AllowedColors(fighter,costume,availability,installed);
    return colors.empty()?0:colors.front();
}
const char* HandicapLabel(int handicap) {
    static const char* const ids[]={"selection.handicap_normal","selection.handicap_one_hit","selection.handicap_25","selection.handicap_50","selection.handicap_75"};
    return loc::T(ids[(std::max)(0,(std::min)(4,handicap))]);
}
// Where Select saves the focused card, the legend says what it saves.
MenuEntry Saving(MenuEntry row,const char* hint,bool editable) {
    if(editable)row.hint=loc::T(hint);
    return row;
}
}
std::string StageLabel(int stageId, const selection::InstalledContent* installed) {
    if (selection::IsRandomStage(stageId)) return loc::T("selection.random_stage");
    const int id = selection::NormalizeStage(stageId);
    if (const auto* stage = selection::FindStage(id)) return stage->name;
    // A custom stage is named by the setup program, or by its code.
    const std::string name = installed ? installed->StageName(id) : std::string();
    return name.empty() ? loc::Tf("selection.custom", selection::StageCode(id)) : name;
}
std::string CostumeLabel(const selection::Pick& pick, const selection::InstalledContent* installed) {
    if (pick.costume == 0) return loc::T("selection.original");
    // A custom costume is named by the setup program, or by its slot as it numbers them (slot = index + 1).
    if (selection::IsCustomCostume(pick.costume)) {
        const std::string name = installed ? installed->Name(pick.fighter, pick.costume) : std::string();
        return name.empty() ? loc::Tf("selection.custom", pick.costume + 1) : name;
    }
    return loc::Tf("selection.alternate_pack", pick.costume, selection::CostumePack(pick.fighter, pick.costume));
}
const char* UltraLabel(int ultra) {
    return loc::T(ultra==2?"selection.ultra_double":ultra==1?"selection.ultra_two":"selection.ultra_one");
}
std::string ShortAppearanceLabel(const selection::Pick& pick, const selection::InstalledContent* installed) {
    const std::string pack = selection::CostumePack(pick.fighter, pick.costume);
    const std::string costume = pick.costume > 0 && pack == "Alternate" ? loc::Tf("home.selection_alternate", pick.costume) :
        pick.costume > 0 && !pack.empty() ? pack : CostumeLabel(pick, installed);
    return loc::Tf("home.selection_appearance", costume, pick.color + 1);
}
const char* ShortUltraLabel(int ultra) {
    return loc::T(ultra==2?"home.selection_ultra_double":ultra==1?"home.selection_ultra_two":"home.selection_ultra_one");
}
namespace {
// "Ultra I: Metsu Hadoken", or "Ultra Double".
std::string UltraName(const selection::Pick& pick) {
    if(pick.ultra==2) return loc::T("selection.ultra_double");
    return std::string(UltraLabel(pick.ultra))+": "+selection::FindFighter(pick.fighter)->ultras[pick.ultra];
}
}



bool DrawStageSelector(int& nativeId, SelectionArt* art) {
    const int previous = nativeId;
    nativeId = selection::NormalizeStage(nativeId);
    const std::string selected = StageLabel(nativeId);
    const float scale = Scale(), width = ImGui::GetContentRegionAvail().x;
    const bool wide = width >= 800 * scale && ImGui::BeginTable("Stage layout", 2, ImGuiTableFlags_SizingStretchProp);
    if (wide) {
        ImGui::TableSetupColumn("Selected stage", ImGuiTableColumnFlags_WidthFixed, 280 * scale);
        ImGui::TableSetupColumn("Stages"); ImGui::TableNextColumn();
    }
    const float previewWidth = (std::min)(ImGui::GetContentRegionAvail().x, 360 * scale);
    const auto start = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(previewWidth, previewWidth * 9 / 16));
    ImageInRect(art ? art->Stage(nativeId) : Missing(), start, ImVec2(start.x + previewWidth, start.y + previewWidth * 9 / 16));
    ImGui::PushFont(HeadingFont()); ImGui::TextWrapped("%s", selected.c_str()); ImGui::PopFont();
    ImGui::TextDisabled("%s", loc::Tf("selection.stage_count", selection::VersusStageCount).c_str());
    ImGui::TextWrapped("%s", loc::T("selection.choose_battlefield"));
    if (wide) ImGui::TableNextColumn();
    else ImGui::Spacing();
    const int columns = (std::max)(1, (std::min)(4, static_cast<int>(ImGui::GetContentRegionAvail().x / (160 * scale))));
    if (ImGui::BeginTable("Stage cards", columns, ImGuiTableFlags_SizingStretchSame)) {
        for (const auto& stage : selection::StageList()) {
            ImGui::TableNextColumn();
            const float cardWidth = ImGui::GetContentRegionAvail().x;
            if (ImageCard(stage.code, stage.name, art ? art->Stage(stage.id) : Missing(),
                ImVec2(cardWidth, cardWidth * 9 / 16 + ImGui::GetTextLineHeight() + 6 * scale), stage.id == nativeId))
                nativeId = stage.id;
        }
        ImGui::EndTable();
    }
    if (wide) ImGui::EndTable();
    return previous != nativeId;
}


bool FighterSelector::Draw(selection::Pick& pick,bool editionSelect,SelectionArt* art,const AvailabilityReader& readAvailability,int* stageId,bool editable,const std::string& selectionError,selection::RandomPool* randomPool,const selection::InstalledContent* installed) {
 using namespace selection;
 bool changed=false;auto& nav=menu_.navigation;
 // A selector the shell has just opened starts on its first page, not on the
 // sub-page an earlier visit left it on; one it reshows resumes.
 // The parent says where Back from here goes and which of its shortcuts
 // this screen hands back to it.
 const auto& embedded=EmbeddedReturnContext();
 if(TakeEmbeddedFresh()){
  nav.Home();openOn_=embedded.openOn;
  // The board has the fighter, the costume and the Ultra on one screen: it opens on that row.
  if(openOn_=="roster"||openOn_=="ultra"||openOn_=="costumes")nav.Prefer(openOn_=="roster"?"fighter-"+std::to_string(pick.fighter):openOn_=="ultra"?"ultra":"costume");
  else if(openOn_=="stage"&&stageId){nav.Push("stage");nav.Prefer("stage-"+std::to_string(*stageId));}
  else if(openOn_=="options")nav.Push("options");
  else openOn_.clear();
 }
 if(ultraStepPending_&&nav.Screen()=="ultra"&&AllowedUltras(pick.fighter,pick.edition).size()<=1){
  // The new fighter has one Ultra in its own edition: nothing to pick.
  nav.Return();focusSavedUltra_=false;
  if(!openOn_.empty())ForwardMenuAction({MenuAction::Close});else nav.Home();
 }
 ultraStepPending_=false;
 if(focusSavedUltra_&&nav.Screen()=="ultra")nav.Prefer("ultra-"+std::to_string(pick.ultra));
 focusSavedUltra_=false;
 const auto screen=nav.Screen();
 menu_.rootName=loc::T("screen.selection");menu_.exitName=embedded.exitName;menu_.shortcutHints=embedded.shortcutHints;
 const auto availability=readAvailability?readAvailability(pick.fighter):Availability{};
 if(editable)changed=Normalize(pick,editionSelect,&availability,installed);
 static const std::vector<int> noStages;
 const std::vector<int>& customStages=installed?installed->stages:noStages;
 const char* locked=loc::T(editable?"selection.select_saves":"selection.locked_detail");
 // The option rows step with Left and Right and save as they change; Select does nothing on them.
 const char* adjusts=loc::T(editable?"selection.adjust_saves":"selection.locked_detail");
 std::vector<MenuEntry> rows;
 std::string title=loc::T("selection.title");int columns=1;float cardHeight=100;
 // Home is one board (Fighter.dc.html), the grid eight across and beside it the
 // preview, the value rows and Ready; the pages stay for the stage and options.
 const bool board=screen=="home";
 if(board){
  const int count=FighterCount;
  for(int i=0;i<count;++i){
   const int id=RosterDisplayOrder[i];
   const bool owned=!readAvailability||!FighterLocked(id,readAvailability(id));
   rows.push_back(Saving(Row("fighter-"+std::to_string(id),FindFighter(id)->name,owned?locked:loc::T("selection.not_owned"),editable&&owned),"menu.hint.save_fighter",editable&&owned));
   // Right from the grid's right edge goes to the costume.
   if(i%BoardColumns==BoardColumns-1||i+1==count)rows.back().right="costume";
  }
  // Words open every choice in a panel, a number keeps its arrows (OfferSteps).
  const auto offer=[&](MenuEntry e,const std::vector<int>& ids,const std::function<std::string(int)>& name){
   std::vector<std::string> labels;for(int id:ids)labels.push_back(name(id));
   OfferSteps(e,labels);e.wide=true;rows.push_back(std::move(e));
  };
  offer(Value("costume",loc::T("selection.costume"),CostumeLabel(pick,installed),adjusts,editable&&availability.ready),AllowedCostumes(pick.fighter,availability,installed),
   [&](int costume){auto option=pick;option.costume=costume;return CostumeLabel(option,installed);});
  const std::string colorName=installed&&selection::IsCustomColor(pick.color)?installed->Name(pick.fighter,pick.costume,pick.color):std::string();
  rows.push_back(Value("color",loc::T("selection.color_row"),colorName.empty()?std::to_string(pick.color+1):colorName,adjusts,editable&&availability.ready));
  rows.back().wide=true;
  offer(Value("ultra",loc::T("selection.ultra_combo"),ShortUltraLabel(pick.ultra),adjusts,editable&&AllowedUltras(pick.fighter,pick.edition).size()>1),AllowedUltras(pick.fighter,pick.edition),
   [](int ultra){return std::string(ShortUltraLabel(ultra));});
  offer(Value("edition",loc::T("selection.edition"),FindEdition(pick.edition)->name,editionSelect?adjusts:loc::T("selection.usfiv_rules"),editable&&editionSelect),AllowedEditions(pick.fighter,editionSelect),
   [](int edition){return std::string(FindEdition(edition)->name);});
  rows.push_back(Row("stage",loc::T("selection.stage"),stageId?loc::T("selection.choose_battlefield"):loc::T("selection.p1_stage"),stageId!=nullptr));
  rows.back().value=stageId?StageLabel(*stageId,installed):std::string();
  rows.push_back(Row("options",loc::T("selection.additional_options"),loc::T("selection.additional_options.detail")));
  rows.push_back(Row("ready",loc::Tf("selection.ready_with",FindFighter(pick.fighter)->name),loc::T("selection.ready_detail")));
  rows.back().left="fighter-"+std::to_string(pick.fighter);
  for(auto it=rows.begin()+count;it!=rows.end();++it)it->wide=true;
  columns=BoardColumns;
  // A focus from no row of the board (a first visit) starts on the saved fighter.
  if(std::none_of(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==nav.Focus();}))nav.Prefer("fighter-"+std::to_string(pick.fighter));
 }else if(screen=="roster"){
  page_=Page::Fighter;title=loc::T("selection.choose_fighter");
  // USFIV's own select order, 15 across when the list has the room for it.
  // A fighter the player's Steam licences do not cover keeps its place in the
  // grid, so the layout matches the game's, but cannot be saved.
  for(const int id:RosterDisplayOrder){
   const bool owned=!readAvailability||!FighterLocked(id,readAvailability(id));
   rows.push_back(Saving(Row("fighter-"+std::to_string(id),FindFighter(id)->name,owned?locked:loc::T("selection.not_owned"),editable&&owned),"menu.hint.save_fighter",editable&&owned));
   rows.back().key=static_cast<std::uint64_t>(id);
  }
  const auto grid=LayOutRosterGrid(ImGui::GetContentRegionAvail().x);
  columns=grid.columns;cardHeight=grid.cardHeight;
 }else if(screen=="appearance"){
  page_=Page::Appearance;title=loc::T("selection.appearance_title");
  rows={Row("costumes",loc::T("selection.costume_gallery"),CostumeLabel(pick,installed)),
   Row("colors",loc::T("selection.color_gallery"),loc::Tf("selection.color_preview",pick.color+1))};
 }else if(screen=="costumes"||screen=="colors"){
  page_=Page::Appearance;title=loc::T(screen=="costumes"?"selection.costume_gallery_title":"selection.color_gallery_title");
  if(!availability.ready)rows.push_back(Row("waiting",loc::T("selection.loading_choices"),loc::T("selection.loading_choices.detail"),false));
  else if(screen=="costumes")for(int costume:AllowedCostumes(pick.fighter,availability,installed)){
   auto option=pick;option.costume=costume;
   const bool usable=!AllowedColors(pick.fighter,costume,availability,installed).empty();
   // The legend says Select saves, so an editable card's detail is just its
   // pack, which fits the compact gallery's two lines with the saved choice.
   rows.push_back(Saving(Row("costume-"+std::to_string(costume),costume==0?loc::T("selection.original"):selection::IsCustomCostume(costume)?CostumeLabel(option,installed):loc::Tf("selection.alternate",costume),
    !usable?std::string(loc::T("selection.no_colors")):editable?CostumeLabel(option,installed):CostumeLabel(option,installed)+"\n"+locked,editable&&usable),"menu.hint.save_costume",editable));
   rows.back().key=static_cast<std::uint64_t>(costume);
  }else for(int color:AllowedColors(pick.fighter,pick.costume,availability,installed)){
   // A custom color is named by the setup program, else numbered like the rest.
   const std::string name=installed&&selection::IsCustomColor(color)?installed->Name(pick.fighter,pick.costume,color):std::string();
   rows.push_back(Saving(Row("color-"+std::to_string(color),name.empty()?loc::Tf("selection.color",color+1):name,editable?"":locked,editable),"menu.hint.save_color",editable));
   rows.back().key=static_cast<std::uint64_t>(color);
  }
  columns=(std::max)(2,(std::min)(4,static_cast<int>(ImGui::GetContentRegionAvail().x/(300*Scale()))));
 }else if(screen=="ultra"){
  page_=Page::Ultra;title=loc::T("selection.ultra_combo_title");
  columns=(std::max)(1,(std::min)(3,static_cast<int>(ImGui::GetContentRegionAvail().x/(280*Scale()))));
  for(int ultra:AllowedUltras(pick.fighter,pick.edition)){
    auto row=Saving(Row("ultra-"+std::to_string(ultra),UltraLabel(ultra),
    ultra==2?loc::T("selection.ultra_double.detail"):FindFighter(pick.fighter)->ultras[ultra],editable),"menu.hint.save_ultra",editable);
   if(ultra==pick.ultra)row.value=loc::T("selection.saved");
   row.key=static_cast<std::uint64_t>(ultra);
   rows.push_back(std::move(row));
  }
 }else if(screen=="stage"){
  page_=Page::Stage;title=loc::T("selection.stage_title");
  rows.push_back(Saving(Row("stage-"+std::to_string(RandomStageId),loc::T("selection.random_stage"),stageId?loc::T("selection.random_stage.detail"):loc::T("selection.only_p1_stage"),editable&&stageId),"menu.hint.save_stage",editable&&stageId));
  rows.back().key=static_cast<std::uint64_t>(RandomStageId);
  for(const auto& stage:StageList()){rows.push_back(Saving(Row("stage-"+std::to_string(stage.id),stage.name,stageId?locked:loc::T("selection.only_p1_stage"),editable&&stageId),"menu.hint.save_stage",editable&&stageId));
   rows.back().key=static_cast<std::uint64_t>(stage.id);}
  for(int id:customStages){rows.push_back(Saving(Row("stage-"+std::to_string(id),StageLabel(id,installed),stageId?locked:loc::T("selection.only_p1_stage"),editable&&stageId),"menu.hint.save_stage",editable&&stageId));
   rows.back().key=static_cast<std::uint64_t>(id);}
  if(stageId&&randomPool){
   rows.push_back(Row("random-pool",loc::T("selection.random_pool"),loc::Tf("selection.random_pool.detail",randomPool->Count(customStages),VersusStageCount+static_cast<int>(customStages.size()))));
   rows.back().wide=true;
  }
  // Derive columns like the roster and the galleries do. A fixed three columns
  // left 16:9 stage cards far below the width the sibling grids guarantee.
  columns=(std::max)(2,(std::min)(4,static_cast<int>(ImGui::GetContentRegionAvail().x/(220*Scale()))));
 }else if(screen=="random-pool"){
  // Select takes a stage out of Random or puts it back; the last one stays.
  page_=Page::Stage;title=loc::T("selection.random_pool_title");
  // The game's stages, then the installed custom stages; the pool keeps the last game stage in.
  const RandomPool everything;
  const RandomPool& current=randomPool?*randomPool:everything;
  std::vector<int> ids;
  for(const auto& stage:StageList())ids.push_back(stage.id);
  ids.insert(ids.end(),customStages.begin(),customStages.end());
  for(int id:ids){
   const bool in=current.Contains(id),last=in&&!current.CanToggle(id);
   auto row=Row("pool-"+std::to_string(id),StageLabel(id,installed),loc::T(last?"selection.random_pool_last":in?"selection.random_pool_in":"selection.random_pool_out"),
    editable&&randomPool&&!last);
   if(row.enabled)row.hint=loc::T(in?"menu.hint.skip_stage":"menu.hint.include_stage");
   row.key=static_cast<std::uint64_t>(id);
   rows.push_back(std::move(row));
  }
  columns=(std::max)(2,(std::min)(4,static_cast<int>(ImGui::GetContentRegionAvail().x/(220*Scale()))));
 }else{
  title=loc::T("selection.options_title");
  rows={Value("edition",loc::T("selection.edition"),FindEdition(pick.edition)->name,editionSelect?adjusts:loc::T("selection.usfiv_rules"),editable&&editionSelect),
   Value("action",loc::T("selection.personal_action"),pick.personalAction==255?loc::T("common.none"):std::to_string(pick.personalAction+1),adjusts,editable&&availability.ready),
   Value("quote",loc::T("selection.win_quote"),pick.winQuote==255?loc::T("selection.random"):std::to_string(pick.winQuote+1),adjusts,editable),
   Value("handicap",loc::T("selection.handicap"),HandicapLabel(pick.handicap),adjusts,editable)};
 }
 const bool compactAppearance=(screen=="costumes"||screen=="colors")&&ImGui::GetContentRegionAvail().x<820*Scale();
 const auto preview=[&](const std::string& id){
  if(screen=="ultra"){
   ImGui::TextWrapped("%s",loc::Tf("selection.saved_value",UltraLabel(pick.ultra)).c_str());
   if(id.compare(0,6,"ultra-")==0){
    const int ultra=std::stoi(id.substr(6));
    ImGui::TextWrapped("%s",loc::T(ultra==pick.ultra?"selection.ultra_selected":editable?"selection.preview_save":"selection.preview_locked"));
    if(ultra==2){UltraMoveInput(pick.fighter,0,pick.edition);UltraMoveInput(pick.fighter,1,pick.edition);}
    else UltraMoveInput(pick.fighter,ultra,pick.edition);
   }
   return;
  }
  if(compactAppearance){
   ImGui::TextWrapped("%s",loc::Tf("selection.saved_appearance",CostumeLabel(pick,installed),pick.color+1).c_str());return;
  }
  int focusFighter=pick.fighter,focusStage=stageId?*stageId:0,focusCostume=pick.costume,focusColor=pick.color;
  if(id.compare(0,8,"fighter-")==0)focusFighter=std::stoi(id.substr(8));
  if(id.compare(0,6,"stage-")==0)focusStage=std::stoi(id.substr(6));
  if(id.compare(0,5,"pool-")==0)focusStage=std::stoi(id.substr(5));
  if(id.compare(0,8,"costume-")==0){focusCostume=std::stoi(id.substr(8));focusColor=PreviewColor(pick.fighter,focusCostume,availability,installed);}
  if(id.compare(0,6,"color-")==0)focusColor=std::stoi(id.substr(6));
  ImGui::TextWrapped("%s",loc::Tf("selection.saved_full",FindFighter(pick.fighter)->name,CostumeLabel(pick,installed),pick.color+1).c_str());
  if(focusFighter!=pick.fighter||focusCostume!=pick.costume||focusColor!=pick.color)ImGui::TextWrapped("%s",loc::T("selection.preview_save_short"));
  const float width=(std::min)(ImGui::GetContentRegionAvail().x,300*Scale());
  const float height=(std::min)(ImGui::GetContentRegionAvail().y-10*Scale(),250*Scale());
  if(height>35*Scale()){
   const auto p=ImGui::GetCursorScreenPos();ImGui::Dummy(ImVec2(width,height));
   const auto img=!art?Missing():screen=="stage"||screen=="random-pool"?art->Stage(focusStage):(screen=="appearance"||screen=="costumes"||screen=="colors")?art->Appearance(pick.fighter,focusCostume,focusColor):art->Portrait(focusFighter,true);
   ImageInRect(img,p,ImVec2(p.x+width,p.y+height));
  }
 };
 GameMenu::Card card;
 const bool pool=screen=="random-pool";
 // An Ultra's card is its photo; Ultra Double is one large W on a dark panel.
 // A badge in the top-right corner names the Ultra (I, II or W); SAVED stays top-left.
 if(screen=="ultra")card=[&](const MenuEntry& e,ImVec2 min,ImVec2 max){
  const int ultra=static_cast<int>(e.key);
  const float labelHeight=ImGui::GetTextLineHeight()+4*Scale();
  const ImVec2 top(min.x+3,min.y+3),bottom(max.x-3,max.y-labelHeight);
  if(ultra==2){
   auto* panel=ImGui::GetWindowDrawList();
   panel->AddRectFilled(top,bottom,IM_COL32(26,22,19,255));
   const float font=(std::max)(1.f,(std::min)((bottom.y-top.y)*.8f,(bottom.x-top.x)*.7f));
   const auto glyph=ImGui::GetFont()->CalcTextSizeA(font,FLT_MAX,0,"W");
   panel->AddText(ImGui::GetFont(),font,ImVec2((top.x+bottom.x-glyph.x)*.5f,(top.y+bottom.y-glyph.y)*.5f),palette::Ember,"W");
  }else ImageInRect(art?art->Ultra(pick.fighter,ultra):Missing(),top,bottom);
  DrawCardBadge(ImVec2(max.x-3,min.y+2),max.x-min.x-6,ultra==2?"W":ultra==1?"II":"I","ultra-badge",true);
  const bool saved=ultra==pick.ultra;
  const std::string label=ultra==2?e.label:e.label+": "+FindFighter(pick.fighter)->ultras[ultra];
  auto* d=ImGui::GetWindowDrawList();
  d->AddRectFilled(ImVec2(min.x,max.y-labelHeight),max,IM_COL32(16,15,14,230));
  const float font=(std::min)(ImGui::GetFontSize(),(max.x-min.x-6)*ImGui::GetFontSize()/(std::max)(1.f,ImGui::CalcTextSize(label.c_str()).x));
  d->AddText(ImGui::GetFont(),font,ImVec2(min.x+3,max.y-labelHeight),saved?palette::Ember:palette::Ivory,label.c_str());
  if(saved)DrawCardBadge(ImVec2(min.x+3,min.y+2),max.x-min.x-6,loc::T("selection.saved"),"saved-badge");
  return true;
 };
 if(screen=="roster"||screen=="stage"||pool||screen=="costumes"||screen=="colors")card=[&](const MenuEntry& e,ImVec2 min,ImVec2 max){
  if(e.id=="waiting"||e.wide)return false;
  const int id=static_cast<int>(e.key);
  const float labelHeight=ImGui::GetTextLineHeight()+4*Scale();
  const auto image=!art?Missing():screen=="roster"?art->Portrait(id):screen=="stage"||pool?art->Stage(id):
   art->Appearance(pick.fighter,screen=="costumes"?id:pick.costume,screen=="costumes"?PreviewColor(pick.fighter,id,availability,installed):id);
  ImageInRect(image,ImVec2(min.x+3,min.y+3),ImVec2(max.x-3,max.y-labelHeight));
  const bool saved=!pool&&(screen=="roster"?id==pick.fighter:screen=="costumes"?id==pick.costume:screen=="colors"?id==pick.color:stageId&&id==*stageId);
  ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(min.x,max.y-labelHeight),max,IM_COL32(16,15,14,230));
  const float font=(std::min)(ImGui::GetFontSize(),(max.x-min.x-6)*ImGui::GetFontSize()/(std::max)(1.f,ImGui::CalcTextSize(e.label.c_str()).x));
  ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(),font,ImVec2(min.x+3,max.y-labelHeight),saved?palette::Ember:palette::Ivory,e.label.c_str());
  if(saved)DrawCardBadge(ImVec2(min.x+3,min.y+2),max.x-min.x-6,loc::T("selection.saved"),"saved-badge");
  if(screen=="roster"&&!saved&&readAvailability&&FighterLocked(id,readAvailability(id))){
   ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(min.x+3,min.y+3),ImVec2(max.x-3,max.y-labelHeight),IM_COL32(16,15,14,170));
   DrawCardBadge(ImVec2(min.x+3,min.y+2),max.x-min.x-6,loc::T("selection.not_owned_badge"),"not-owned-badge");
  }
  if(pool&&randomPool&&!randomPool->Contains(id)){
   // A skipped stage reads as switched off at a glance: dimmed, and labelled.
   ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(min.x+3,min.y+3),ImVec2(max.x-3,max.y-labelHeight),IM_COL32(16,15,14,170));
   DrawCardBadge(ImVec2(min.x+3,min.y+2),max.x-min.x-6,loc::T("selection.random_pool_skipped"),"skipped-badge");
  }
  return true;
 };
 // What the status line promises depends on the page: Select saves a card on
 // the galleries, opens a page from the menus, and does nothing on the option rows.
 const bool selectSaves=screen=="roster"||screen=="costumes"||screen=="colors"||screen=="ultra"||screen=="stage"||pool;
 const std::string status=!selectionError.empty()?selectionError:
  loc::T(!editable?"selection.status_locked":selectSaves?"selection.status_editable":
   screen=="home"||screen=="appearance"?"selection.status_browse":"selection.status_adjust");
 GameMenu::Body body;
 if(board)body=[&](const std::vector<MenuEntry>& entries,MenuNavigation& navigation,MenuAction& action,float height,const MenuVisualFeedback& feedback){
  DrawBoard(entries,navigation,action,height,feedback,pick,art,readAvailability);};
 const auto a=menu_.Draw(title.c_str(),rows,status.c_str(),preview,columns,card,body,false,
  screen=="costumes"||screen=="colors"?180.f:cardHeight,selectionError.empty()?Tone::Neutral:Tone::Error);
 if(a.kind==MenuAction::Close||a.kind==MenuAction::Shortcut)ForwardMenuAction(a);
 // Opened for one change, Back from its page goes back to where it came from.
 if(!openOn_.empty()&&a.kind==MenuAction::Returned&&screen==openOn_)ForwardMenuAction({MenuAction::Close});
 // A pick is done: back to where fighter select was opened from.
 const auto finish=[&]{if(!openOn_.empty())ForwardMenuAction({MenuAction::Close});else nav.Home();};
 if(a.kind==MenuAction::Activate){
  if(a.id=="ready")ForwardMenuAction({MenuAction::Close});
  else if((screen=="home"&&a.id.rfind("fighter-",0)!=0)||screen=="appearance"||a.id=="random-pool")nav.Push(a.id);
  else if(editable&&randomPool&&a.id.compare(0,5,"pool-")==0){
   // The page disables the last game stage left; the pool refuses it too.
   if(randomPool->Toggle(std::stoi(a.id.substr(5))))changed=true;
  }
  else if(editable&&a.id.compare(0,8,"fighter-")==0){
   pick.fighter=std::stoi(a.id.substr(8));const auto next=readAvailability?readAvailability(pick.fighter):Availability{};
   Normalize(pick,editionSelect,&next,installed);changed=true;
   // The Ultra comes next, when the fighter has more than one.
   // The Ultra comes next; the next frame drops the step if this fighter,
   // with its own saved pick restored, has only one.
   // On the board the Ultra is a row beside the grid.
   if(!board){nav.Push("ultra");nav.Prefer("ultra-"+std::to_string(pick.ultra));focusSavedUltra_=ultraStepPending_=true;}
  }else if(editable&&a.id.compare(0,6,"ultra-")==0){pick.ultra=std::stoi(a.id.substr(6));changed=true;finish();}
  else if(editable&&a.id.compare(0,8,"costume-")==0){
   // A costume's colors come next, from the color the pick keeps.
   pick.costume=std::stoi(a.id.substr(8));Normalize(pick,editionSelect,&availability,installed);changed=true;
   nav.Push("colors");nav.Prefer("color-"+std::to_string(pick.color));
  }else if(editable&&a.id.compare(0,6,"color-")==0){pick.color=std::stoi(a.id.substr(6));changed=true;finish();}
  else if(editable&&stageId&&a.id.compare(0,6,"stage-")==0){
   *stageId=std::stoi(a.id.substr(6));changed=true;
   // Opened for this one change, the pick is it; from its own menu the stage page stays.
   if(!openOn_.empty())finish();
  }
 }else if(editable&&a.kind==MenuAction::Adjust){
  if(a.id=="ultra")Step(pick.ultra,AllowedUltras(pick.fighter,pick.edition),a.delta);
  else if(a.id=="costume")Step(pick.costume,AllowedCostumes(pick.fighter,availability,installed),a.delta);
  else if(a.id=="color")Step(pick.color,AllowedColors(pick.fighter,pick.costume,availability,installed),a.delta);
  else if(a.id=="edition")Step(pick.edition,AllowedEditions(pick.fighter,editionSelect),a.delta);
  else if(a.id=="action")Step(pick.personalAction,AllowedPersonalActions(availability),a.delta);
  else if(a.id=="quote")Step(pick.winQuote,{255,0,1,2,3,4,5,6,7,8,9,10},a.delta);
  else if(a.id=="handicap")Step(pick.handicap,{0,1,2,3,4},a.delta);
  Normalize(pick,editionSelect,&availability,installed);changed=true;
 }
 return changed;
}

void FighterSelector::DrawBoard(const std::vector<MenuEntry>& rows,MenuNavigation& nav,MenuAction& action,float height,const MenuVisualFeedback& feedback,
 const selection::Pick& pick,SelectionArt* art,const AvailabilityReader& readAvailability) {
 using namespace selection;
 const float s=Scale(),width=ImGui::GetContentRegionAvail().x,font=ImGui::GetFontSize(),gap=6*s,rowHeight=48*s,valueWidth=220*s;
 const auto& c=v2::Colors();
 auto* d=ImGui::GetWindowDrawList();
 const auto& focus=nav.Focus();
 const bool changed=focus!=boardFocus_;
 boardFocus_=focus;
 // A box: its value's ends step it, anything else on it is Select; the moving pointer focuses it.
 const auto press=[&](const MenuEntry& e,const v2::Box& b){return PressEntry(nav,rows,e,b,action,changed,valueWidth).hovered;};
 const auto origin=ImGui::GetCursorScreenPos();
 const bool narrow=v2::Narrow(ImGui::GetWindowWidth(),s);
 const std::size_t cells=MenuGridCells(rows),lines=(cells+BoardColumns-1)/BoardColumns;
 const float paneWidth=narrow?width:(std::min)(420*s,width*.42f),gridWidth=narrow?width:width-paneWidth-24*s;
 const float cw=(gridWidth-gap*(BoardColumns-1))/BoardColumns,ch=(std::max)(52*s,(std::min)(cw,(height-gap*(lines-1))/(std::max<std::size_t>)(1,lines)));
 const float label=ImGui::GetTextLineHeight()+4*s;
 // The grid: each fighter's portrait and name; the saved one ruled, one not owned dimmed.
 for(std::size_t i=0;i<cells;++i){
  const auto& e=rows[i];
  const int id=std::stoi(e.id.substr(8));
  const float x=origin.x+(i%BoardColumns)*(cw+gap),y=origin.y+(i/BoardColumns)*(ch+gap);
  const v2::Box b{ImVec2(x,y),ImVec2(x+cw,y+ch)};
  const bool hovered=press(e,b),focused=e.id==focus;
  v2::CardGround(d,b,focused,hovered,id==pick.fighter);
  ImageInRect(art?art->Portrait(id):Missing(),ImVec2(b.min.x+3*s,b.min.y+3*s),ImVec2(b.max.x-3*s,b.max.y-label));
  if(readAvailability&&FighterLocked(id,readAvailability(id)))d->AddRectFilled(b.min,b.max,v2::color::Alpha(c.ink,.65f),4*s);
  d->AddRectFilled(ImVec2(b.min.x,b.max.y-label),b.max,v2::color::Alpha(c.ink,.85f));
  std::string elided;
  DrawCardText(elided,ImVec2(b.min.x+4*s,b.max.y-label+2*s),cw-8*s,e.label,(std::min)(font,15*s),id==pick.fighter?c.accent:c.text,true);
  if(focused)v2::Focus(d,b.min,b.max,false);
 }
 // The pane: the preview, the value rows, Ready at its foot (under the grid when narrow).
 const v2::Box pane{ImVec2(narrow?origin.x:origin.x+gridWidth+24*s,narrow?origin.y+lines*(ch+gap)+10*s:origin.y),ImVec2(origin.x+width,origin.y+height)};
 const std::size_t values=rows.size()-cells-1;
 const float readyHeight=48*s,stack=values*(rowHeight+gap)+readyHeight+12*s;
 const float preview=narrow?0:(std::min)(300*s,pane.H()-stack-gap);
 float y=pane.min.y;
 if(preview>=120*s){
  // The focused fighter's portrait, else the saved fighter in their costume and colour.
  const v2::Box b{pane.min,ImVec2(pane.max.x,pane.min.y+preview)};
  const int shown=focus.rfind("fighter-",0)==0?std::stoi(focus.substr(8)):pick.fighter;
  d->AddRectFilled(b.min,b.max,c.panel,4*s);
  ImageInRect(!art?Missing():shown==pick.fighter?art->Appearance(pick.fighter,pick.costume,pick.color):art->Portrait(shown,true),b.min,b.max);
  d->AddRect(b.min,b.max,c.border,4*s,0,s);
  d->AddRectFilledMultiColor(ImVec2(b.min.x,b.max.y-48*s),b.max,0,0,c.ink,c.ink);
  d->AddText(HeadingFont(),22*s,ImVec2(b.min.x+12*s,b.max.y-34*s),c.text,FindFighter(shown)->name);
  if(shown==pick.fighter){
   const std::string summary=ShortAppearanceLabel(pick)+" · "+ShortUltraLabel(pick.ultra);
   d->AddText(ImVec2(b.max.x-12*s-ImGui::CalcTextSize(summary.c_str()).x,b.max.y-30*s),c.text,summary.c_str());
  }
  y=b.max.y+gap;
 }
 for(std::size_t i=cells;i<rows.size();++i){
  const auto& e=rows[i];
  const bool focused=e.id==focus,enabled=feedback.Enabled(e);
  if(e.id=="ready"){
   // Ready: the Ember button at the pane's foot.
   const float top=narrow?y+6*s:(std::max)(y+6*s,pane.max.y-readyHeight);
   const v2::Box b{ImVec2(pane.min.x,top),ImVec2(pane.max.x,top+readyHeight)};
   v2::Button(d,b,e.label,v2::ButtonKind::Primary,focused,press(e,b),enabled);
   y=b.max.y;
   continue;
  }
  const v2::Box r{ImVec2(pane.min.x,y),ImVec2(pane.max.x,y+rowHeight)};
  y+=rowHeight+gap;
  const bool hovered=press(e,r);
  v2::CardGround(d,r,focused,hovered);
  DrawMenuRow(d,e,r,enabled,valueWidth,14*s);
  if(focused){
   v2::Focus(d,r.min,r.max);
   if(!e.choices.empty())menu_.bodyChoices={r,pane};
  }
 }
 ImGui::SetCursorScreenPos(ImVec2(origin.x,(std::max)(y,origin.y+lines*(ch+gap)-gap)));
 ImGui::Dummy(ImVec2(width,0));
}
} }
