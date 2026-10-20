// The identity requests the interface sends become the helper's tournament
// JSON, and the helper's answers fold into the view: here, the browser
// sign-ins Discord and the listed sign-in sites share, each into its own fields.
#include "../session/IdentityEvents.hxx"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

nlohmann::json Request(sf4e::netplay::IdentityOp op, const std::string& bridge = "brg_1", const std::string& site = {}) {
    sf4e::netplay::IdentityRequest request; request.op = op; request.bridge = bridge; request.site = site;
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
    // A site's requests name the site too, and are refused without it.
    const std::pair<IdentityOp, const char*> sites[] = {
        {IdentityOp::SiteStatus, "site_status"}, {IdentityOp::SiteConnect, "site_connect"},
        {IdentityOp::SiteRemove, "site_remove"}, {IdentityOp::SiteCancel, "site_cancel"},
    };
    for (const auto& [op, name] : sites) {
        Check(Request(op, "brg_1", "startgg") == nlohmann::json({{"op", name}, {"bridge_id", "brg_1"}, {"site", "startgg"}}),
            "A site request was not built for the helper");
        Check(Request(op, "brg_1").is_null() && Request(op, "", "startgg").is_null(), "A site request without its site or service was built");
        // The organizer sign-in says so; it has no cancel of its own.
        sf4e::netplay::IdentityRequest organizer; organizer.op = op; organizer.bridge = "brg_1"; organizer.site = "startgg"; organizer.organizer = true;
        const auto built = nlohmann::json::parse(sf4e::session::BuildTournamentRequest(organizer));
        Check(built.value("organizer", false) == (op != IdentityOp::SiteCancel), "An organizer request was not marked for the helper");
    }
}

void Answers() {
    sf4e::netplay::IdentityView view;
    // The profile says whether the service offers Discord, and which sign-in sites it lists.
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":1,"op":"bridge_inspect","ok":true,"data":{
        "profile":{"bridge_id":"brg_1","origin":"https://bridge.embernetplay.link","display_name":"Ember"},
        "capabilities":{"connections":[],"features":["discord.accounts","sites"],"sign_in_sites":[
            {"id":"challonge","display_name":"Challonge","sign_in":true,"lookup_by":"username",
             "authorize_origin":"https://api.challonge.com","authorize_path":"/oauth/authorize","organizer":true},
            {"id":"startgg","display_name":"start.gg","sign_in":false,"lookup_by":"user_id",
             "authorize_origin":"https://start.gg","authorize_path":"/oauth/authorize"},
            {"display_name":"No ID"}, 7, {"id":"","display_name":"Empty"},
            {"id":"later","display_name":"Later","sign_in":"yes","lookup_by":"email"}]}}})"), view);
    Check(!view.inspectedDiscord && view.inspectedDiscordAccounts, "Discord's features were not read");
    Check(view.signInSites.size() == 3 && view.signInSites[0].id == "challonge" && view.signInSites[0].name == "Challonge" &&
        view.signInSites[0].signIn && view.signInSites[0].organizer && !view.signInSites[1].organizer &&
        view.signInSites[1].id == "startgg" && view.signInSites[1].name == "start.gg" && !view.signInSites[1].signIn,
        "The sign-in sites were not read");
    // An entry without an ID is skipped; a site whose sign_in is not a yes offers none.
    Check(view.signInSites[2].id == "later" && !view.signInSites[2].signIn, "A site entry was not read");
    nlohmann::json many = nlohmann::json::array();
    for (int n = 0; n < 40; ++n) many.push_back({{"id", "site-" + std::to_string(n)}, {"display_name", "Site"}, {"sign_in", true}});
    {
        sf4e::netplay::IdentityView listed;
        sf4e::session::ApplyTournamentEvent({{"request_id", 9}, {"op", "bridge_inspect"}, {"ok", true},
            {"data", {{"profile", nlohmann::json::object()}, {"capabilities", {{"sign_in_sites", many}}}}}}, listed);
        Check(listed.signInSites.size() == sf4e::netplay::MaxSignInSites && listed.signInSites.back().id == "site-15",
            "A long site list was not cut to the kept sites");
    }
    view.discordUser = "274220342558756145"; view.discordName = "player1";
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":2,"op":"site_status","ok":true,"data":{
        "account":{"user_id":"1001","username":"Player_1","connected_at":10}}})"), view);
    Check(view.siteUser == "1001" && view.siteName == "Player_1" && view.siteConnectedAt == 10, "The site's account was not read");
    Check(view.discordUser == "274220342558756145" && view.discordName == "player1", "A site's answer changed the Discord account");
    // An organizer read says whether the grant is held and leaves the account as read.
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":8,"op":"site_status","ok":true,"data":{
        "organizer":{"scope":"me","expires_at":99}}})"), view);
    Check(view.siteOrganizer && view.siteUser == "1001", "An organizer read was not read apart from the account");
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":9,"op":"site_remove","ok":true,"data":{"organizer":null}})"), view);
    Check(!view.siteOrganizer && view.siteUser == "1001", "Dropping organizer access was not read, or it unlinked the account");
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":3,"op":"site_remove","ok":true,"data":{"account":null}})"), view);
    Check(view.siteUser.empty() && view.siteName.empty() && view.discordName == "player1", "Unlinking a site's account was not read");
    Check(view.siteConnectedAt == 0, "Unlinking a site's account kept when it connected");
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":7,"op":"discord_remove","ok":true,"data":{"account":null}})"), view);
    Check(view.discordUser.empty() && view.discordConnectedAt == 0, "Unlinking Discord's account kept when it connected");
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":6,"op":"discord_status","ok":true,"data":{
        "account":{"user_id":"274220342558756145","username":"player1","connected_at":30}}})"), view);
    Check(view.discordConnectedAt == 30 && view.siteConnectedAt == 0, "When Discord's account connected was not read");
    // A failed read changes neither account.
    view.siteName = "Player_1";
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":4,"op":"site_status","ok":false,"reason":"rate_limited"})"), view);
    Check(view.siteName == "Player_1" && view.failure == "rate_limited", "A failed read changed the account");
    // A later profile without sites lists none.
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":5,"op":"bridge_inspect","ok":true,"data":{
        "profile":{"bridge_id":"brg_1","origin":"https://bridge.embernetplay.link","display_name":"Ember"},
        "capabilities":{"connections":[],"features":["discord"]}}})"), view);
    Check(view.inspectedDiscord && !view.inspectedDiscordAccounts && view.signInSites.empty(), "A profile without sites kept them");
}

