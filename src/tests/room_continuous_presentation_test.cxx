#include "../ui/ApplicationShell.hxx"
#include "../ui/ChatTranscript.hxx"
#include "../ui/RoomControls.hxx"
#include "../common/Localization.hxx"
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

int main() try {
    using namespace sf4e;
    ui::ShellView view;
    view.room.roomEpoch = 42;
    view.room.localMember = 3; // Someone waiting in the room, not a fighter.
    room::Member first; first.id = 1; first.name = "First";
    room::Member second; second.id = 2; second.name = "Second";
    view.room.members = {first, second};
    auto& table = view.room.tables[0];
    table.p1 = first.id; table.p2 = second.id;
    table.phase = room::TablePhase::Playing;
    table.matchGeneration = 7;
    table.continuous.enabled = true;
    ui::ChatTranscript transcript;
    transcript.Update(view.room, {});
    const auto banner = [&] { return ui::room_controls::DescribeTableBanner(view, table); };
    Check(banner().text.empty(), "A fresh set announced a previous result");
    table.resultPending = true;
    Check(banner().text == loc::T("room.continuous.confirming") && banner().seat == -1,
        "One fighter's report announced a winner before agreement");
    transcript.Update(view.room, {});
    Check(transcript.Lines().empty(), "A pending result awarded a chat win");
    table.resultPending = false;
    table.continuous.games = 1; table.continuous.lastResult = room::MatchResult::P2Win;
    table.score[1] = 1;
    Check(banner().text == loc::Tf("room.continuous.won", 1, "Second", "First") && banner().seat == 1,
        "The room did not identify both players after the second player won");
    transcript.Update(view.room, {}); transcript.Update(view.room, {});
    Check(transcript.Lines().size() == 1 && transcript.Lines().back().kind == ui::ChatLine::Kind::GameWon &&
        transcript.Lines().back().sender == 2 && transcript.Lines().back().score[0] == 1,
        "A resident rematch result did not update the room score exactly once");
    Check(ui::room_controls::DescribeTableBanner(view, view.room.tables[1]).text.empty(),
        "A result leaked into another table");
    table.continuous.games = 2; table.continuous.lastResult = room::MatchResult::Draw;
    Check(banner().text == loc::Tf("room.continuous.draw", 2) && banner().seat == -1,
        "A draw retained the previous winner");
    table.continuous.games = 3; table.continuous.lastResult = room::MatchResult::P1Win;
    table.score[0] = 1; transcript.Update(view.room, {});
    Check(banner().seat == 0 && transcript.Lines().size() == 2,
        "The next winner did not update without a new transport generation");
    Check(table.matchGeneration == 7 && table.phase == room::TablePhase::Playing,
        "Presentation ended the live transport");
    table.continuous.closed = true;
    Check(banner().text.empty(), "A closed generation retained its result banner");
    table.continuous = {}; table.phase = room::TablePhase::Waiting;
    Check(banner().text.empty(), "A new waiting matchup retained an old winner");
    std::cout << "Continuous results remain visible and confirmed in the room.\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
