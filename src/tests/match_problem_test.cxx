#include "../session/sf4e__SessionProtocol.hxx"
#include <iostream>
#include "test_support.hxx"

using sf4e::SessionProtocol::FindMatchProblem;
using sf4e::SessionProtocol::LobbyData;
using sf4e::SessionProtocol::MatchData;
using sf4e::SessionProtocol::MatchProblem;

// A room's match is checked as the game would take it, before any of it is
// written: its round count and time, its stage, and both fighters.
int main() {
    LobbyData lobby;
    MatchData match;
    match.stageID = 0;
    for (auto& chara : match.chara) { chara.charaID = 0; chara.ultraCombo = 0; chara.unc_edition = 14; }
    CHECK(FindMatchProblem(lobby, match) == MatchProblem::None);

    for (const int rounds : {0, 2, 4, 100, -1}) {
        LobbyData bad = lobby; bad.roundCount = rounds;
        CHECK(FindMatchProblem(bad, match) == MatchProblem::Settings);
    }
    for (const int time : {0, 45, 100}) {
        LobbyData bad = lobby; bad.roundTime.integral = time;
        CHECK(FindMatchProblem(bad, match) == MatchProblem::Settings);
    }
    { LobbyData bad = lobby; bad.roundTime.fractional = 1; CHECK(FindMatchProblem(bad, match) == MatchProblem::Settings); }

    { MatchData bad = match; bad.stageID = 9999; CHECK(FindMatchProblem(lobby, bad) == MatchProblem::Stage); }
    { MatchData bad = match; bad.stageID = -1; CHECK(FindMatchProblem(lobby, bad) == MatchProblem::Stage); }

    for (const int side : {0, 1}) {
        MatchData bad = match; bad.chara[side].charaID = 200;
        CHECK(FindMatchProblem(lobby, bad) == MatchProblem::Fighter);
        bad = match; bad.chara[side].costume = 250;
        CHECK(FindMatchProblem(lobby, bad) == MatchProblem::Fighter);
        bad = match; bad.chara[side].color = 250;
        CHECK(FindMatchProblem(lobby, bad) == MatchProblem::Fighter);
        bad = match; bad.chara[side].unc_edition = 99;
        CHECK(FindMatchProblem(lobby, bad) == MatchProblem::Fighter);
        bad = match; bad.chara[side].handicap = 9;
        CHECK(FindMatchProblem(lobby, bad) == MatchProblem::Fighter);
    }
    // With edition select off, only the Ultra edition is a fighter's.
    { LobbyData plain = lobby; plain.editionSelect = false; MatchData older = match; older.chara[1].unc_edition = 0;
      CHECK(FindMatchProblem(plain, older) == MatchProblem::Fighter); }

    std::cout << "Match problems: settings, stage and either fighter refused; a valid match passes\n";
}
