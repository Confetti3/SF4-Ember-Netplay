// The identity requests the interface sends become the helper's tournament
// JSON, and the helper's answers fold into the view: here, the browser
// sign-ins Discord and Challonge share, each into its own site's fields.
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
    const std::pair<IdentityOp, const char*> ops[] = {
        {IdentityOp::DiscordStatus, "discord_status"}, {IdentityOp::DiscordConnect, "discord_connect"},
        {IdentityOp::DiscordRemove, "discord_remove"}, {IdentityOp::DiscordCancel, "discord_cancel"},
        {IdentityOp::ChallongeStatus, "challonge_status"}, {IdentityOp::ChallongeConnect, "challonge_connect"},
        {IdentityOp::ChallongeRemove, "challonge_remove"}, {IdentityOp::ChallongeCancel, "challonge_cancel"},
    };
    for (const auto& [op, name] : ops) {
        Check(Request(op) == nlohmann::json({{"op", name}, {"bridge_id", "brg_1"}}), "A sign-in request was not built for the helper");
        Check(Request(op, "").is_null(), "A sign-in request without its service was built");
    }
}

void Answers() {
    sf4e::netplay::IdentityView view;
    // The profile says which sites the service offers sign-in for and keeps accounts of.
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":1,"op":"bridge_inspect","ok":true,"data":{
        "profile":{"bridge_id":"brg_1","origin":"https://bridge.embernetplay.link","display_name":"Ember"},
        "capabilities":{"connections":[],"features":["discord.accounts","challonge","challonge.accounts"]}}})"), view);
    Check(!view.inspectedDiscord && view.inspectedDiscordAccounts && view.inspectedChallonge && view.inspectedChallongeAccounts, "The sign-in features were not read");
    view.discordUser = "274220342558756145"; view.discordName = "kate";
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":2,"op":"challonge_status","ok":true,"data":{
        "account":{"user_id":"1001","username":"Kate_99","connected_at":10}}})"), view);
    Check(view.challongeUser == "1001" && view.challongeName == "Kate_99", "The Challonge account was not read");
    Check(view.discordUser == "274220342558756145" && view.discordName == "kate", "A Challonge answer changed the Discord account");
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":3,"op":"challonge_remove","ok":true,"data":{"account":null}})"), view);
    Check(view.challongeUser.empty() && view.challongeName.empty() && view.discordName == "kate", "Disconnecting Challonge was not read");
    // A failed read changes neither account.
    view.challongeName = "Kate_99";
    sf4e::session::ApplyTournamentEvent(nlohmann::json::parse(R"({"request_id":4,"op":"challonge_status","ok":false,"reason":"rate_limited"})"), view);
    Check(view.challongeName == "Kate_99" && view.failure == "rate_limited", "A failed read changed the account");
}
}

int main() {
    try {
        Requests(); Answers();
        std::cout << "Identity events passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
