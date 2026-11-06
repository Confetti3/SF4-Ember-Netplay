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
void ImageInRect(const SelectionImage& image, ImVec2 min, ImVec2 max) {
    if (!image.texture || !image.width || !image.height) {
        const char* label=loc::T(image.missing?"selection.preview_unavailable":"selection.preview_loading");
        const auto size=ImGui::CalcTextSize(label);
        const float factor=(std::max)(0.f,(std::min)(1.f,(std::min)((max.x-min.x-8*Scale())/(std::max)(1.f,size.x),
            (max.y-min.y-8*Scale())/(std::max)(1.f,size.y))));
        ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(),ImGui::GetFontSize()*factor,
            ImVec2((min.x+max.x-size.x*factor)*.5f,(min.y+max.y-size.y*factor)*.5f),palette::Muted,label);
        return;
    }
    const float factor = (std::min)((max.x - min.x) / image.width, (max.y - min.y) / image.height);
    const ImVec2 size(image.width * factor, image.height * factor);
    const ImVec2 start(min.x + (max.x - min.x - size.x) / 2, min.y + (max.y - min.y - size.y) / 2);
    ImGui::GetWindowDrawList()->AddImage(image.texture, start, ImVec2(start.x + size.x, start.y + size.y), image.uvMin, image.uvMax);
}
namespace {
using palette::Ember;
using palette::Ivory;
using palette::Muted;

// The page fighter select was opened on for one change, whose Back hands Close
// back to the parent; none for the roster, which is the board itself.
const char* StartPage(SelectionStart start) {
    switch (start) {
    case SelectionStart::LookUltra: return "look";
    case SelectionStart::Costumes: return "costumes";
    case SelectionStart::Ultras: return "ultras";
    case SelectionStart::Stage: return "stage";
    case SelectionStart::Options: return "options";
    case SelectionStart::Board: case SelectionStart::Roster: break;
    }
    return nullptr;
}
// What an Ultra's command needs besides its input, in the player's language.
const char* UltraConditionText(selection::UltraCondition condition) {
    using C = selection::UltraCondition;
    switch (condition) {
    case C::None: break;
    case C::Ground: return loc::T("selection.ultra_condition.ground");
    case C::AntiAir: return loc::T("selection.ultra_condition.anti_air");
    case C::GroundOrAir: return loc::T("selection.ultra_condition.ground_or_air");
    case C::InAir: return loc::T("selection.ultra_condition.in_air");
    case C::CloseRange: return loc::T("selection.ultra_condition.close_range");
    case C::Counter: return loc::T("selection.ultra_condition.counter");
    case C::MantisStance: return loc::T("selection.ultra_condition.mantis_stance");
    case C::CraneStance: return loc::T("selection.ultra_condition.crane_stance");
    case C::CraneStanceOrAir: return loc::T("selection.ultra_condition.crane_stance_or_air");
    case C::PunchStart: return loc::T("selection.ultra_condition.punch_start");
    case C::KickStart: return loc::T("selection.ultra_condition.kick_start");
    case C::Forward: return loc::T("selection.ultra_condition.forward");
    case C::DelayOrCancel: return loc::T("selection.ultra_condition.delay_or_cancel");
    case C::StopHealing: return loc::T("selection.ultra_condition.stop_healing");
    }
    return "";
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
    draw->AddRectFilled(ImVec2(min.x + 1, max.y - labelHeight), ImVec2(max.x - 1, max.y - 1), palette::InkAt(235));
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
// The Random card's picture, as USFIV's own Random box: a large question mark.
void RandomMark(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 color) {
    const float size = (std::min)(max.x - min.x, max.y - min.y) * .7f;
    const auto text = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, "?");
    draw->AddText(ImGui::GetFont(), size, ImVec2((min.x + max.x - text.x) / 2, (min.y + max.y - text.y) / 2), color, "?");
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
std::vector<UltraInput> UltraInputs(int fighter, int ultra, int edition) {
    std::vector<UltraInput> inputs;
    const auto allowed = selection::AllowedUltras(fighter, edition);
    if (std::find(allowed.begin(), allowed.end(), ultra) == allowed.end()) return inputs;
    const std::string charge = loc::T("selection.ultra_charge");
    // Ultra Double has no input of its own: it is Ultra I's and Ultra II's.
    for (const int one : ultra == 2 ? std::vector<int>{0, 1} : std::vector<int>{ultra})
        for (const auto& command : selection::UltraCommands(fighter, one, edition)) {
            std::string words = selection::UltraNotation(command.symbols, charge);
            if (command.condition != selection::UltraCondition::None)
                words = loc::Tf("selection.ultra_input_condition", UltraConditionText(command.condition), words);
            inputs.push_back({one, command.symbols, std::move(words)});
        }
    return inputs;
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


bool FighterSelector::Draw(selection::Pick& pick,bool editionSelect,SelectionArt* art,const AvailabilityReader& readAvailability,int* stageId,bool editable,const std::string& selectionError,selection::RandomPool* randomPool,const selection::InstalledContent* installed,bool fighterKept,bool* random) {
 using namespace selection;
 bool changed=false;auto& nav=menu_.navigation;
 // The card the saved pick is: Random's, or the fighter's.
 const bool randomSaved=random&&*random;
 const std::string savedCard=randomSaved?"random-fighter":"fighter-"+std::to_string(pick.fighter);
 // A selector the shell has just opened starts on its board, not on the page
 // an earlier visit left it on; one it reshows resumes.
 // The parent says where Back from here goes and which of its shortcuts
 // this screen hands back to it.
 const auto& embedded=EmbeddedReturnContext();
 if(TakeEmbeddedFresh()){
  nav.Home();openOn_=embedded.openOn;
  // Opened for the roster (a room's Fighter, the post-match Character select),
  // a fighter picked is the change it was opened for, as a stage is.
  // The board is the grid; the costume and the Ultra are on the fighter's own page.
  switch(openOn_){
  case SelectionStart::Roster:nav.Prefer(savedCard);break;
  case SelectionStart::LookUltra:nav.Push("look");nav.Prefer("ultra");break;
  // Opened for the look or the Ultra, its grid shows at once, over the fighter's page.
  case SelectionStart::Costumes:nav.Push("look");nav.Prefer("costume");gallery_.Open(nav,AppearanceGallery::Page::Costumes,pick);break;
  case SelectionStart::Ultras:nav.Push("look");nav.Prefer("ultra");gallery_.Open(nav,AppearanceGallery::Page::Ultras,pick);break;
  case SelectionStart::Stage:if(stageId){nav.Push("stage");nav.Prefer("stage-"+std::to_string(*stageId));}else openOn_=SelectionStart::Board;break;
  case SelectionStart::Options:nav.Push("options");break;
  case SelectionStart::Board:break;
  }
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
 // The edition, on the board and on the options page alike: a kept fighter
 // keeps its edition too.
 const auto editionRow=[&]{
  return Value("edition",loc::T("selection.edition"),FindEdition(pick.edition)->name,
   fighterKept?loc::T("selection.winner_keeps_fighter"):editionSelect?adjusts:loc::T("selection.usfiv_rules"),editable&&editionSelect&&!fighterKept);
 };
 std::vector<MenuEntry> rows;
 std::string title=loc::T("selection.title");int columns=1;
 // Home is the grid, eight across and nothing beside it, so a player who
 // hears the menu moves in one place. Select on a fighter opens their own
 // page ("look"): costume, colour, Ultra, edition, stage, options and Ready
 // in one list. The stage, its Random pool and the options are pages too.
 const bool board=screen=="home",look=screen=="look",pool=screen=="random-pool",gallery=AppearanceGallery::Shows(screen);
 // Stage cards and the pool's cards are as many across as fit 220 px.
 const int stageColumns=(std::max)(2,(std::min)(4,static_cast<int>(ImGui::GetContentRegionAvail().x/(220*Scale()))));
 if(board){
  // A fighter the player's Steam licences do not cover keeps its place in the
  // grid, so the layout matches the game's, but cannot be saved. The pad moves
  // by where DrawBoard puts each card (MenuNavigation::Place), by its columns.
  for(const int id:RosterDisplayOrder){
   const bool owned=!readAvailability||!FighterLocked(id,readAvailability(id));
   const bool choosable=editable&&owned&&!fighterKept;
   // The saved fighter opens their page even where no other can be chosen.
   rows.push_back(Saving(Row("fighter-"+std::to_string(id),FindFighter(id)->name,
    !owned?loc::T("selection.not_owned"):fighterKept?loc::T("selection.winner_keeps_fighter"):locked,choosable||id==pick.fighter),"menu.hint.save_fighter",choosable));
  }
  // Random is the last card, where USFIV's own select has it. Saved, it opens
  // the rolled fighter's page as a saved fighter does.
  if(random){
   const bool choosable=editable&&!fighterKept;
   rows.push_back(Saving(Row("random-fighter",loc::T("selection.random_card"),fighterKept?loc::T("selection.winner_keeps_fighter"):
    editable?loc::T("selection.random_card.detail"):locked,choosable||randomSaved),"menu.hint.save_fighter",choosable));
  }
  columns=BoardColumns;
  // A focus from no row of the board (a first visit) starts on the saved card.
  if(std::none_of(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==nav.Focus();}))nav.Prefer(savedCard);
 }else if(look){
  title=randomSaved?loc::Tf("selection.random_fighter",FindFighter(pick.fighter)->name):std::string(FindFighter(pick.fighter)->name);
  // Words open every choice in a panel, a number keeps its arrows (OfferSteps).
  const auto offer=[&](MenuEntry e,const std::vector<int>& ids,const std::function<std::string(int)>& name){
   std::vector<std::string> labels;for(int id:ids)labels.push_back(name(id));
   OfferSteps(e,labels);e.wide=true;rows.push_back(std::move(e));
  };
  // The costume, the colour and the Ultra step in place too, and Select shows
  // them all as pictures (AppearanceGallery), each Ultra with its input.
  const auto opens=[&](MenuEntry e){e.opens=e.wide=true;rows.push_back(std::move(e));};
  opens(Value("costume",loc::T("selection.costume"),CostumeLabel(pick,installed),editable?loc::T("selection.costume_row.detail"):adjusts,editable&&availability.ready));
  const std::string colorName=installed&&selection::IsCustomColor(pick.color)?installed->Name(pick.fighter,pick.costume,pick.color):std::string();
  opens(Value("color",loc::T("selection.color_row"),colorName.empty()?std::to_string(pick.color+1):colorName,editable?loc::T("selection.color_row.detail"):adjusts,editable&&availability.ready));
  opens(Value("ultra",loc::T("selection.ultra_combo"),UltraName(pick.fighter,pick.ultra),editable?loc::T("selection.ultra_row.detail"):adjusts,editable&&AllowedUltras(pick.fighter,pick.edition).size()>1));
  offer(editionRow(),AllowedEditions(pick.fighter,editionSelect),
   [](int edition){return std::string(FindEdition(edition)->name);});
  rows.push_back(Row("stage",loc::T("selection.stage"),stageId?loc::T("selection.choose_battlefield"):loc::T("selection.p1_stage"),stageId!=nullptr));
  rows.back().value=stageId?StageLabel(*stageId,installed):std::string();
  rows.push_back(Row("options",loc::T("selection.additional_options"),loc::T("selection.additional_options.detail")));
  rows.push_back(Row("ready",loc::Tf("selection.ready_with",FindFighter(pick.fighter)->name),loc::T("selection.ready_detail")));
 }else if(gallery){
  title=AppearanceGallery::Title(screen);rows=gallery_.Build(screen,pick,availability,installed,editable);
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
  rows={editionRow(),
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
   const auto img=!art?Missing():screen=="stage"||pool?art->Stage(focusStage):look?art->Appearance(pick.fighter,pick.costume,pick.color):art->Portrait(pick.fighter,true);
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
  ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(min.x,max.y-labelHeight),max,palette::ShadeAt(230));
  const float font=(std::min)(ImGui::GetFontSize(),(max.x-min.x-6)*ImGui::GetFontSize()/(std::max)(1.f,ImGui::CalcTextSize(e.label.c_str()).x));
  ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(),font,ImVec2(min.x+3,max.y-labelHeight),saved?palette::Ember:palette::Ivory,e.label.c_str());
  if(saved)DrawCardBadge(ImVec2(min.x+3,min.y+2),max.x-min.x-6,loc::T("selection.saved"),"saved-badge");
  if(pool&&randomPool&&!randomPool->Contains(id)){
   // A skipped stage reads as switched off at a glance: dimmed, and labelled.
   ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(min.x+3,min.y+3),ImVec2(max.x-3,max.y-labelHeight),palette::ShadeAt(170));
   DrawCardBadge(ImVec2(min.x+3,min.y+2),max.x-min.x-6,loc::T("selection.random_pool_skipped"),"skipped-badge");
  }
  return true;
 };
 // What the status line promises depends on the page: Select saves a stage
 // card, opens a page from the board, and does nothing on the option rows;
 // the costume, colour and Ultra grids say their own.
 const std::string status=!selectionError.empty()?selectionError:
  loc::T(!editable?"selection.status_locked":fighterKept&&board?"selection.winner_keeps_fighter":gallery?AppearanceGallery::StatusKey(screen):
   screen=="stage"||pool?"selection.status_editable":board?"selection.status_browse":"selection.status_adjust");
 GameMenu::Body body;
 if(board)body=[&](const std::vector<MenuEntry>& entries,MenuNavigation& navigation,MenuAction& action,float height,const MenuVisualFeedback&){
  DrawBoard(entries,navigation,action,height,pick,randomSaved,art,readAvailability);};
 else if(gallery)body=[&](const std::vector<MenuEntry>& entries,MenuNavigation& navigation,MenuAction& action,float height,const MenuVisualFeedback&){
  gallery_.DrawBody(entries,navigation,action,height);};
 const auto a=menu_.Draw(title.c_str(),rows,status.c_str(),preview,columns,card,body,false,100,selectionError.empty()?Tone::Neutral:Tone::Error);
 if(a.kind==MenuAction::Close||a.kind==MenuAction::Shortcut)ForwardMenuAction(a);
 // Opened for one change, Back from its page goes back to where it came from.
 if(const char* page=StartPage(openOn_);page&&a.kind==MenuAction::Returned&&screen==page)ForwardMenuAction({MenuAction::Close});
 if(a.kind==MenuAction::Activate){
  if(a.id=="ready")ForwardMenuAction({MenuAction::Close});
  else if(a.id=="stage"||a.id=="options"||a.id=="random-pool")nav.Push(a.id);
  else if(const auto chosen=gallery_.Choose(a.id,nav,pick,availability,installed);chosen!=AppearanceGallery::Choice::Unhandled){
   // A colour saves its costume with it; opened for the look or the Ultra,
   // that is the change.
   if(chosen==AppearanceGallery::Choice::Saved){
    Normalize(pick,editionSelect,&availability,installed);changed=true;
    if(openOn_==SelectionStart::Costumes||openOn_==SelectionStart::Ultras)ForwardMenuAction({MenuAction::Close});
   }
  }
  else if(const auto pooled=RowKey<int>(a.id,"pool-");pooled&&editable&&randomPool){
   // The page disables the last game stage left; the pool refuses it too.
   if(randomPool->Toggle(*pooled))changed=true;
  }
  else if(a.id=="random-fighter"&&random){
   // Chosen, Random rolls its fighter (room::RandomFighterPick) and the board stays.
   if(!*random&&editable&&!fighterKept){
    *random=true;changed=true;
    if(openOn_==SelectionStart::Roster)ForwardMenuAction({MenuAction::Close});
   }else if(*random)nav.Push("look");
  }
  else if(const auto fighter=RowKey<int>(a.id,"fighter-");fighter&&editable&&!fighterKept&&*fighter<FighterCount){
   if(random)*random=false;
   pick.fighter=*fighter;const auto next=readAvailability?readAvailability(pick.fighter):Availability{};
   Normalize(pick,editionSelect,&next,installed);changed=true;
   if(openOn_==SelectionStart::Roster)ForwardMenuAction({MenuAction::Close});
   else nav.Push("look");
  }else if(RowKey<int>(a.id,"fighter-")){
   // The saved fighter where no other can be chosen (kept, or locked): their page.
   nav.Push("look");
  }else if(const auto stage=RowKey<int>(a.id,"stage-");stage&&editable&&stageId){
   *stageId=*stage;changed=true;
   // Opened for this one change, the pick is it: back to where it was opened from.
   // From the board the stage page stays.
   if(openOn_==SelectionStart::Stage)ForwardMenuAction({MenuAction::Close});
  }
 }else if(editable&&a.kind==MenuAction::Adjust){
  bool stepped=true;
  if(a.id=="ultra")Step(pick.ultra,AllowedUltras(pick.fighter,pick.edition),a.delta);
  else if(a.id=="costume")Step(pick.costume,AllowedCostumes(pick.fighter,availability,installed),a.delta);
  else if(a.id=="color")Step(pick.color,AllowedColors(pick.fighter,pick.costume,availability,installed),a.delta);
  else if(a.id=="edition"&&!fighterKept)Step(pick.edition,AllowedEditions(pick.fighter,editionSelect),a.delta);
  else if(a.id=="action")Step(pick.personalAction,AllowedPersonalActions(availability),a.delta);
  else if(a.id=="quote")Step(pick.winQuote,{255,0,1,2,3,4,5,6,7,8,9,10},a.delta);
  else if(a.id=="handicap")Step(pick.handicap,{0,1,2,3,4},a.delta);
  else stepped=false;
  if(stepped){Normalize(pick,editionSelect,&availability,installed);changed=true;}
 }
 return changed;
}

void FighterSelector::DrawBoard(const std::vector<MenuEntry>& rows,MenuNavigation& nav,MenuAction& action,float height,
 const selection::Pick& pick,bool randomSaved,SelectionArt* art,const AvailabilityReader& readAvailability) {
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
 const std::size_t cells=MenuGridCells(rows),lines=(cells+BoardColumns-1)/BoardColumns;
 const float cw=(width-gap*(BoardColumns-1))/BoardColumns,ch=(std::max)(52*s,(std::min)(cw,(height-gap*(lines-1))/(std::max<std::size_t>)(1,lines)));
 const float label=ImGui::GetTextLineHeight()+4*s;
 // The grid: each fighter's portrait and name, then the Random card; the saved
 // card ruled (Random's, not the fighter it rolled), a fighter not owned dimmed.
 for(std::size_t i=0;i<cells;++i){
  const auto& e=rows[i];
  const auto fighter=RowKey<int>(e.id,"fighter-");
  const bool randomCard=e.id=="random-fighter";
  if(!fighter&&!randomCard)continue;
  const float x=origin.x+(i%BoardColumns)*(cw+gap),y=origin.y+(i/BoardColumns)*(ch+gap);
  const v2::Box b{ImVec2(x,y),ImVec2(x+cw,y+ch)};
  const bool hovered=press(e,b),focused=e.id==focus,saved=randomCard?randomSaved:!randomSaved&&*fighter==pick.fighter;
  v2::CardGround(d,b,focused,hovered,saved);
  if(randomCard)RandomMark(d,b.min,ImVec2(b.max.x,b.max.y-label),saved?c.accent:c.text);
  else ImageInRect(art?art->Portrait(*fighter):Missing(),ImVec2(b.min.x+3*s,b.min.y+3*s),ImVec2(b.max.x-3*s,b.max.y-label));
  if(fighter&&readAvailability&&FighterLocked(*fighter,readAvailability(*fighter)))d->AddRectFilled(b.min,b.max,v2::color::Alpha(c.ink,.65f),4*s);
  d->AddRectFilled(ImVec2(b.min.x,b.max.y-label),b.max,v2::color::Alpha(c.ink,.85f));
  std::string elided;
  DrawCardText(elided,ImVec2(b.min.x+4*s,b.max.y-label+2*s),cw-8*s,e.label,(std::min)(font,15*s),saved?c.accent:c.text,true);
  if(focused)v2::Focus(d,b.min,b.max,false);
 }
 ImGui::SetCursorScreenPos(ImVec2(origin.x,origin.y+lines*(ch+gap)-gap));
 ImGui::Dummy(ImVec2(width,0));
}
} }
