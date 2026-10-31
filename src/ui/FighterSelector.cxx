#include "FighterSelector.hxx"
#include "Theme.hxx"
#include "RoomCards.hxx"
#include <functional>
#include "MenuRows.hxx"
#include "RowId.hxx"
#include "../common/Localization.hxx"
#include "../common/StageCatalog.hxx"
#include <algorithm>

namespace sf4e { namespace ui {
namespace {
using palette::Ember;
using palette::Ivory;
using palette::Muted;

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
 // A selector the shell has just opened starts on its board, not on the page
 // an earlier visit left it on; one it reshows resumes.
 // The parent says where Back from here goes and which of its shortcuts
 // this screen hands back to it.
 const auto& embedded=EmbeddedReturnContext();
 if(TakeEmbeddedFresh()){
  nav.Home();openOn_.clear();
  const auto& openOn=embedded.openOn;
  // The board has the fighter, the costume and the Ultra on one screen: it opens on that row.
  if(openOn=="roster"||openOn=="ultra"||openOn=="costumes")nav.Prefer(openOn=="roster"?"fighter-"+std::to_string(pick.fighter):openOn=="ultra"?"ultra":"costume");
  else if(openOn=="stage"&&stageId){nav.Push("stage");nav.Prefer("stage-"+std::to_string(*stageId));openOn_=openOn;}
  else if(openOn=="options"){nav.Push("options");openOn_=openOn;}
 }
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
 std::string title=loc::T("selection.title");int columns=1;
 // Home is one board (Fighter.dc.html), the grid eight across and beside it the
 // preview, the value rows and Ready; the stage, its Random pool and the
 // options are pages of their own.
 const bool board=screen=="home",pool=screen=="random-pool";
 // Stage cards and the pool's cards are as many across as fit 220 px.
 const int stageColumns=(std::max)(2,(std::min)(4,static_cast<int>(ImGui::GetContentRegionAvail().x/(220*Scale()))));
 if(board){
  // A fighter the player's Steam licences do not cover keeps its place in the
  // grid, so the layout matches the game's, but cannot be saved. The pad moves
  // by where DrawBoard puts each row (MenuNavigation::Place): the grid by its
  // columns, and across to the rows beside it where they are level.
  for(const int id:RosterDisplayOrder){
   const bool owned=!readAvailability||!FighterLocked(id,readAvailability(id));
   rows.push_back(Saving(Row("fighter-"+std::to_string(id),FindFighter(id)->name,owned?locked:loc::T("selection.not_owned"),editable&&owned),"menu.hint.save_fighter",editable&&owned));
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
  for(auto it=rows.begin()+FighterCount;it!=rows.end();++it)it->wide=true;
  columns=BoardColumns;
  // A focus from no row of the board (a first visit) starts on the saved fighter.
  if(std::none_of(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==nav.Focus();}))nav.Prefer("fighter-"+std::to_string(pick.fighter));
 }else if(screen=="stage"){
  title=loc::T("selection.stage_title");
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
  columns=stageColumns;
 }else if(pool){
  // Select takes a stage out of Random or puts it back; the last one stays.
  title=loc::T("selection.random_pool_title");
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
  columns=stageColumns;
 }else{
  title=loc::T("selection.options_title");
  rows={Value("edition",loc::T("selection.edition"),FindEdition(pick.edition)->name,editionSelect?adjusts:loc::T("selection.usfiv_rules"),editable&&editionSelect),
   Value("action",loc::T("selection.personal_action"),pick.personalAction==255?loc::T("common.none"):std::to_string(pick.personalAction+1),adjusts,editable&&availability.ready),
   Value("quote",loc::T("selection.win_quote"),pick.winQuote==255?loc::T("selection.random"):std::to_string(pick.winQuote+1),adjusts,editable),
   Value("handicap",loc::T("selection.handicap"),HandicapLabel(pick.handicap),adjusts,editable)};
 }
 // The pages' detail pane: the saved pick, and the focused stage's picture
 // (the saved fighter's portrait on the options page).
 const auto preview=[&](const std::string& id){
  int focusStage=stageId?*stageId:0;
  if(const auto n=RowKey<int>(id,pool?"pool-":"stage-"))focusStage=*n;
  ImGui::TextWrapped("%s",loc::Tf("selection.saved_full",FindFighter(pick.fighter)->name,CostumeLabel(pick,installed),pick.color+1).c_str());
  const float width=(std::min)(ImGui::GetContentRegionAvail().x,300*Scale());
  const float height=(std::min)(ImGui::GetContentRegionAvail().y-10*Scale(),250*Scale());
  if(height>35*Scale()){
   const auto p=ImGui::GetCursorScreenPos();ImGui::Dummy(ImVec2(width,height));
   const auto img=!art?Missing():screen=="stage"||pool?art->Stage(focusStage):art->Portrait(pick.fighter,true);
   ImageInRect(img,p,ImVec2(p.x+width,p.y+height));
  }
 };
 // A stage's card is its picture; the saved stage is marked, a skipped one dimmed.
 GameMenu::Card card;
 if(screen=="stage"||pool)card=[&](const MenuEntry& e,ImVec2 min,ImVec2 max){
  if(e.wide)return false;
  const int id=static_cast<int>(e.key);
  const float labelHeight=ImGui::GetTextLineHeight()+4*Scale();
  ImageInRect(art?art->Stage(id):Missing(),ImVec2(min.x+3,min.y+3),ImVec2(max.x-3,max.y-labelHeight));
  const bool saved=!pool&&stageId&&id==*stageId;
  ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(min.x,max.y-labelHeight),max,IM_COL32(16,15,14,230));
  const float font=(std::min)(ImGui::GetFontSize(),(max.x-min.x-6)*ImGui::GetFontSize()/(std::max)(1.f,ImGui::CalcTextSize(e.label.c_str()).x));
  ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(),font,ImVec2(min.x+3,max.y-labelHeight),saved?palette::Ember:palette::Ivory,e.label.c_str());
  if(saved)DrawCardBadge(ImVec2(min.x+3,min.y+2),max.x-min.x-6,loc::T("selection.saved"),"saved-badge");
  if(pool&&randomPool&&!randomPool->Contains(id)){
   // A skipped stage reads as switched off at a glance: dimmed, and labelled.
   ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(min.x+3,min.y+3),ImVec2(max.x-3,max.y-labelHeight),IM_COL32(16,15,14,170));
   DrawCardBadge(ImVec2(min.x+3,min.y+2),max.x-min.x-6,loc::T("selection.random_pool_skipped"),"skipped-badge");
  }
  return true;
 };
 // What the status line promises depends on the page: Select saves a stage
 // card, opens a page from the board, and does nothing on the option rows.
 const std::string status=!selectionError.empty()?selectionError:
  loc::T(!editable?"selection.status_locked":screen=="stage"||pool?"selection.status_editable":
   board?"selection.status_browse":"selection.status_adjust");
 GameMenu::Body body;
 if(board)body=[&](const std::vector<MenuEntry>& entries,MenuNavigation& navigation,MenuAction& action,float height,const MenuVisualFeedback& feedback){
  DrawBoard(entries,navigation,action,height,feedback,pick,art,readAvailability);};
 const auto a=menu_.Draw(title.c_str(),rows,status.c_str(),preview,columns,card,body,false,100,selectionError.empty()?Tone::Neutral:Tone::Error);
 if(a.kind==MenuAction::Close||a.kind==MenuAction::Shortcut)ForwardMenuAction(a);
 // Opened for one change, Back from its page goes back to where it came from.
 if(!openOn_.empty()&&a.kind==MenuAction::Returned&&screen==openOn_)ForwardMenuAction({MenuAction::Close});
 if(a.kind==MenuAction::Activate){
  if(a.id=="ready")ForwardMenuAction({MenuAction::Close});
  else if(a.id=="stage"||a.id=="options"||a.id=="random-pool")nav.Push(a.id);
  else if(const auto pooled=RowKey<int>(a.id,"pool-");pooled&&editable&&randomPool){
   // The page disables the last game stage left; the pool refuses it too.
   if(randomPool->Toggle(*pooled))changed=true;
  }
  else if(const auto fighter=RowKey<int>(a.id,"fighter-");fighter&&editable&&*fighter<FighterCount){
   pick.fighter=*fighter;const auto next=readAvailability?readAvailability(pick.fighter):Availability{};
   Normalize(pick,editionSelect,&next,installed);changed=true;
  }else if(const auto stage=RowKey<int>(a.id,"stage-");stage&&editable&&stageId){
   *stageId=*stage;changed=true;
   // Opened for this one change, the pick is it: back to where it was opened from.
   // From the board the stage page stays.
   if(!openOn_.empty())ForwardMenuAction({MenuAction::Close});
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
 const float s=Scale(),width=ImGui::GetContentRegionAvail().x,font=ImGui::GetFontSize(),gap=6*s;
 const auto& c=v2::Colors();
 auto* d=ImGui::GetWindowDrawList();
 const auto& focus=nav.Focus();
 const bool changed=focus!=boardFocus_;
 boardFocus_=focus;
 // A card or button: a press is Select on it; the moving pointer focuses it.
 const auto press=[&](const MenuEntry& e,const v2::Box& b){return PressEntry(nav,rows,e,b,action,changed).hovered;};
 const auto origin=ImGui::GetCursorScreenPos();
 const bool narrow=v2::Narrow(ImGui::GetWindowWidth(),s);
 const std::size_t cells=MenuGridCells(rows),lines=(cells+BoardColumns-1)/BoardColumns;
 const float paneWidth=narrow?width:(std::min)(420*s,width*.42f),gridWidth=narrow?width:width-paneWidth-24*s;
 const float cw=(gridWidth-gap*(BoardColumns-1))/BoardColumns,ch=(std::max)(52*s,(std::min)(cw,(height-gap*(lines-1))/(std::max<std::size_t>)(1,lines)));
 const float label=ImGui::GetTextLineHeight()+4*s;
 // The grid: each fighter's portrait and name; the saved one ruled, one not owned dimmed.
 for(std::size_t i=0;i<cells;++i){
  const auto& e=rows[i];
  const auto fighter=RowKey<int>(e.id,"fighter-");
  if(!fighter)continue;
  const int id=*fighter;
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
 // The value rows' height (a row is a line taller with its value under its label) and Ready's.
 float stack=48*s+12*s;
 for(std::size_t i=cells;i<rows.size();++i)if(rows[i].id!="ready")stack+=BoardRowHeight(rows[i],pane.W())+gap;
 const float readyHeight=48*s;
 const float preview=narrow?0:(std::min)(300*s,pane.H()-stack-gap);
 float y=pane.min.y;
 if(preview>=120*s){
  // The focused fighter's portrait, else the saved fighter in their costume and colour.
  const v2::Box b{pane.min,ImVec2(pane.max.x,pane.min.y+preview)};
  const int shown=RowKey<int>(focus,"fighter-").value_or(pick.fighter);
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
  const bool enabled=feedback.Enabled(e);
  if(e.id=="ready"){
   // Ready: the Ember button at the pane's foot.
   const float top=narrow?y+6*s:(std::max)(y+6*s,pane.max.y-readyHeight);
   const v2::Box b{ImVec2(pane.min.x,top),ImVec2(pane.max.x,top+readyHeight)};
   v2::Button(d,b,e.label,v2::ButtonKind::Primary,e.id==focus,press(e,b),enabled);
   y=b.max.y;
   continue;
  }
  // A value row card; its values open beside it, as wide as the board lets
  // them (the pane alone squeezes their cells).
  BoardRowStyle style;
  style.column={origin,ImVec2(origin.x+width,origin.y+height)};style.enabled=enabled;style.scroll=changed;style.card=true;
  y=menu_.DrawBoardRow(rows,e,ImVec2(pane.min.x,y),pane.W(),action,style).box.max.y+gap;
 }
 ImGui::SetCursorScreenPos(ImVec2(origin.x,(std::max)(y,origin.y+lines*(ch+gap)-gap)));
 ImGui::Dummy(ImVec2(width,0));
}
} }
