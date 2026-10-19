// The identity requests the interface sends become the helper's tournament
// JSON, and the helper's answers fold into the view: here, the browser
// sign-ins Discord and the listed sign-in sites share, each into its own fields.
#include "../session/IdentityEvents.hxx"
#include <nlohmann/json.hpp>
#include <fstream>
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
    }
}

void Answers() {
    sf4e::netplay::IdentityView view;
    // The profile says whether the service offers Discord, and which sign-in sites it lists.
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":1,"op":"bridge_inspect","ok":true,"data":{
        "profile":{"bridge_id":"brg_1","origin":"https://bridge.embernetplay.link","display_name":"Ember"},
        "capabilities":{"connections":[],"features":["discord.accounts","sites"],"sign_in_sites":[
            {"id":"challonge","display_name":"Challonge","sign_in":true,"lookup_by":"username",
             "authorize_origin":"https://api.challonge.com","authorize_path":"/oauth/authorize"},
            {"id":"startgg","display_name":"start.gg","sign_in":false,"lookup_by":"user_id",
             "authorize_origin":"https://start.gg","authorize_path":"/oauth/authorize"},
            {"display_name":"No ID"}, 7, {"id":"Bad/Id","display_name":"Bad","sign_in":true},
            {"id":"","display_name":"Empty"}, {"id":"abcdefghijklmnopqrstuvwxyz0123456","display_name":"Too long"},
            {"id":"challonge","display_name":"Second Challonge","sign_in":false},
            {"id":"later","display_name":"Later","sign_in":"yes","lookup_by":"email"}]}}})"), view);
    Check(!view.inspectedDiscord && view.inspectedDiscordAccounts, "Discord's features were not read");
    Check(view.signInSites.size() == 3 && view.signInSites[0].id == "challonge" && view.signInSites[0].name == "Challonge" &&
        view.signInSites[0].signIn &&
        view.signInSites[1].id == "startgg" && view.signInSites[1].name == "start.gg" && !view.signInSites[1].signIn,
        "The sign-in sites were not read");
    // Only IDs a site can have, each once; a site whose sign_in is not a yes offers none.
    Check(view.signInSites[2].id == "later" && !view.signInSites[2].signIn, "A site entry was not checked");
    nlohmann::json many = nlohmann::json::array();
    for (int n = 0; n < 40; ++n) many.push_back({{"id", "site-" + std::to_string(n)}, {"display_name", "Site"}, {"sign_in", true}});
    {
        sf4e::netplay::IdentityView listed;
        sf4e::session::ApplyTournamentEvent({{"request_id", 9}, {"op", "bridge_inspect"}, {"ok", true},
            {"data", {{"profile", nlohmann::json::object()}, {"capabilities", {{"sign_in_sites", many}}}}}}, listed);
        Check(listed.signInSites.size() == sf4e::netplay::MaxSignInSites && listed.signInSites.back().id == "site-15",
            "A long site list was not cut to the kept sites");
    }
    view.discordUser = "274220342558756145"; view.discordName = "kate";
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":2,"op":"site_status","ok":true,"data":{
        "account":{"user_id":"1001","username":"Kate_99","connected_at":10}}})"), view);
    Check(view.siteUser == "1001" && view.siteName == "Kate_99", "The site's account was not read");
    Check(view.discordUser == "274220342558756145" && view.discordName == "kate", "A site's answer changed the Discord account");
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":3,"op":"site_remove","ok":true,"data":{"account":null}})"), view);
    Check(view.siteUser.empty() && view.siteName.empty() && view.discordName == "kate", "Unlinking a site's account was not read");
    // A failed read changes neither account.
    view.siteName = "Kate_99";
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":4,"op":"site_status","ok":false,"reason":"rate_limited"})"), view);
    Check(view.siteName == "Kate_99" && view.failure == "rate_limited", "A failed read changed the account");
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

// A site ID is kept exactly when the helper and the bridge take it: the
// cases are the ones the helper's tests read too.
void SharedSiteIds() {
    std::ifstream file(SF4E_SITE_IDS);
    Check(file.good(), "The shared site IDs could not be read");
    const auto cases = nlohmann::json::parse(file);
    const auto listed = [](const std::string& id) {
        sf4e::netplay::IdentityView view;
        sf4e::session::ApplyTournamentEvent(Inspected(nlohmann::json::array({SiteEntry(id, "Site")})), view);
        return view.signInSites.size() == 1 && view.signInSites[0].id == id;
    };
    Check(!cases.at("valid").empty() && !cases.at("invalid").empty(), "The shared site IDs are empty");
    for (const auto& id : cases.at("valid")) Check(listed(id.get<std::string>()), "A valid site ID was skipped");
    for (const auto& id : cases.at("invalid")) Check(!listed(id.get<std::string>()), "An invalid site ID was kept");
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
}
}

int main() {
    try {
        Requests(); Answers(); Effects(); SharedSiteIds(); Names();
        std::cout << "Identity events passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
