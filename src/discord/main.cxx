#define DISCORDPP_IMPLEMENTATION
#include <discordpp.h>
#include "BridgeServer.hxx"
#include "Ticket.hxx"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <ctime>
#include <set>
#include <shellapi.h>

using Json = nlohmann::json;
int WINAPI wWinMain(HINSTANCE,HINSTANCE,LPWSTR,int) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    int argc=0; auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(!argv) return 1;
    const bool valid=argc==3 && std::wstring(argv[1])==L"--pipe";
    const std::wstring pipe=valid?argv[2]:L""; LocalFree(argv);
    if(!valid) return 1;
    sf4e::discord::BridgeServer bridge;
    if(!bridge.Open(pipe)) return 2;
    bool running=true, pending=false;
    std::string accepted, lastSent, clearedFor;
    std::uint64_t sequence=0, epoch=0;
    auto client=std::make_unique<discordpp::Client>();
    client->SetApplicationId(SF4E_DISCORD_APPLICATION_ID);
    wchar_t path[32768]{};
    const auto length=GetModuleFileNameW(nullptr,path,32768);
    if(!length || length>=32768) return 3;
    const auto launcher=std::filesystem::path(path).parent_path()/L"Launcher.exe";
    const auto command=L"\""+launcher.wstring()+L"\" --discord-launch";
    const int bytes=WideCharToMultiByte(CP_UTF8,0,command.c_str(),-1,nullptr,0,nullptr,nullptr);
    std::string utf8(bytes,'\0');
    WideCharToMultiByte(CP_UTF8,0,command.c_str(),-1,&utf8[0],bytes,nullptr,nullptr); utf8.pop_back();
    const bool registered=client->RegisterLaunchCommand(SF4E_DISCORD_APPLICATION_ID,utf8);
    client->SetActivityJoinCallback([&](std::string secret) {
        std::string party; std::uint64_t expires=0;
        if(sf4e::discord::TicketMetadata(secret,party,expires))
            accepted=std::move(secret); // one bounded slot, drained outside SDK callbacks
    });
    Json latest;
    ULONGLONG nextUpdate=0, lastInput=GetTickCount64(), backoff=1000;
    const auto started=static_cast<std::uint64_t>(std::time(nullptr));
    const std::set<std::string> activities={"Starting Ember","In menus","Playing offline","In a room","Queued","Ready","Fighting","Spectating"};
    while(running && bridge.Alive()) {
        for(int budget=0;budget<8;++budget) {
            std::string payload; bool present=false;
            if(!bridge.Receive(payload,present)) { running=false; break; }
            if(!present) break;
            try {
                const auto value=Json::parse(payload);
                if(value.at("type")=="shutdown") { running=false; break; }
                if(value.at("type")!="presence" || !activities.count(value.at("activity").get<std::string>())) { running=false; break; }
                const auto roomEpoch=value.at("epoch").get<std::uint64_t>();
                if(roomEpoch<epoch) continue;
                const auto size=value.at("size").get<int>(), capacity=value.at("capacity").get<int>();
                if(size<0 || size>16 || capacity<0 || capacity>16 || size>capacity ||
                    value.at("party").get<std::string>().size()>64 || value.at("secret").get<std::string>().size()>128) { running=false; break; }
                epoch=roomEpoch; latest=value; lastInput=GetTickCount64();
            } catch(...) { running=false; break; }
        }
        discordpp::RunCallbacks();
        if(!accepted.empty()) {
            running=bridge.Send(Json{{"type","join"},{"sequence",++sequence},{"epoch",epoch},{"secret",accepted}}.dump()) && running;
            accepted.clear();
        }
        if(!running || GetTickCount64()-lastInput>15000) break;
        if(!latest.is_null()) {
            if(latest.value("expires",std::uint64_t(0))<=static_cast<std::uint64_t>(std::time(nullptr))) latest["secret"]="";
            const auto serialized=latest.dump();
            const bool hidden=!latest.at("show").get<bool>();
            // Clear immediately when disabled or when a previous invitation
            // becomes unsafe; the replacement presence is still coalesced.
            if(!lastSent.empty() && (hidden || latest.at("secret")=="")) {
                const auto previous=Json::parse(lastSent);
                if(clearedFor!=serialized && ((hidden && previous.at("show")==true) || previous.at("secret")!="")) { client->ClearRichPresence(); clearedFor=serialized; }
            }
            if(hidden) { lastSent=serialized; }
            else if(!pending && GetTickCount64()>=nextUpdate && serialized!=lastSent) {
                discordpp::Activity activity;
                activity.SetType(discordpp::ActivityTypes::Playing);
                activity.SetDetails(latest.at("activity").get<std::string>());
                discordpp::ActivityTimestamps timestamps; timestamps.SetStart(started); activity.SetTimestamps(timestamps);
                discordpp::ActivityAssets assets; assets.SetLargeImage("ember"); assets.SetLargeText("Ember"); activity.SetAssets(assets);
                if(latest.at("size").get<int>()>0) {
                    discordpp::ActivityParty party;
                    party.SetId(latest.at("party").get<std::string>());
                    party.SetCurrentSize(latest.at("size").get<int>()); party.SetMaxSize(latest.at("capacity").get<int>());
                    activity.SetParty(party);
                }
                const auto secret=latest.at("secret").get<std::string>();
                if(!secret.empty()) { discordpp::ActivitySecrets secrets; secrets.SetJoin(secret); activity.SetSecrets(secrets); }
                activity.SetSupportedPlatforms(discordpp::ActivityGamePlatforms::Desktop);
                pending=true; nextUpdate=GetTickCount64()+5000;
                client->UpdateRichPresence(activity,[&,serialized](const discordpp::ClientResult& result) {
                    pending=false;
                    if(result.Successful()) { lastSent=serialized; backoff=1000; }
                    else { lastSent.clear(); nextUpdate=GetTickCount64()+backoff; backoff=(std::min)(backoff*2,ULONGLONG(30000)); }
                    running=bridge.Send(Json{{"type","status"},{"available",result.Successful()},{"registered",registered}}.dump()) && running;
                });
            }
            // Refresh periodically so a Discord restart restores unchanged activity.
            if(!pending && GetTickCount64()>nextUpdate+30000) lastSent.clear();
        }
        Sleep(10);
    }
    client->ClearRichPresence();
    for(int i=0;i<10;++i) { discordpp::RunCallbacks(); Sleep(10); }
    client.reset();
    return 0;
}
