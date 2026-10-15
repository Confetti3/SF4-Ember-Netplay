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
    installed.names[{0,7,-1}]=std::string(40,'M');
    installed.names[{0,7,98}]=std::string(40,'W');
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
    page("costumes"); focus("costume-7"); draw("custom-costumes");
    Require(CostumeLabel(pick,&installed)==loc::T("selection.original"),"Custom fixture lost original costume");
    draw(nullptr,MenuInput::Select,1);draw();
    Require(pick.costume==7&&selector.Navigation().Screen()=="colors","Custom costume was not selected");
    focus("color-98"); draw("custom-colors");
    draw(nullptr,MenuInput::Select,1);draw();
    Require(pick.costume==7&&pick.color==98,"Custom color was not selected");
    page("costumes");focus("costume-98");draw("custom-costume-fallback");
    Require(CostumeLabel(pick,&installed)==std::string(40,'M'),"Installed costume name was lost");
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
