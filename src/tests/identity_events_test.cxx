// The identity requests the interface sends become the helper's tournament
// JSON, and the helper's answers fold into the view: here, Discord's browser
// sign-in and the tournament desk.
#include "../session/IdentityEvents.hxx"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

nlohmann::json Request(sf4e::netplay::IdentityOp op, const std::string& bridge = "brg_1") {
    sf4e::netplay::IdentityRequest request; request.op = op; request.bridge = bridge;
    const auto text = sf4e::session::BuildTournamentRequest(request);
    return text.empty() ? nlohmann::json() : nlohmann::json::parse(text);
}

void Requests() {
    using sf4e::netplay::IdentityOp;
    const std::pair<IdentityOp, const char*> discord[] = {
        {IdentityOp::DiscordStatus, "discord_status"}, {IdentityOp::DiscordConnect, "discord_connect"},
        {IdentityOp::DiscordRemove, "discord_remove"}, {IdentityOp::DiscordCancel, "discord_cancel"},
    };
    for (const auto& [op, name] : discord) {
        Check(Request(op) == nlohmann::json({{"op", name}, {"bridge_id", "brg_1"}}), "A Discord request was not built for the helper");
        Check(Request(op, "").is_null(), "A Discord request without its service was built");
    }
}

void Answers() {
    sf4e::netplay::IdentityView view;
    // The profile says whether the service offers Discord and names a bracket
    // site; an older service's sign-in sites are not read.
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":1,"op":"bridge_inspect","ok":true,"data":{
        "profile":{"bridge_id":"brg_1","origin":"https://bridge.embernetplay.link","display_name":"Ember"},
        "capabilities":{"connections":[],"features":["discord.accounts"],"sign_in_sites":[{"id":"challonge"}],
        "tournaments":"https://brackets.embernetplay.link"}}})"), view);
    Check(!view.inspectedDiscord && view.inspectedDiscordAccounts, "Discord's features were not read");
    Check(view.inspectedDesk, "The bracket site for the desk was not read");
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":7,"op":"discord_status","ok":true,"data":{
        "account":{"user_id":"274220342558756145","username":"player1","connected_at":30}}})"), view);
    Check(view.discordUser == "274220342558756145" && view.discordName == "player1" && view.discordConnectedAt == 30,
        "Discord's account was not read");
    // A failed read leaves the account as it was.
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":4,"op":"discord_status","ok":false,"reason":"rate_limited"})"), view);
    Check(view.discordName == "player1" && view.failure == "rate_limited", "A failed read changed the account");
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":3,"op":"discord_remove","ok":true,"data":{"account":null}})"), view);
    Check(view.discordUser.empty() && view.discordName.empty() && view.discordConnectedAt == 0, "Unlinking Discord's account was not read");
    // A later profile without a bracket site offers no desk.
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":5,"op":"bridge_inspect","ok":true,"data":{
        "profile":{"bridge_id":"brg_1","origin":"https://bridge.embernetplay.link","display_name":"Ember"},
        "capabilities":{"connections":[],"features":["discord"]}}})"), view);
    Check(view.inspectedDiscord && !view.inspectedDiscordAccounts && !view.inspectedDesk, "A profile without a bracket site offered the desk");
}

// One classification decides what the runtime lets through during a live
// game and what the screens retire, read once, or send as cleanup.
void Effects() {
    using sf4e::netplay::IdentityEffect;
    using sf4e::netplay::IdentityOp;
    using sf4e::netplay::EffectOf;
    for (const auto op : {IdentityOp::Status, IdentityOp::BridgeList, IdentityOp::BridgeInspect, IdentityOp::LinkList,
             IdentityOp::DiscordStatus})
        Check(EffectOf(op) == IdentityEffect::Read, "A read is not a read, so a live game would refuse it");
    Check(EffectOf(IdentityOp::DiscordConnect) == IdentityEffect::Open, "A sign-in does not open");
    Check(EffectOf(IdentityOp::DiscordCancel) == IdentityEffect::Cleanup, "A cancel is not cleanup");
    for (const auto op : {IdentityOp::Enable, IdentityOp::Import, IdentityOp::Reset}) Check(EffectOf(op) == IdentityEffect::KeyChange, "A key change is not one");
    Check(EffectOf(IdentityOp::PreviewImport) == IdentityEffect::Preview, "A backup preview is not a preview");
    for (const auto op : {IdentityOp::Unlock, IdentityOp::Export, IdentityOp::BridgeApprove, IdentityOp::BridgeForget, IdentityOp::LinkClaim,
             IdentityOp::LinkCancel, IdentityOp::LinkRemove, IdentityOp::DiscordRemove})
        Check(EffectOf(op) == IdentityEffect::Change, "A change is classified as something else");
    // The four sign-in requests map both ways.
    using sf4e::netplay::SignInSource;
    using sf4e::netplay::SignInStep;
    for (const auto source : {SignInSource::Discord})
        for (const auto step : {SignInStep::Status, SignInStep::Connect, SignInStep::Remove, SignInStep::Cancel}) {
            const auto signIn = sf4e::netplay::SignInOf(sf4e::netplay::SignInRequest(source, step));
            Check(signIn && signIn->source == source && signIn->step == step, "A sign-in request does not map back to its source and step");
        }
    Check(!sf4e::netplay::SignInOf(IdentityOp::LinkList), "A link request is a sign-in");
}

