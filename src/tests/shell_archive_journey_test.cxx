// Replays > Archive, the public replay archive in the game: the first page asked for once on a service the
// player trusts, the sets the listing holds as rows and what A offers on one, Watch now sending the download
// of the set's games in order, the filters page and the one page asked for after it, the next page at the end
// of the list, the states with their own words, and Find this player's sets from a row of Matches.
#include "public_rooms_journey_support.hxx"
#include "shell_replay_journey.hxx"
#include "../ui/ArchiveTab.hxx"
#include "../ui/CountryFlag.hxx"
#include "../ui/NameMarks.hxx"
#include "../ui/PublicRoomsPanel.hxx"
#include "../ui/ReplaysPanel.hxx"
#include <memory>
namespace {
namespace ar = sf4e::netplay::archive;
using Archived = sf4e::platform::replays::ArchivedReplay;

unsigned digests = 0;
std::string NewDigest() {
 char text[80];std::snprintf(text,sizeof(text),"%064x",++digests);return text;
}
ar::Game MakeGame(std::uint64_t at,const char* a,const char* b,int fighterA=0,int fighterB=1) {
 ar::Game game;game.digest=NewDigest();game.url="https://archive.example/games/"+game.digest+".emberreplay";
 game.savedAt=at;game.names[0]=a;game.names[1]=b;game.fighters={{fighterA,fighterB}};return game;
}
ar::Game InSet(std::uint64_t at,const char* set) {
 auto game=MakeGame(at,"Player 1","Player 2");game.set=set;game.event="Player 1's Cup";game.round="Top 8";game.phase="Winners";
 game.countries={{"GB","NL"}};game.classes={{7,0}};return game;
}
std::shared_ptr<const ar::Listing> ListedGames(std::vector<ar::Game> games,const std::string& next="") {
 auto listing=std::make_shared<ar::Listing>();listing->games=std::move(games);listing->next=next;
 listing->sets["emt_a"].score={{2,1}};listing->events={"Player 1's Cup"};listing->countries={"GB","NL"};
 listing->total=40;listing->matching=3;return listing;
}
ar::Filter NamedFilter(const char* text) {
 return ar::Filter::Named(text);
}

// The runtime's status as it is after a page is heard for `filter`.
void Hear(Journey& j,const ar::Filter& filter,std::shared_ptr<const ar::Listing> listing,bool loadingMore=false,bool moreFailed=false) {
 auto& status=j.h.view.archiveBrowse;
 status.bridge="brg_1";status.filter=filter;status.state=ar::Status::State::Listed;status.error.clear();
 status.listing=std::move(listing);status.loadingMore=loadingMore;status.moreFailed=moreFailed;++status.revision;j.h.Frame(0,2);
}
void HearFailure(Journey& j,const ar::Filter& filter,ar::Status::State state,const char* error) {
 auto& status=j.h.view.archiveBrowse;
 status.bridge="brg_1";status.filter=filter;status.state=state;status.error=error;status.listing.reset();++status.revision;j.h.Frame(0,2);
}
// The archive commands sent so far.
std::vector<const commands::ArchiveList*> Lists(const Journey& j) {
 std::vector<const commands::ArchiveList*> out;
 for(const auto& a:j.h.actions)if(const auto* list=As<commands::ArchiveList>(a))out.push_back(list);
 return out;
}
std::vector<const commands::ArchiveWatch*> Watches(const Journey& j) {
 std::vector<const commands::ArchiveWatch*> out;
 for(const auto& a:j.h.actions)if(const auto* watch=As<commands::ArchiveWatch>(a))out.push_back(watch);
 return out;
}
const MenuChoice* ChoiceOf(const MenuEntry& row,const char* id) {
 for(const auto& choice:row.choices)if(choice.id==id)return &choice;
 return nullptr;
}
std::vector<std::string> Choices(const MenuEntry& row) {
 std::vector<std::string> ids;for(const auto& choice:row.choices)ids.push_back(choice.id);return ids;
}
// Opens the Archive tab with a service the player trusts and lets the Ember ID answer.
void OpenArchive(Journey& j) {
 j.h.view.replays.ready=true;
 j.h.Screen("replay-archive");j.h.Frame(0,2);
 j.answerIdentityUntilIdle();j.h.Frame(0,2);
}
// Opens the row's choices, moves along them `steps` times and picks.
void PickChoice(Journey& j,const std::string& row,int steps) {
 j.h.Choose(row.c_str());
 for(int i=0;i<steps;++i)j.h.Press(MenuInput::Right);
 j.h.Press(MenuInput::Select);
}

// The first page is asked for once with nothing narrowed; the listing's sets are the rows, newest first, each
// with what A offers.
void ListingJourney() {
 Journey j;OpenArchive(j);
 Check(j.h.shell.Navigation().Screen()=="replay-archive","The Archive tab did not open");
 const auto first=Lists(j);
 Check(first.size()==1&&first[0]->bridge=="brg_1"&&!first[0]->next&&!first[0]->filter.Active()&&first[0]->filter.order==ar::Order::Newest,
  "The first page was not asked for once on the service, with nothing narrowed");
 j.h.Frame(0,5);
 Check(Lists(j).size()==1,"The first page was asked for again before it was answered");
 Check(j.row("archive-filters")&&j.row("archive-loading")&&!j.row("archive-clear"),"A page loading did not show the Filters row and a loading row");
 Check(j.row("archive-filters")->value==loc::T("archive.filter_none"),"The Filters row did not say that nothing narrows the list");

 // A tournament set (its score the bridge's), an online pair's games, and one player's session.
 const auto game1=InSet(1800000300,"emt_a"),game0=InSet(1800000100,"emt_a");
 const auto pair2=MakeGame(1800000200,"Player 3","Player 4",2,3),pair1=MakeGame(1800000150,"Player 4","Player 3",3,2);
 const auto s3=MakeGame(1800000040,"Player 5","",9,10),s2=MakeGame(1800000020,"","Player 5",11,9);
 Hear(j,{},ListedGames({game1,pair2,pair1,game0,s3,s2}));
 Check(j.rows.size()>=4&&j.rows[0].id=="archive-filters"&&j.rows[1].id=="archive:emt_a"&&j.rows[2].id=="archive:"+pair2.digest&&j.rows[3].id=="archive:"+s3.digest,
  "The sets are not the rows, newest first");
 Check(!j.row("archive-loading")&&!j.row("archive-more")&&!j.row("archive-more-retry"),"A page that was heard still says it is loading");
 const auto& tournament=*j.row("archive:emt_a");
 Check(tournament.label=="Player 1 2-1 Player 2"&&tournament.userText&&tournament.Match()&&tournament.fighter1==0&&tournament.fighter2==1,"A tournament set's row does not name its players, score and fighters");
 Check(tournament.hint==loc::T("archive.watch_hint")&&tournament.chosen=="archive-watch","A set's row does not say that A watches it");
 Check(Choices(tournament)==std::vector<std::string>({"archive-watch","find-1","find-2"})&&ChoiceOf(tournament,"archive-watch")->enabled&&
  ChoiceOf(tournament,"archive-watch")->label==loc::T("replays.watch_set")&&ChoiceOf(tournament,"find-2")->label==loc::Tf("archive.find","Player 2"),"A set's row does not offer to watch it and to find each player's sets");
 Check(tournament.value.size()>0&&tournament.detail.find("Player 1's Cup")!=std::string::npos&&tournament.detail.find("Top 8")!=std::string::npos,"A tournament set's row does not say its event and round");
 const auto& pair=*j.row("archive:"+pair2.digest);
 Check(pair.label==loc::Tf("replays.fighters",std::string("Player 4"),std::string("Player 3"))&&pair.fighter1==3&&pair.fighter2==2,"Two players' online games are not a row of their names without a score");
 const auto& session=*j.row("archive:"+s3.digest);
 Check(session.label==loc::Tf("archive.session_label",std::string("Player 5"),2)&&Choices(session)==std::vector<std::string>({"archive-watch","find-1"})&&session.fighter1==9&&session.fighter2==11,
  "One player's games are not a session row of that player alone");
 Check(ChoiceOf(session,"find-1")->label==loc::Tf("archive.find","Player 5"),"A session did not offer to find its player's sets");
 // Nothing is asked for again, and the end of a short list asks for no page it does not have.
 j.h.Frame(0,5);
 Check(Lists(j).size()==1,"A page that was heard was asked for again");

 // Cards and the pane draw for the focused row (the harness draws them with no art).
 j.h.FocusOn(("archive:"+s3.digest).c_str());j.h.Frame(0,2);
 j.h.FocusOn("archive:emt_a");j.h.Frame(0,2);
}

// Watch now: the first game of the set is downloaded and then watched; Next game on Replays sends the next.
void WatchJourney() {
 Journey j;OpenArchive(j);
 j.h.view.preferences.classEmblems=true;
 const auto game1=InSet(1800000300,"emt_a"),game0=InSet(1800000100,"emt_a");
 Hear(j,{},ListedGames({game1,game0}));
 PickChoice(j,"archive:emt_a",0);
 auto watches=Watches(j);
 Check(watches.size()==1&&watches[0]->bridge=="brg_1"&&watches[0]->digest==game0.digest&&watches[0]->url==game0.url&&
  watches[0]->watch.mode==sf4e::replay::Mode::Watch&&watches[0]->watch.path.empty(),"Watch now did not ask for the set's first game to be downloaded and watched");
 // The names and classes the archive has are drawn over the game, as a replay's noted classes are.
 const auto& caption=watches[0]->watch.caption;
 Check(caption.names&&caption.name[0]=="Player 1"&&caption.band[0]==7&&caption.band[1]==0,"The watched game did not carry the names and classes the archive has");
 j.h.view.archiveBrowse.fetching=game0.digest;j.h.Frame(0,2);
 Check(j.status==loc::T("archive.downloading"),"The status line did not say the game is downloading");
 // It is downloaded; the log has it, and Replays offers the next game, once.
 j.h.view.archiveBrowse.fetching.clear();j.h.view.archiveBrowse.fetched++;j.h.Frame(0,2);
 j.h.Screen("replays");
 const auto* next=j.row("set-next");
 Check(next&&next->label==loc::Tf("replays.next_game",2,2)&&next->enabled,"Replays did not offer the set's next game");
 j.h.Choose("set-next");
 watches=Watches(j);
 Check(watches.size()==2&&watches[1]->digest==game1.digest,"Next game did not ask for the set's second game");
 j.h.Frame(0,2);
 Check(!j.row("set-next"),"Next game stayed after the set's last game was started");

 // A download that failed says why; one in flight keeps A from starting another.
 Journey k;OpenArchive(k);
 Hear(k,{},ListedGames({game1,game0}));
 PickChoice(k,"archive:emt_a",0);
 k.h.view.archiveBrowse.fetching=game0.digest;k.h.Frame(0,2);
 Check(k.status==loc::T("archive.downloading")&&!ChoiceOf(*k.row("archive:emt_a"),"archive-watch")->enabled,"A game being downloaded did not keep Watch now from starting another");
 k.h.view.archiveBrowse.fetching.clear();k.h.view.archiveBrowse.fetched++;k.h.view.archiveBrowse.fetchError="http_404";k.h.Frame(0,2);
 Check(k.status==loc::Tf("archive.download_failed",sf4e::ui::PublicRoomsPanel::FailureText("http_404")),"A download that failed did not say so");

 // Without the main menu (a room is open, or the game is not ready) A does not start it.
 Journey m;OpenArchive(m);
 m.h.view.replays.ready=false;
 Hear(m,{},ListedGames({game1,game0}));
 const auto* off=ChoiceOf(*m.row("archive:emt_a"),"archive-watch");
 Check(off&&!off->enabled&&off->detail==loc::T("replays.not_ready"),"Watch now was offered while the game is not ready");
 Check(ChoiceOf(*m.row("archive:emt_a"),"find-1")->enabled&&m.row("archive:emt_a")->chosen=="find-1","Find this player's sets was not what A picks while Watch now cannot start");
}

// Find this player's sets sets the filter to that name alone and asks for its first page.
void FindJourney() {
 Journey j;OpenArchive(j);
 const auto pair2=MakeGame(1800000200,"Player 3","Player 4",2,3),pair1=MakeGame(1800000150,"Player 4","Player 3",3,2);
 Hear(j,{},ListedGames({pair2,pair1}));
 PickChoice(j,"archive:"+pair2.digest,2);
 Check(j.h.shell.Navigation().Screen()=="replay-archive","Find this player's sets left the Archive tab");
 j.h.Frame(0,2);
 const auto asks=Lists(j);
 Check(asks.size()==2&&!asks[1]->next&&asks[1]->filter==NamedFilter("Player 3"),"Find this player's sets did not ask for the first page of that name alone");
 Check(j.row("archive-filters")->value=="Player 3"&&j.row("archive-clear")&&j.row("archive-loading"),"The Filters row does not say the name, or the old sets stay while the new page loads");
 // The same from anywhere else: the card stream calls it, and Back returns to where it was called.
 Journey k;k.h.view.replays.ready=true;k.h.Frame();
 k.h.shell.OpenArchiveFor("Player 9");k.h.Frame(0,2);
 k.answerIdentityUntilIdle();k.h.Frame(0,2);
 const auto from=Lists(k);
 Check(k.h.shell.Navigation().Screen()=="replay-archive"&&from.size()==1&&from[0]->filter==NamedFilter("Player 9"),"OpenArchiveFor did not open the Archive tab on that name alone");
 k.h.Press(MenuInput::Back);
 Check(k.h.shell.Navigation().Screen()=="home","Back from the Archive tab did not return to where it was opened from");

 // A row of Matches offers each named player's sets, and picking one opens the Archive tab on that name.
 Journey m;m.h.view.replays.ready=true;
 Archived replay;replay.path=std::filesystem::u8path("C:\\r\\m.emberreplay");replay.label="2026-11-01 18:50";replay.names[0]="Player 1";replay.names[1]="Player 2";
 Archived lone=replay;lone.path=std::filesystem::u8path("C:\\r\\n.emberreplay");lone.names[1].clear();
 m.h.view.replays.archive=std::make_shared<const std::vector<Archived>>(std::vector<Archived>{replay,lone});
 m.h.Screen("replays");
 const auto& both=*m.row("replay:C:\\r\\m.emberreplay");
 Check(both.choices.size()==9&&both.choices[7].id=="find-1"&&both.choices[8].id=="find-2"&&both.choices[7].label==loc::Tf("archive.find","Player 1"),"A match's row does not offer each player's sets");
 Check(Choices(*m.row("replay:C:\\r\\n.emberreplay")).back()=="find-1"&&m.row("replay:C:\\r\\n.emberreplay")->choices.size()==8,"A match with one named player does not offer that player's sets alone");
 PickChoice(m,"replay:C:\\r\\m.emberreplay",8);
 Check(m.h.shell.Navigation().Screen()=="replay-archive","Find this player's sets on a match did not open the Archive tab");
 m.answerIdentityUntilIdle();m.h.Frame(0,2);
 const auto found=Lists(m);
 Check(found.size()==1&&found[0]->filter==NamedFilter("Player 2"),"Find this player's sets on a match did not ask for that player's name alone");
}

// The Filters row opens one page of rows; stepping them asks for nothing, and Back asks once for the first page of what is chosen.
void FiltersJourney() {
 Journey j;OpenArchive(j);
 const auto game=InSet(1800000300,"emt_a");
 Hear(j,{},ListedGames({game}));
 j.h.Choose("archive-filters");
 Check(j.h.shell.Navigation().Screen()=="replay-archive-filter","The Filters row did not open the filters");
 for(const char* id:{"af-text","af-fighter","af-class","af-country","af-event","af-round","af-sort","af-count","af-clear"})Check(j.row(id)!=nullptr,(std::string("Missing filter row ")+id).c_str());
 Check(j.row("af-count")->value==loc::Tf("archive.games_of",3,40)&&!j.row("af-clear")->enabled,"The filters page does not say how many games it keeps, or Clear filters is open with nothing narrowed");
 Check(j.row("af-class")->value==loc::T("archive.any_class")&&j.row("af-country")->value==loc::T("archive.any_country")&&j.row("af-event")->value==loc::T("archive.any_event")&&
  j.row("af-round")->value==loc::T("archive.any_round")&&j.row("af-sort")->value==loc::T("archive.sort_newest")&&j.row("af-fighter")->value==loc::T("archive.any_fighter"),"The filters do not start on Any");
 // Class steps up the bands; the country and event steps are the archive's own choices.
 j.h.FocusOn("af-class");j.h.Press(MenuInput::Right);j.h.Press(MenuInput::Right);
 Check(j.row("af-class")->value==ClassName(2),"Right on Class did not step to the second band");
 j.h.FocusOn("af-country");j.h.Press(MenuInput::Right);
 Check(j.row("af-country")->value==CountryName(CountryCode::Parsed("GB")),"Right on Country did not step to the archive's first country");
 j.h.FocusOn("af-event");j.h.Press(MenuInput::Right);
 Check(j.row("af-event")->value==loc::T("archive.online"),"Right on Event did not step to Online games");
 j.h.Press(MenuInput::Right);
 Check(j.row("af-event")->value=="Player 1's Cup","Right on Event did not step to the archive's event");
 j.h.FocusOn("af-round");j.h.Press(MenuInput::Right);
 Check(j.row("af-round")->value==loc::T("archive.round.top8"),"Right on Round did not step to Top 8");
 j.h.FocusOn("af-sort");j.h.Press(MenuInput::Right);
 Check(j.row("af-sort")->value==loc::T("archive.sort_class"),"Right on Sort did not choose the highest class first");
 // The fighter is picked on a page of its own.
 j.h.Choose("af-fighter");
 Check(j.h.shell.Navigation().Screen()=="replay-archive-fighter"&&j.row("af-0")&&j.row("af-any"),"The Fighter row did not open the fighters");
 j.h.Choose("af-0");
 Check(j.h.shell.Navigation().Screen()=="replay-archive-filter"&&j.row("af-fighter")->value==std::string(sf4e::selection::FindFighter(0)->name),"Picking a fighter did not return to the filters with it chosen");
 Check(Lists(j).size()==1,"A page was asked for while the filters were being stepped");
 // Back to the list: one first page of everything chosen.
 j.h.Press(MenuInput::Back);
 Check(j.h.shell.Navigation().Screen()=="replay-archive","Back from the filters did not return to the list");
 const auto asks=Lists(j);
 Check(asks.size()==2&&!asks[1]->next,"Back from the filters did not ask for the first page");
 const ar::Filter chosen=asks[1]->filter;
 Check(chosen.band==2&&chosen.country=="GB"&&chosen.event=="Player 1's Cup"&&chosen.round=="top8"&&chosen.order==ar::Order::Class&&chosen.fighter==0&&chosen.text.empty(),"The page asked for is not what the filters chose");
 j.h.Frame(0,5);
 Check(Lists(j).size()==2,"The first page of the new filter was asked for twice");
 // The Filters row and the header say what is chosen, and Clear filters narrows nothing but keeps the order.
 const std::string summary=ui::ArchiveTab::FilterSummary(chosen);
 Check(j.row("archive-filters")->value==summary&&summary.find("Player 1's Cup")!=std::string::npos&&summary.find(std::string(sf4e::selection::FindFighter(0)->name))!=std::string::npos&&j.row("archive-clear"),"The Filters row does not say what is chosen");
 j.h.Choose("archive-clear");
 j.h.Frame(0,2);
 const auto cleared=Lists(j);
 Check(cleared.size()==3&&!cleared[2]->filter.Active()&&cleared[2]->filter.order==ar::Order::Class,"Clear filters did not ask again for everything in the order chosen");
 Check(ui::ArchiveTab::FilterSummary(cleared[2]->filter)==loc::T("archive.sort_class"),"The summary of a filter that only orders is not its order");

 // Typing a name: Select on the first row opens the editor; accepted text is the filter.
 j.h.Press(MenuInput::Fighter);
 Check(j.h.shell.Navigation().Screen()=="replay-archive-filter","The Filters shortcut did not open the filters");
 j.h.Choose("af-text");
 Check(j.h.shell.Navigation().Editing(),"Select on Player or event did not open the editor");
}

// The next page is asked for once when the end of the list is near, and never while one is on its way.
void PagingJourney() {
 Journey j;OpenArchive(j);
 const auto g1=MakeGame(1800000300,"Player 1","Player 2"),g2=MakeGame(1800000200,"Player 3","Player 4");
 Hear(j,{},ListedGames({g1,g2},"9.ab"));
 auto asks=Lists(j);
 Check(asks.size()==2&&asks[1]->next&&!asks[1]->again&&asks[1]->bridge=="brg_1","The next page was not asked for at the end of the list");
 j.h.Frame(0,5);
 Check(Lists(j).size()==2,"The next page was asked for twice");
 Hear(j,{},ListedGames({g1,g2},"9.ab"),true);
 Check(j.row("archive-more")&&j.row("archive-more")->label==loc::T("archive.loading_more")&&Lists(j).size()==2,"A page loading did not say Loading more or was asked for again");
 // The page came: the cursor moved on, and the list is asked from there.
 Hear(j,{},ListedGames({g1,g2,MakeGame(1800000100,"Player 5","Player 6")},"7.cd"));
 asks=Lists(j);
 Check(asks.size()==3&&asks[2]->next,"The page after that was not asked for");
 // The last page: nothing more is asked for.
 Hear(j,{},ListedGames({g1,g2,MakeGame(1800000100,"Player 5","Player 6")}));
 j.h.Frame(0,5);
 Check(Lists(j).size()==3&&!j.row("archive-more"),"A last page asked for another");
 // A next page that failed keeps the sets and says so; Y and A ask again.
 Hear(j,{},ListedGames({g1,g2},"5.ef"),false,true);
 Check(Lists(j).size()==3&&j.row("archive-more-retry")&&j.row("archive:"+g1.digest),"A next page that failed was asked for again by itself, or lost the sets");
 j.h.Press(MenuInput::Options);
 asks=Lists(j);
 Check(asks.size()==4&&asks[3]->next&&asks[3]->again,"Y did not ask again for the page that failed");
 j.h.Choose("archive-more-retry");
 Check(Lists(j).size()==5,"Select on Could not load more did not ask again");
 // A filter changed while a page is on its way asks for the first page of the new one.
 Hear(j,{},ListedGames({g1,g2},"3.aa"),true);
 j.h.Press(MenuInput::Fighter);
 j.h.FocusOn("af-class");j.h.Press(MenuInput::Right);j.h.Press(MenuInput::Back);
 asks=Lists(j);
 Check(asks.back()->filter.band==1&&!asks.back()->next,"A filter changed while a page loaded did not ask for its first page");
}

// A page that failed, a service without an archive, nothing that matches, and no service at all, each in its own words.
void StatesJourney() {
 Journey j;OpenArchive(j);
 HearFailure(j,{},ar::Status::State::Failed,"bridge_unreachable");
 const auto* retry=j.row("archive-retry");
 Check(retry&&retry->label==loc::T("archive.failed")&&retry->detail==loc::Tf("archive.failed_detail",sf4e::ui::PublicRoomsPanel::FailureText("bridge_unreachable"))&&!j.row("archive-loading"),
  "A page that failed did not say so with its reason");
 Check(Lists(j).size()==1,"A failed page was asked for again by itself");
 j.h.Press(MenuInput::Options);
 Check(Lists(j).size()==2&&!Lists(j)[1]->next,"Y did not try the first page again");
 HearFailure(j,{},ar::Status::State::Failed,"bridge_unreachable");
 j.h.Choose("archive-retry");
 Check(Lists(j).size()==3,"Select on the failure did not try again");
 HearFailure(j,{},ar::Status::State::Unsupported,ar::Unsupported);
 Check(j.row("archive-unsupported")&&j.row("archive-unsupported")->label==loc::T("archive.unsupported")&&!j.row("archive-retry"),"A service without an archive did not say so");
 // Nothing matches the filters: the Filters row, Clear filters and the words for it.
 j.h.shell.OpenArchiveFor("Player 7");
 Hear(j,NamedFilter("Player 7"),ListedGames({}));
 j.h.Frame(0,2);
 Check(j.rows.size()>=1&&j.row("archive-none")&&j.row("archive-none")->label==loc::T("archive.none")&&j.row("archive-clear"),"A filter that matches nothing did not say so with Clear filters");
 // An archive with no game and no filter is another thing.
 Journey e;OpenArchive(e);
 Hear(e,{},ListedGames({}));
 Check(e.row("archive-none")&&e.row("archive-none")->label==loc::T("archive.empty")&&!e.row("archive-clear"),"An empty archive did not say so");

 // No service set up on this PC: the tab says to set up the Ember ID, and asks for nothing.
 Journey n(false);
 n.h.view.replays.ready=true;n.h.Screen("replay-archive");n.h.Frame(0,5);
 Check(n.rows.size()==1&&n.row("archive-identity")&&Lists(n).empty(),"A tab with no service asked for a page or showed more than the way to set it up");
 n.h.Choose("archive-identity");
 Check(n.h.shell.Navigation().Screen()=="identity","Select on set up your Ember ID did not open it");
}

// The tabs over Replays: the Archive after Sets, before Export video.
void TabsJourney() {
 using namespace sf4e;
 ui::ReplaysPanel panel;
 ShellView plain;
 Check(panel.Tabs(plain).size()==2&&panel.Tabs(plain)[1].first==ui::ArchiveTab::Screen,"The Archive is not the tab after Matches when nothing else is");
 Archived a,b;a.path=std::filesystem::u8path("C:\\r\\a.emberreplay");b.path=std::filesystem::u8path("C:\\r\\b.emberreplay");
 a.names[0]=b.names[0]="Player 1";a.names[1]=b.names[1]="Player 2";a.time=5000;b.time=4800;
 ShellView view;view.replays.archive=std::make_shared<const std::vector<Archived>>(std::vector<Archived>{a,b});
 MenuNavigation navigation;netplay::PlayerPreferences saved;
 panel.Update(view,saved,navigation,[](ShellAction){return true;});
 const auto tabs=panel.Tabs(view);
 Check(tabs.size()==4&&tabs[0].first=="replays"&&tabs[1].first=="replay-sets"&&tabs[2].first==ui::ArchiveTab::Screen&&tabs[3].first=="replay-export","The tabs are not Matches, Sets, Archive and Export video");
 Check(ui::ArchiveTab::Owns("replay-archive")&&ui::ArchiveTab::Owns("replay-archive-filter")&&ui::ArchiveTab::Owns("replay-archive-fighter")&&!ui::ReplaysPanel::Owns("replay-archive"),
  "The archive's screens are not its own");
 // The hint bar names Filters and Retry or Refresh, where the device has the buttons.
 ui::ArchiveTab tab;
 tab.Update(view,"replay-archive","brg_1","",[](ShellAction){return true;});
 auto hints=tab.Hints(view,"replay-archive");
 Check(hints.size()<2||(hints.size()==2&&hints[0].label==loc::T("archive.filters")&&hints[1].label==loc::T("legend.refresh")),"The hint bar does not name Filters and Refresh");
 view.archiveBrowse.bridge="brg_1";view.archiveBrowse.state=ar::Status::State::Failed;
 hints=tab.Hints(view,"replay-archive");
 Check(hints.size()<2||hints[1].label==loc::T("archive.retry"),"The hint bar does not name Retry after a failure");
 Check(tab.Hints(view,"replays").empty(),"The hint bar of another tab names the archive's shortcuts");
}
}
void ArchiveJourneys() {
 TabsJourney();
 ListingJourney();
 WatchJourney();
 FindJourney();
 FiltersJourney();
 PagingJourney();
 StatesJourney();
}