// One classification decides what the runtime lets through during a live
// game and what the screens retire, read once, or send as cleanup.
void Effects() {
    using sf4e::netplay::IdentityEffect;
    using sf4e::netplay::IdentityOp;
    using sf4e::netplay::EffectOf;
    for (const auto op : {IdentityOp::Status, IdentityOp::BridgeList, IdentityOp::BridgeInspect, IdentityOp::LinkList,
             IdentityOp::DiscordStatus, IdentityOp::SiteStatus})
        Check(EffectOf(op) == IdentityEffect::Read, "A read is not a read, so a live game would refuse it");
    for (const auto op : {IdentityOp::DiscordConnect, IdentityOp::SiteConnect}) Check(EffectOf(op) == IdentityEffect::Open, "A sign-in does not open");
    for (const auto op : {IdentityOp::DiscordCancel, IdentityOp::SiteCancel}) Check(EffectOf(op) == IdentityEffect::Cleanup, "A cancel is not cleanup");
    for (const auto op : {IdentityOp::Enable, IdentityOp::Import, IdentityOp::Reset}) Check(EffectOf(op) == IdentityEffect::KeyChange, "A key change is not one");
    Check(EffectOf(IdentityOp::PreviewImport) == IdentityEffect::Preview, "A backup preview is not a preview");
    for (const auto op : {IdentityOp::Unlock, IdentityOp::Export, IdentityOp::BridgeApprove, IdentityOp::BridgeForget, IdentityOp::LinkClaim,
             IdentityOp::LinkCancel, IdentityOp::LinkRemove, IdentityOp::DiscordRemove, IdentityOp::SiteRemove})
        Check(EffectOf(op) == IdentityEffect::Change, "A change is classified as something else");
    // Each source's four requests map both ways.
    using sf4e::netplay::SignInSource;
    using sf4e::netplay::SignInStep;
    for (const auto source : {SignInSource::Discord, SignInSource::Site})
        for (const auto step : {SignInStep::Status, SignInStep::Connect, SignInStep::Remove, SignInStep::Cancel}) {
            const auto signIn = sf4e::netplay::SignInOf(sf4e::netplay::SignInRequest(source, step));
            Check(signIn && signIn->source == source && signIn->step == step, "A sign-in request does not map back to its source and step");
        }
    Check(!sf4e::netplay::SignInOf(IdentityOp::LinkList), "A link request is a sign-in");
}

