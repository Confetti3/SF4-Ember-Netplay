#pragma once
// Exercise installed content through the real selector in each render locale
// and viewport, including slots without bundled previews and long names.
#include "../common/InstalledContent.hxx"
#include "../common/StageCatalog.hxx"
#include "../ui/FighterSelector.hxx"
#include "../ui/MenuRows.hxx"
#include <algorithm>
namespace {
template<class Draw>
void ShootCustomContent(sf4e::ui::FighterSelector& selector, sf4e::selection::Pick& pick,
                        int& stage, sf4e::selection::RandomPool& pool,
                        const sf4e::selection::InstalledContent*& catalog, const Draw& draw) {
    using namespace sf4e; using namespace ui;
    const auto savedPick=pick; const int savedStage=stage; const auto savedPool=pool;
    selection::InstalledContent installed;
    installed.costumes[0]={7,98};
    installed.colors[{0,7}]={29,98};
    // Names as long as a board row shows whole at the narrowest size; the
    // stage's name is longer, for the stage cards.
    installed.names[{0,7,-1}]="Long installed costume name";
    installed.names[{0,7,98}]="Long installed color name";
    std::vector<std::int64_t> excluded;
    for(int i=0;i<100;++i){
        const std::string code="A"+std::to_string(100+i).substr(1);
        const int id=selection::CustomStageId(code.c_str());
        Require(id>=0,"Invalid custom stage fixture");
        installed.stages.push_back(id); excluded.push_back(id);
        if(i==99)installed.stageNames[id]=std::string(40,'W');
    }
    pool=selection::RandomPool(excluded); catalog=&installed;
    pick=selection::Pick{}; pick.fighter=0;
    std::vector<MenuEntry> rows;
    SetMenuEntriesProbe([&](const std::vector<MenuEntry>& entries){rows=entries;});
    const auto page=[&](const char* screen){selector.Navigation().Home();selector.Navigation().Push(screen);draw();};
    const auto focus=[&](const std::string& id){
        Require(std::any_of(rows.begin(),rows.end(),[&](const MenuEntry& row){return row.id==id;}),"Custom selector row missing");
        selector.Navigation().Focus(id,rows);draw();
    };
    // The board's costume panel names the installed costumes; Right steps the
    // row onto them, and the color row onto the installed colors.
    const auto step=[&](const char* id,const int& value,int target,const char* failure){
        selector.Navigation().Home();draw();focus(id);
        for(int i=0;i<40&&value!=target;++i){draw(nullptr,MenuInput::Right,1);draw(nullptr,0,1);}
        Require(value==target,failure);
    };
    selector.Navigation().Home();draw();focus("costume");
    Require(CostumeLabel(pick,&installed)==loc::T("selection.original"),"Custom fixture lost original costume");
    draw(nullptr,MenuInput::Select,1);draw("custom-costumes");
    Require(selector.Navigation().Choosing(),"The costume row did not open its values");
    // Select on the saved costume closes the panel and keeps it.
    draw(nullptr,MenuInput::Select,1);draw();
    Require(!selector.Navigation().Choosing()&&pick.costume==0,"Picking the saved costume changed it");
    step("costume",pick.costume,7,"Custom costume was not selected");
    Require(CostumeLabel(pick,&installed)=="Long installed costume name","Installed costume name was lost");
    step("color",pick.color,98,"Custom color was not selected");
    Require(pick.costume==7&&std::any_of(rows.begin(),rows.end(),[](const MenuEntry& row){return row.id=="color"&&row.value=="Long installed color name";}),
        "Stepping the color changed the custom costume, or lost the installed color's name");
    draw("custom-colors");
    step("costume",pick.costume,98,"Unnamed custom costume was not selected");
    Require(CostumeLabel(pick,&installed)==loc::Tf("selection.custom",99),"Unnamed custom costume lost its fallback label");
    draw("custom-costume-fallback");
    page("stage");
    Require(rows.size()==selection::VersusStageCount+102,"Custom stage gallery lost stages");
    const int last=installed.stages.back();focus("stage-"+std::to_string(last));draw("custom-stages");
    draw(nullptr,MenuInput::Select,1);draw();
    Require(stage==last&&StageLabel(stage,&installed)==std::string(40,'W'),"Custom stage selection or name was lost");
    Require(StageLabel(installed.stages.front(),&installed)==loc::Tf("selection.custom","A00"),"Unnamed custom stage lost its fallback label");
    page("random-pool");
    Require(rows.size()==selection::VersusStageCount+100&&pool.Excluded().size()==100,"Custom Random exclusions were truncated");
    focus("pool-"+std::to_string(last));draw("custom-random-pool");
    draw(nullptr,MenuInput::Select,1);draw();
    Require(pool.Contains(last)&&pool.Excluded().size()==99&&!pool.Contains(installed.stages.front()),"Random toggle changed other exclusions");
    draw(nullptr,MenuInput::Select,1);draw();
    Require(pool.Excluded().size()==100&&!pool.Contains(last),"Random toggle failed to restore exclusion");
    SetMenuEntriesProbe({}); catalog=nullptr; pick=savedPick; stage=savedStage; pool=savedPool;
    selector.Navigation().Home(); draw();
}
}
