#include "TrainingPanelSf4.hxx"
#include "TrainingPanel.hxx"
#include "TrainingRemote.hxx"
#include "MenuRows.hxx"
#include "RowId.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/Localization.hxx"
#include "../training/ComboCheck.hxx"
#include "../training/FighterMoves.hxx"
#include "../training/MoveInputs.hxx"
#include "../training/RemoteOptions.hxx"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

// SF4's own pages of the lab's controls (TrainingPanelSf4.hxx). The rest of
// the controls, the settings file and the HUD are TrainingPanel.cxx's.
namespace sf4e { namespace ui {
namespace {
using namespace training;
// The dummy's settings as the game's Training menu numbers them, and their
// names (the phone remote's table has both): 0 action, 1 guard, 2 counter hit,
// 3 quick stand, 4 either gauge.
const remote::DummyKind& DummyKindAt(int kind) {
    static const char* const ids[5]={"action","guard","counterHit","quickStand","super"};
    return remote::DummyKindOf(ids[kind]);
}
std::vector<int> DummyChoices(int kind) {
    std::vector<int> values;
    for(const auto& choice:DummyKindAt(kind).choices) values.push_back(choice.value);
    return values;
}
// Empty until the game's own setting has been read. The action's last three
// are the pause menu's own and are named but never picked here.
const char* DummyLabel(int kind,int value) {
    if(kind==0&&value>=4&&value<=6) return loc::T(value==4?"training.dummy.human":value==5?"training.record":"training.play");
    for(const auto& choice:DummyKindAt(kind).choices) if(choice.value==value) return loc::T(choice.key);
    return "";
}
// Left and Right step a setting through its choices.
int StepDummy(int kind,int value,int delta) {
    const auto& choices=DummyChoices(kind);
    const auto found=std::find(choices.begin(),choices.end(),value);
    if(found==choices.end()) return choices[0];
    // A jump of more than one (a value picked in its panel) lands where it was picked.
    const int n=static_cast<int>(choices.size());
    return choices[((static_cast<int>(found-choices.begin())+delta)%n+n)%n];
}
std::string FighterName(int fighter){const auto* f=selection::FindFighter(fighter);return f?f->name:std::to_string(fighter);}
// The moves the list offers, the same for every fighter first, then the
// dummy's own, one row per move ("pick-" and its index here) with its
// strengths as chips on the row (LP, MP, HP, EX). A move's name ends in its
// strength, or starts with EX, as the tables and the command list write it.
struct Pickable { std::string name,group; std::vector<std::pair<std::string,std::string>> strengths; };
void AddPick(std::vector<Pickable>& list,std::string name,const char* moves,const std::string& group) {
    std::string strength;
    if(name.compare(0,3,"EX ")==0) { strength="EX"; name.erase(0,3); }
    const auto space=name.rfind(' ');
    if(space!=std::string::npos&&space+1<name.size()&&name.find_first_not_of("LMHPK+",space+1)==std::string::npos) {
        const auto buttons=name.substr(space+1); name.erase(space);
        strength=buttons.find('+')==std::string::npos&&!strength.empty()?strength+" "+buttons:buttons; // a pair is EX by itself: "LK+MK"
    }
    for(auto& pick:list) if(pick.name==name&&pick.group==group) { pick.strengths.push_back({strength,moves}); return; }
    list.push_back({std::move(name),group,{{strength,moves}}});
}
std::vector<Pickable> Pickables(const training::View& view) {
    std::vector<Pickable> list;
    for(const auto& move:training::UniversalMoves) AddPick(list,move.name,move.moves,loc::T("training.reply.every_fighter"));
    const auto* fighter=selection::FindFighter(view.check.dummy);
    for(const auto* move:training::MovesOf(fighter?fighter->code:nullptr)) AddPick(list,move->name,move->moves,FighterName(view.check.dummy));
    return list;
}
// A pick's ways to hold its charge (combo::ChargeHolds); none when it is no single charge move.
std::vector<combo::Step> ChargeHolds(const std::string& moves) {
    combo::Step step; std::string error;
    return combo::ParseStep(moves,step,error)?combo::ChargeHolds(step):std::vector<combo::Step>{};
}
// Combo check's start and stance, in the order Left and Right step through them.
const char* const StartNames[]={"training.combo_check.start_from.recorded","training.combo_check.start_from.middle","training.combo_check.start_from.corner"};
const char* const StanceNames[]={"training.combo_check.stance.standing","training.combo_check.stance.crouching","training.combo_check.stance.both"};

class Sf4Pages final : public TrainingExtraPages {
public:
    bool Offers(const std::string& screen) const override {
        if(screen=="remote") return TrainingRemoteOffered();
        return screen=="dummy"||screen=="reply-slot"||screen=="combo-check"||screen=="check-fighters";
    }
    bool Rows(const training::View& view,const std::string& screen,bool ready,std::vector<MenuEntry>& rows) override {
        if(screen=="remote"&&TrainingRemoteOffered()) { rows=TrainingRemoteRows(); return true; }
        if(screen=="dummy") { rows=DummyRows(view); return true; }
        if(screen=="reply-slot") { rows=SlotRows(view); return true; }
        if(screen=="combo-check"||screen=="check-fighters") { rows=ComboCheckRows(view,screen,ready); return true; }
        return false;
    }
    TrainingPageAnswer Handle(const MenuAction& a,const training::View& view,const std::string& screen,const TrainingSubmit& submit,std::string& error) override {
        TrainingPageAnswer answer; answer.taken=true;
        if(screen=="remote"&&TrainingRemoteOffered()) { HandleTrainingRemote(a,error); return answer; }
        if(screen=="dummy"||screen=="reply-slot"||screen=="reply") { HandleTools(a,view,submit); return answer; }
        if(a.kind==MenuAction::Activate&&a.id=="check-fighters") { TrainingNavigation().Push(a.id); return answer; }
        if((screen=="combo-check"||screen=="check-fighters")&&HandleComboCheck(a)) { SaveTrainingLab(); return answer; }
        answer.taken=false;
        if(a.kind!=MenuAction::Activate&&a.kind!=MenuAction::Adjust) return answer;
        Command command; command.generation=view.generation;
        if(a.id=="check-record"&&a.kind==MenuAction::Activate) command.action=Action::ComboRecord;
        else if(a.id=="check-start") { command.action=Action::ComboStart; command.check=check_; }
        else if(a.id=="check-stop") command.action=Action::ComboStop;
        else return answer;
        answer.taken=true; answer.send=command;
        // Back to the fight for a combo or a check.
        answer.returnAfter=command.action==Action::ComboRecord||command.action==Action::ComboStart;
        return answer;
    }
    bool Values(const MenuEntry& e,std::vector<std::string>& labels) override {
        if(e.id.compare(0,6,"dummy-")==0) {
            const std::string which=e.id.substr(6);
            const int kind=which=="action"?0:which=="guard"?1:which=="counter"?2:which=="quick"?3:4;
            for(const int value:DummyChoices(kind)) labels.push_back(DummyLabel(kind,value));
        }
        else if(e.id=="slot-weight") for(int w=1;w<=training::MaxReplyWeight;++w) labels.push_back(std::to_string(w));
        else if(e.id=="check-start-from") for(const char* name:StartNames) labels.push_back(loc::T(name));
        else if(e.id=="check-stance") for(const char* name:StanceNames) labels.push_back(loc::T(name));
        else if(e.id=="check-speed") labels={loc::T("training.combo_check.speed.normal"),loc::T("training.combo_check.speed.fast")};
        else return false;
        return true;
    }
    GameMenu::ListCard ListCard(const std::string& screen) override { return TrainingRemoteCard(screen=="remote"); }
    // Combo check: fighters it does not know are dropped, in the roster's order.
    void Read(const nlohmann::json& practice) override {
        const auto check=practice.value("combo_check",nlohmann::json());
        if(!check.is_object()) return;
        const auto fighters=check.value("fighters",nlohmann::json());
        if(fighters.is_array()) {
            check_.fighters.clear();
            for(const int id:selection::RosterDisplayOrder)
                if(std::find(fighters.begin(),fighters.end(),nlohmann::json(id))!=fighters.end()) check_.fighters.push_back(id);
        }
        check_.standing=check.value("standing",nlohmann::json(true))!=false;
        check_.crouching=check.value("crouching",nlohmann::json(true))!=false;
        if(!check_.standing&&!check_.crouching) check_.standing=check_.crouching=true;
        const auto start=check.value("start",nlohmann::json(0));
        if(start.is_number_integer()&&start>=0&&start<=2) check_.start=static_cast<training::combo_check::Start>(start.get<int>());
        check_.fast=check.value("fast",nlohmann::json(false))==true;
    }
    void Write(nlohmann::json& practice) const override {
        practice["combo_check"]={{"fighters",check_.fighters},{"standing",check_.standing},{"crouching",check_.crouching},
            {"start",static_cast<int>(check_.start)},{"fast",check_.fast}};
    }
    training::combo_check::Settings& Check() { return check_; }
private:
    // Combo check's choices (training/ComboCheck.hxx); every fighter at first.
    training::combo_check::Settings check_=[]{
        training::combo_check::Settings settings;
        settings.fighters.assign(std::begin(selection::RosterDisplayOrder),std::end(selection::RosterDisplayOrder));
        return settings;
    }();
    // The reply slot the move list is open for, and lit, for each of its rows
    // (Pickables), the chip lit: [0] the strength, [1] the way the charge row
    // under a charge move holds it; the first until stepped.
    int slotAt_=0; std::vector<std::array<int,2>> lit_;
    // The chip a pick row (which 0) or its charge row (1) has lit, kept while the move list is open.
    std::size_t LitAt(std::size_t row,int which,std::size_t count) {
        if(lit_.size()<=row) lit_.resize(row+1,{{0,0}});
        return static_cast<std::size_t>(lit_[row][which])%count;
    }
    // A pick row's moves as its chips have them: the lit strength, held the lit way.
    std::string PickedMoves(const Pickable& pick,std::size_t row) {
        const std::string& moves=pick.strengths[LitAt(row,0,pick.strengths.size())].second;
        const auto holds=ChargeHolds(moves);
        return holds.empty()?moves:combo::Canonical(holds[LitAt(row,1,holds.size())]);
    }
    std::vector<MenuEntry> DummyRows(const training::View& view) const {
        const auto& dummy=view.dummy;
        return {
            Value("dummy-action",loc::T("training.dummy.action"),DummyLabel(0,dummy.action),loc::T("training.dummy.detail")),
            Value("dummy-guard",loc::T("training.dummy.guard"),DummyLabel(1,dummy.guard),loc::T("training.dummy.detail")),
            Value("dummy-counter",loc::T("training.dummy.counter"),DummyLabel(2,dummy.counterHit),loc::T("training.dummy.detail")),
            Value("dummy-quick",loc::T("training.dummy.quick"),DummyLabel(3,dummy.quickStand),loc::T("training.dummy.detail")),
            Value("dummy-super",loc::T("training.dummy.super"),DummyLabel(4,dummy.super),loc::T("training.dummy.detail")),
            Value("dummy-revenge",loc::T("training.dummy.revenge"),DummyLabel(4,dummy.revenge),loc::T("training.dummy.detail"))};
    }
    std::vector<MenuEntry> SlotRows(const training::View& view) {
        auto detailed=[](MenuEntry e,const char* detail){e.detail=detail;return e;};
        const auto& slot=TrainingLabState().slots[slotAt_];
        std::vector<MenuEntry> rows{
            Value("slot-on",loc::T("training.reply.slot_on"),loc::T(slot.on?"common.on":"common.off"),loc::T("training.reply.slot_on.detail"),!slot.moves.empty()),
            Value("slot-weight",loc::T("training.reply.weight"),std::to_string(slot.weight),loc::T("training.reply.weight.detail")),
            detailed(TextRow("slot-moves",loc::T("training.reply.moves"),slot.moves,256),loc::T("training.reply.moves.detail")),
            Row("slot-clear",loc::T("training.reply.clear_slot"),loc::T("training.reply.clear_slot.detail"),!slot.moves.empty())};
        // A move's strengths are chips on its row: Left and Right light one,
        // Select puts it in the slot. A move with one strength shows its input.
        const auto picks=Pickables(view);
        for(std::size_t i=0;i<picks.size();++i) {
            const auto& pick=picks[i]; const auto& lit=pick.strengths[LitAt(i,0,pick.strengths.size())];
            const auto moves=PickedMoves(pick,i);
            auto e=Row("pick-"+std::to_string(i),pick.name,pick.group+": "+pick.name+", "+(lit.first.empty()?"":lit.first+", ")+moves);
            if(pick.strengths.size()==1) e.value=moves;
            else { e.value=lit.first; e.adjustable=e.opens=true; for(const auto& strength:pick.strengths) e.segments.push_back(strength.first); }
            rows.push_back(e);
            // Its charge's ways to hold, as chips the same way.
            const auto holds=ChargeHolds(lit.second);
            if(holds.empty()) continue;
            auto c=Row("charge-"+std::to_string(i),loc::T("training.reply.charge"),pick.name+", "+loc::T("training.reply.charge")+", "+moves);
            c.value=moves; c.adjustable=c.opens=true; for(const auto& hold:holds) c.segments.push_back(combo::Canonical(hold));
            rows.push_back(c);
        }
        return rows;
    }
    // The dummy page, the reply slot's page and Select on a reply slot.
    void HandleTools(const MenuAction& a,const training::View& view,const TrainingSubmit& submit) {
        auto lab=TrainingLabState();
        if(a.kind==MenuAction::Adjust) {
            if(a.id=="slot-on") { auto& slot=lab.slots[slotAt_]; slot.on=a.delta>0&&!slot.moves.empty(); SaveTrainingLab(); SendTrainingPlan(view,submit); }
            else if(a.id=="slot-weight") { auto& slot=lab.slots[slotAt_]; slot.weight=(std::max)(1,(std::min)(training::MaxReplyWeight,slot.weight+a.delta)); SaveTrainingLab(); SendTrainingPlan(view,submit); }
            else if(a.id.compare(0,5,"pick-")==0||a.id.compare(0,7,"charge-")==0) {
                const int which=a.id[0]=='c'; const auto picks=Pickables(view); const auto at=static_cast<std::size_t>(std::atoi(a.id.c_str()+a.id.find('-')+1));
                if(at<picks.size()) {
                    const auto& strengths=picks[at].strengths;
                    const int n=static_cast<int>(which?ChargeHolds(strengths[LitAt(at,0,strengths.size())].second).size():strengths.size());
                    // A click on a chip lights that chip: the whole distance, not one step.
                    if(n) lit_[at][which]=((static_cast<int>(LitAt(at,which,n))+a.delta)%n+n)%n;
                }
            }
            else if(a.id.compare(0,6,"dummy-")==0) {
                const std::string which=a.id.substr(6);
                const auto& now=view.dummy;
                Command command; command.action=Action::DummyState; command.generation=view.generation;
                if(which=="action") command.dummy.action=StepDummy(0,now.action,a.delta);
                else if(which=="guard") command.dummy.guard=StepDummy(1,now.guard,a.delta);
                else if(which=="counter") command.dummy.counterHit=StepDummy(2,now.counterHit,a.delta);
                else if(which=="quick") command.dummy.quickStand=StepDummy(3,now.quickStand,a.delta);
                else if(which=="super") command.dummy.super=StepDummy(4,now.super,a.delta);
                else if(which=="revenge") command.dummy.revenge=StepDummy(4,now.revenge,a.delta);
                else return;
                if(!submit||!submit(command)) TrainingLabNotice(loc::T("training.command_rejected"),true);
            }
            return;
        }
        if(a.kind==MenuAction::TextAccepted&&a.id=="slot-moves") {
            training::ReplyChoice reply; std::string error; const auto text=combo::Clean(a.text);
            if(!BuildReply(text,reply,error)) { TrainingLabNotice(loc::Tf("training.invalid",error),true); return; }
            auto& slot=lab.slots[slotAt_]; slot.moves=text; slot.on=!text.empty();
            SaveTrainingLab();
            SendTrainingPlan(view,submit);
            return;
        }
        if(a.kind!=MenuAction::Activate) return;
        if(a.id.compare(0,11,"reply-slot-")==0) { slotAt_=std::atoi(a.id.c_str()+11); lit_.clear(); TrainingNavigation().Push("reply-slot"); }
        else if(a.id=="slot-clear") { lab.slots[slotAt_]={}; SaveTrainingLab(); SendTrainingPlan(view,submit); TrainingNavigation().Replace("reply"); }
        else if(a.id.compare(0,5,"pick-")==0||a.id.compare(0,7,"charge-")==0) {
            const auto picks=Pickables(view); const auto at=static_cast<std::size_t>(std::atoi(a.id.c_str()+a.id.find('-')+1));
            if(at>=picks.size()) return;
            auto& slot=lab.slots[slotAt_]; slot.moves=PickedMoves(picks[at],at); slot.on=true;
            SaveTrainingLab(); SendTrainingPlan(view,submit); TrainingNavigation().Replace("reply");
        }
    }
    bool Checked(int fighter) const { return std::find(check_.fighters.begin(),check_.fighters.end(),fighter)!=check_.fighters.end(); }
    // The fighters on, in the roster's order, so the runs go as the select screen reads.
    void CheckFighter(int fighter,bool on) {
        auto& list=check_.fighters;
        list.erase(std::remove(list.begin(),list.end(),fighter),list.end());
        if(!on) return;
        std::vector<int> ordered;
        for(const int id:selection::RosterDisplayOrder) if(id==fighter||Checked(id)) ordered.push_back(id);
        list=std::move(ordered);
    }
    int Stance() const { return check_.standing&&check_.crouching?2:check_.crouching?1:0; }
    std::vector<MenuEntry> ComboCheckRows(const training::View& v,const std::string& screen,bool ready) const {
        const auto onOff=[](bool on){return std::string(loc::T(on?"common.on":"common.off"));};
        std::vector<MenuEntry> rows;
        if(screen=="check-fighters") {
            rows.push_back(Row("check-all",loc::T("training.combo_check.all"),loc::T("training.combo_check.all.detail")));
            for(const int fighter:selection::RosterDisplayOrder)
                rows.push_back(Value("check-f-"+std::to_string(fighter),FighterName(fighter),onOff(Checked(fighter)),loc::T("training.combo_check.fighters.detail")));
            return rows;
        }
        const auto& c=v.check;
        const bool idle=ready&&v.mode==Mode::Idle&&!c.running;
        rows.push_back(Value("check-record",loc::T("training.combo_check.record"),
            c.recording?loc::T("training.combo_check.recording"):c.frames?loc::Tf("training.combo_check.hits",c.hits):loc::T("training.combo_check.none"),
            loc::T("training.combo_check.record.detail"),idle));
        rows.back().adjustable=false;
        rows.push_back(Value("check-start-from",loc::T("training.combo_check.start_from"),loc::T(StartNames[static_cast<int>(check_.start)]),loc::T("training.combo_check.start_from.detail"),!c.running));
        rows.push_back(Value("check-fighters",loc::T("training.combo_check.fighters"),loc::Tf("training.combo_check.fighters.count",check_.fighters.size(),selection::FighterCount),loc::T("training.combo_check.fighters.detail"),!c.running));
        rows.back().opens=true; rows.back().adjustable=false;
        rows.push_back(Value("check-stance",loc::T("training.combo_check.stance"),loc::T(StanceNames[Stance()]),loc::T("training.combo_check.stance.detail"),!c.running));
        rows.push_back(Value("check-speed",loc::T("training.combo_check.speed"),loc::T(check_.fast?"training.combo_check.speed.fast":"training.combo_check.speed.normal"),loc::T("training.combo_check.speed.detail"),!c.running));
        if(c.running) rows.push_back(Row("check-stop",loc::T("training.combo_check.stop"),loc::T("training.combo_check.stop.detail"),ready));
        else rows.push_back(Row("check-start",loc::T("training.combo_check.start"),loc::T("training.combo_check.start.detail"),idle&&c.frames>0&&!check_.fighters.empty()));
        // One row a run, as it is done: the fighter, the stance and the hits that landed.
        for(std::size_t i=0;i<c.runs.size();++i) {
            const auto& run=c.runs[i];
            const bool landed=combo_check::Landed(run,c.hits);
            rows.push_back(Value("check-run-"+std::to_string(i),loc::Tf(run.crouching?"training.combo_check.run.crouching":"training.combo_check.run.standing",FighterName(run.fighter)),
                run.done?loc::Tf("training.combo_check.run.hits",run.hits,c.hits):loc::T("training.combo_check.run.waiting"),
                run.done?loc::T(landed?"training.combo_check.run.landed":"training.combo_check.run.dropped"):""));
            rows.back().adjustable=false;
        }
        return rows;
    }
    // Left and Right on a setting; Select on a fighter or All. True when it was the check's.
    bool HandleComboCheck(const MenuAction& a) {
        const int step=a.delta>0?1:-1;
        if(a.kind==MenuAction::Adjust) {
            if(a.id=="check-start-from") check_.start=static_cast<training::combo_check::Start>((static_cast<int>(check_.start)+3+step)%3);
            else if(a.id=="check-stance") { const int s=(Stance()+3+step)%3; check_.standing=s!=1; check_.crouching=s!=0; }
            else if(a.id=="check-speed") check_.fast=a.delta>0;
            else if(const auto f=RowKey<int>(a.id,"check-f-")) CheckFighter(*f,a.delta>0);
            else return false;
            return true;
        }
        if(a.kind!=MenuAction::Activate) return false;
        if(const auto f=RowKey<int>(a.id,"check-f-")) { CheckFighter(*f,!Checked(*f)); return true; }
        if(a.id=="check-all") {
            const bool all=check_.fighters.size()==static_cast<std::size_t>(selection::FighterCount);
            check_.fighters.clear();
            if(!all) for(const int id:selection::RosterDisplayOrder) check_.fighters.push_back(id);
            return true;
        }
        return false;
    }
};
Sf4Pages& Pages() { static Sf4Pages pages; return pages; }
}
void InstallSf4TrainingPages() { SetTrainingExtraPages(&Pages()); }
TrainingLabParts TrainingLab() {
    const auto lab=TrainingLabState();
    return {lab.meter,lab.plan,Pages().Check(),lab.slots,lab.directory};
}
} }