// A profile answer listing `sites`.
nlohmann::json Inspected(const nlohmann::json& sites) {
    nlohmann::json event = {{"request_id", 1}, {"op", "bridge_inspect"}, {"ok", true}};
    event["data"]["profile"] = nlohmann::json::object();
    event["data"]["capabilities"]["sign_in_sites"] = sites;
    return event;
}

nlohmann::json SiteEntry(const std::string& id, const std::string& name) {
    return {{"id", id}, {"display_name", name}, {"sign_in", true}};
}

// Names are kept to 64 characters, as the helper counts them, and never cut
// inside a character.
void Names() {
    std::string name;
    for (int i = 0; i < 70; ++i) name += "\xe5\xa4\xa7";  // 3 bytes each
    const std::string twoByte = std::string(63, 'a') + "\xc3\xa9\xc3\xa9";
    sf4e::netplay::IdentityView view;
    sf4e::session::ApplyTournamentEvent(Inspected(nlohmann::json::array({SiteEntry("a", name), SiteEntry("b", twoByte)})), view);
    Check(view.signInSites.size() == 2 && view.signInSites[0].name == name.substr(0, 64 * 3), "A long name was not kept to 64 characters");
    Check(view.signInSites[1].name == std::string(63, 'a') + "\xc3\xa9", "A name was cut inside a character");
    nlohmann::json status = {{"request_id", 2}, {"op", "site_status"}, {"ok", true}};
    status["data"]["account"] = {{"user_id", "1"}, {"username", name}};
    sf4e::session::ApplyTournamentEvent(status, view);
    Check(view.siteName == name.substr(0, 64 * 3), "A long account name was not kept to 64 characters");
    Check(view.siteConnectedAt == 0, "A missing connection time was not 0");
    for (const auto& at : {nlohmann::json("soon"), nlohmann::json(-5), nlohmann::json(1.5)}) {
        status["data"]["account"]["connected_at"] = at;
        sf4e::session::ApplyTournamentEvent(status, view);
        Check(view.siteConnectedAt == 0, "A connection time that is no whole number was not 0");
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
        "The events read was not built");
    read.target = "oev_1";
    Check(nlohmann::json::parse(sf4e::session::BuildTournamentRequest(read)).value("event", "") == "oev_1", "A sets read lost its event");
    sf4e::netplay::IdentityRequest act; act.op = IdentityOp::DeskAct; act.bridge = "brg_1"; act.target = "/v1/organizer/events/oev_1/sets/7";
    act.command = R"({"held":true})";
    const auto built = nlohmann::json::parse(sf4e::session::BuildTournamentRequest(act));
    Check(built.value("path", "") == act.target && built["command"] == nlohmann::json({{"held", true}}), "A desk action was not built");
    act.command = "[1]";
    Check(sf4e::session::BuildTournamentRequest(act).empty(), "A desk action whose command is no object was built");

    sf4e::netplay::IdentityView view;
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":1,"op":"desk_read","ok":true,"data":{"events":[
        {"id":"oev_1","title":"Weekly","status":"running","site":"challonge"}, 5]}})"), view);
    Check(view.deskEvents.size() == 1 && view.deskEvents[0].title == "Weekly" && view.deskEvents[0].status == "running", "The events were not read");
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":2,"op":"desk_read","ok":true,"data":{"event":"oev_1","sets":[
        {"id":"7","round_label":"Losers Top 8","round":-3,"side":"losers","players":["Kuro",null],"score":[2,1],"desk_state":"needs_review",
         "match_id":"emt_1","revision":"4","held":true,"called_at":50,"posted":"failed",
         "games":[{"attempt_id":"att_1","outcome":null,"state":"review"}]},
        {"id":"8","score":"2-1"}]}})"), view);
    Check(view.deskEvent == "oev_1" && view.deskSets.size() == 2 && view.deskEvents.size() == 1, "The sets were not read, or they replaced the events");
    const auto& set = view.deskSets[0];
    Check(set.round == -3 && set.side == "losers" && set.players[0] == "Kuro" && set.players[1].empty() && set.scored &&
        set.score[0] == 2 && set.score[1] == 1 && set.state == "needs_review" && set.held && set.calledAt == 50 && set.posted == "failed",
        "A set was not read as the desk shows it");
    Check(set.games.size() == 1 && set.games[0].attempt == "att_1" && set.games[0].state == "review", "A set's games were not read");
    Check(!view.deskSets[1].scored, "A score that is no pair was taken");

    // The web desk's sessions: read, noted, and all signed out.
    Check(sf4e::netplay::EffectOf(IdentityOp::DeskWeb) == sf4e::netplay::IdentityEffect::Read, "A web sessions read is not a read");
    Check(sf4e::netplay::EffectOf(IdentityOp::DeskWebEnd) == sf4e::netplay::IdentityEffect::Change, "Signing the web out is not a change");
    sf4e::netplay::IdentityRequest web; web.op = IdentityOp::DeskWebEnd; web.bridge = "brg_1";
    Check(nlohmann::json::parse(sf4e::session::BuildTournamentRequest(web)) == nlohmann::json({{"op", "desk_web"}, {"bridge_id", "brg_1"}, {"end", true}}),
        "Signing the web out was not built");
    web.op = IdentityOp::DeskWebSeen;
    Check(nlohmann::json::parse(sf4e::session::BuildTournamentRequest(web)).value("seen", false), "Noting web sessions seen was not built");
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":3,"op":"desk_web","ok":true,"data":{"sessions":[
        {"site":"challonge","browser":"Chrome on a phone","started_at":10,"last_used_at":20,"new":true}, 5]}})"), view);
    Check(view.webSessions.size() == 1 && view.webSessions[0].browser == "Chrome on a phone" && view.webSessions[0].fresh &&
        view.webSessions[0].lastUsedAt == 20 && view.deskSets.size() == 2, "The web sessions were not read, or they replaced the desk");

    // Opening the web desk from the game in the browser, and approving the
    // code another device's browser shows.
    Check(sf4e::netplay::EffectOf(IdentityOp::DeskOpen) == sf4e::netplay::IdentityEffect::Open, "Opening the desk is not an open");
    sf4e::netplay::IdentityRequest open; open.op = IdentityOp::DeskOpen; open.bridge = "brg_1";
    Check(nlohmann::json::parse(sf4e::session::BuildTournamentRequest(open)) == nlohmann::json({{"op", "desk_handoff"}, {"bridge_id", "brg_1"}}),
        "Opening the desk was not built");
    sf4e::netplay::IdentityRequest approve; approve.op = IdentityOp::DeskApprove; approve.bridge = "brg_1"; approve.command = "ABCD EFGH";
    Check(nlohmann::json::parse(sf4e::session::BuildTournamentRequest(approve)) == nlohmann::json({{"op", "desk_approve"}, {"bridge_id", "brg_1"}, {"code", "ABCD EFGH"}}),
        "Approving another device was not built");
    // The code is read first, for what asked and the numbers to pick from,
    // and approved with the number picked.
    sf4e::netplay::IdentityRequest info; info.op = IdentityOp::DeskCodeInfo; info.bridge = "brg_1"; info.command = "123 456";
    Check(sf4e::netplay::EffectOf(IdentityOp::DeskCodeInfo) == sf4e::netplay::IdentityEffect::Read, "Reading a code is not a read");
    Check(nlohmann::json::parse(sf4e::session::BuildTournamentRequest(info)) == nlohmann::json({{"op", "desk_code_info"}, {"bridge_id", "brg_1"}, {"code", "123 456"}}),
        "Reading another device's code was not built");
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":4,"op":"desk_code_info","ok":true,"data":{
        "device":"Chrome on Android","requested_at":1700000000,"choices":[12,47,83,99]}})"), view);
    Check(view.deskDevice.device == "Chrome on Android" && view.deskDevice.requestedAt == 1700000000 &&
        view.deskDevice.choices == std::vector<int>{12, 47, 83}, "What asked with the code was not read, or more than three numbers were kept");
    approve.number = 47;
    Check(nlohmann::json::parse(sf4e::session::BuildTournamentRequest(approve)).value("number", 0) == 47, "The number picked was not sent");
    approve.command.clear();
    Check(sf4e::session::BuildTournamentRequest(approve).empty(), "An approval without a code was built");
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
