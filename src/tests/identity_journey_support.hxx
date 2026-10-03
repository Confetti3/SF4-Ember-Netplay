#pragma once
// What the Ember ID, tournament and Connect Discord journeys share: a Harness
// that records the menu rows and status line the shell draws, lists the
// identity requests it has submitted, and answers them. The probes capture the
// object, so it is not copyable; make one per scenario and never two at once.
#include "shell_journey_support.hxx"
#include <algorithm>
#include <string>
#include <vector>
namespace {
struct Journey:Harness {
 using IdentityOp=sf4e::netplay::IdentityOp;
 std::vector<MenuEntry> rows;std::string status;
 int untilLimit=10; // how many answers until() gives before it stops
 Journey(){SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});SetMenuStatusProbe([&](const char* s,Tone){status=s;});}
 ~Journey(){SetMenuEntriesProbe({});SetMenuStatusProbe({});}
 Journey(const Journey&)=delete;Journey& operator=(const Journey&)=delete;
 const MenuEntry* row(const char* name)const{const auto it=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==name;});return it==rows.end()?nullptr:&*it;}
 bool has(const char* name)const{return row(name)!=nullptr;}
 std::ptrdiff_t index(const char* name)const{return std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==name;})-rows.begin();}
 // The identity requests and tournament commands submitted so far.
 std::vector<const sf4e::netplay::IdentityRequest*> sent()const{std::vector<const sf4e::netplay::IdentityRequest*> out;for(const auto& a:actions)if(a.identity.op!=IdentityOp::None)out.push_back(&a.identity);return out;}
 std::vector<const sf4e::netplay::tournament::Command*> played()const{
  using sf4e::netplay::tournament::Command;std::vector<const Command*> out;for(const auto& a:actions)if(a.tournament.op!=Command::Op::None)out.push_back(&a.tournament);return out;}
 // How many of the requests from index `from` on carry `op`.
 std::size_t count(IdentityOp op,std::size_t from=0)const{std::size_t n=0;const auto all=sent();for(std::size_t i=from;i<all.size();++i)n+=all[i]->op==op;return n;}
 // The service answers the newest request, with a failure when `ok` is false.
 void answer(bool ok=true,const char* failure=""){
  auto& id=view.identity;view.identityTicket=sent().back()->ticket;view.identityRequest=id.requestId=view.identityTicket+100;
  id.ok=ok;id.failure=failure;Frame(0,2);
 }
 // Answers requests until the newest one is `op`; false if it never comes.
 bool until(IdentityOp op){for(int i=0;i<untilLimit&&sent().back()->op!=op;++i)answer();return sent().back()->op==op;}
 // Types into the open editor and presses Enter.
 void type(const char* text){
  ImGui::GetIO().AddInputCharactersUTF8(text);Frame();
  ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,true);Frame();ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,false);Frame();
 }
 void identify(){auto& id=view.identity;id.emberId="emb1_j25zrhe6yjirrt6lgivzsqfrlszk3sqwxwoa6k2xtkugpmdhvlja";id.fingerprint="j25zrhe6-pmdhvlja";}
 void ready(){view.identity.state="ready";identify();}
};
}
// The Connect Discord and onboarding scenarios, in discord_connect_journey_test.cxx.
void RunDiscordConnectJourneys();