// Names are kept to 64 characters, as the helper counts them, and never cut
// inside a character.
void Names() {
    std::string name;
    for (int i = 0; i < 70; ++i) name += "\xe5\xa4\xa7";  // 3 bytes each
    sf4e::netplay::IdentityView view;
    nlohmann::json status = {{"request_id", 2}, {"op", "discord_status"}, {"ok", true}};
    status["data"]["account"] = {{"user_id", "1"}, {"username", name}};
    sf4e::session::ApplyTournamentEvent(status, view);
    Check(view.discordName == name.substr(0, 64 * 3), "A long account name was not kept to 64 characters");
    status["data"]["account"]["username"] = std::string(63, 'a') + "\xc3\xa9\xc3\xa9";
    sf4e::session::ApplyTournamentEvent(status, view);
    Check(view.discordName == std::string(63, 'a') + "\xc3\xa9", "A name was cut inside a character");
    // A byte that is no character reads as U+FFFD and counts as one.
    status["data"]["account"]["username"] = std::string("ab\xff") + "cd";
    sf4e::session::ApplyTournamentEvent(status, view);
    Check(view.discordName == std::string("ab\xef\xbf\xbd") + "cd", "A stray byte was not replaced");
    Check(view.discordConnectedAt == 0, "A missing connection time was not 0");
    for (const auto& at : {nlohmann::json("soon"), nlohmann::json(-5), nlohmann::json(1.5)}) {
        status["data"]["account"]["connected_at"] = at;
        sf4e::session::ApplyTournamentEvent(status, view);
        Check(view.discordConnectedAt == 0, "A connection time that is no whole number was not 0");
    }
}
}

// The tournament desk: its reads and actions as the helper takes them, and
// the events and sets it answers.
void Desk() {
    using sf4e::netplay::IdentityOp;
    Check(sf4e::netplay::EffectOf(IdentityOp::DeskRead) == sf4e::netplay::IdentityEffect::Read, "A desk read is not a read");
    Check(sf4e::netplay::EffectOf(IdentityOp::DeskAct) == sf4e::netplay::IdentityEffect::Change, "A desk action is not a change");
    sf4e::netplay::IdentityRequest read; read.op = IdentityOp::DeskRead; read.bridge = "brg_1";
    Check(nlohmann::json::parse(sf4e::session::BuildTournamentRequest(read)) == nlohmann::json({{"op", "desk_read"}, {"bridge_id", "brg_1"}}),
        "The tournaments read was not built");
    read.command = "r1-1";
    Check(!nlohmann::json::parse(sf4e::session::BuildTournamentRequest(read)).contains("set"), "A set was read without its tournament");
    read.target = "weekly_3";
    const auto sets = nlohmann::json::parse(sf4e::session::BuildTournamentRequest(read));
    Check(sets.value("event", "") == "weekly_3" && sets.value("set", "") == "r1-1", "A sets read lost its tournament or its set");
    sf4e::netplay::IdentityRequest act; act.op = IdentityOp::DeskAct; act.bridge = "brg_1"; act.target = "/tournaments/weekly_3/sets/r1-1/hold";
    act.command = R"({"held":true})";
    const auto built = nlohmann::json::parse(sf4e::session::BuildTournamentRequest(act));
    Check(built.value("path", "") == act.target && built["command"] == nlohmann::json({{"held", true}}), "A desk action was not built");
    act.command = "[1]";
    Check(sf4e::session::BuildTournamentRequest(act).empty(), "A desk action whose command is no object was built");

    sf4e::netplay::IdentityView view;
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":1,"op":"desk_read","ok":true,"data":{"events":[
        {"id":"weekly_3","title":"Weekly","status":"sign_ups","starts_at":1800000000,"signed_up":9,"cap":0,"games_to_win":3,
         "check_in":true,"discord":true,"actions":["close_sign_ups","start",5]}, 5]}})"), view);
    Check(view.deskEvents.size() == 1, "The tournaments were not read");
    const auto& event = view.deskEvents[0];
    Check(event.title == "Weekly" && event.status == "sign_ups" && event.startsAt == 1800000000 && event.signedUp == 9 && event.gamesToWin == 3 &&
        event.checkIn && event.webhook && event.actions == std::vector<std::string>{"close_sign_ups", "start"},
        "A tournament was not read as the desk shows it");
    Check(!view.deskAccess.known && view.deskAccess.allowed, "A read without access said something about Tournament Mode");
    // Not an organizer yet: the invite is asked for.
    sf4e::netplay::IdentityView invited;
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":3,"op":"desk_read","ok":true,"data":{"events":[],
        "access":{"invite_only":true,"allowed":false}}})"), invited);
    Check(invited.deskAccess.known && invited.deskAccess.inviteOnly && !invited.deskAccess.allowed, "Tournament Mode's invitation was not read");
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":2,"op":"desk_read","ok":true,"data":{"event":"weekly_3","sets":[
        {"id":"r1-1","round_label":"Semifinals","round":1,"players":["Kuro",""],"score":[2,1],"desk_state":"needs_review",
         "match_id":"emt_1","revision":"4","held":true,"games":[{"attempt_id":"att_1","outcome":null,"state":"review"}]},
        {"id":"r1-2","score":"2-1"}]}})"), view);
    Check(view.deskEvent == "weekly_3" && view.deskSets.size() == 2 && view.deskEvents.size() == 1, "The sets were not read, or they replaced the tournaments");
    const auto& set = view.deskSets[0];
    Check(set.round == 1 && set.players[0] == "Kuro" && set.players[1].empty() && set.scored && set.score[0] == 2 && set.score[1] == 1 &&
        set.state == "needs_review" && set.held && set.revision == "4", "A set was not read as the desk shows it");
    Check(set.games.size() == 1 && set.games[0].attempt == "att_1" && set.games[0].state == "review", "A set's games were not read");
    Check(!view.deskSets[1].scored, "A score that is no pair was taken");

    // Opening the bracket site, redeeming an invite, a new tournament's ID, and deleting a draft.
    Check(sf4e::netplay::EffectOf(IdentityOp::DeskOpen) == sf4e::netplay::IdentityEffect::Open, "Opening the site is not an open");
    sf4e::netplay::IdentityRequest open; open.op = IdentityOp::DeskOpen; open.bridge = "brg_1";
    Check(nlohmann::json::parse(sf4e::session::BuildTournamentRequest(open)) == nlohmann::json({{"op", "desk_open"}, {"bridge_id", "brg_1"}}),
        "Opening the site was not built");
    sf4e::netplay::IdentityRequest invite; invite.op = IdentityOp::DeskRedeem; invite.bridge = "brg_1"; invite.command = "ABCDE-FGHJK";
    Check(nlohmann::json::parse(sf4e::session::BuildTournamentRequest(invite)) == nlohmann::json({{"op", "desk_redeem"}, {"bridge_id", "brg_1"}, {"code", "ABCDE-FGHJK"}}),
        "Redeeming an invite was not built");
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":6,"op":"desk_act","ok":true,"data":{"id":"t8"}})"), view);
    Check(view.deskMade == "t8", "A new tournament's ID was not read");
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":7,"op":"desk_act","ok":true,"data":null})"), view);
    Check(view.deskMade.empty(), "An action that made no tournament kept the last one's ID");
    sf4e::netplay::IdentityRequest stop; stop.op = IdentityOp::DeskStop; stop.bridge = "brg_1"; stop.target = "t8";
    Check(nlohmann::json::parse(sf4e::session::BuildTournamentRequest(stop)) ==
        nlohmann::json({{"op", "desk_stop"}, {"bridge_id", "brg_1"}, {"event", "t8"}}), "Deleting a tournament was not asked for");
}

int main() {
    try {
        Requests(); Answers(); Effects(); Names(); Desk();
        std::cout << "Identity events passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
