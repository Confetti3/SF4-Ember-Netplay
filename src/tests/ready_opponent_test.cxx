#include "../common/ReadyOpponent.hxx"
#include "test_support.hxx"
#include <array>
#include <vector>

struct Member { std::uint64_t id; int fighter; };
struct Table { int id; std::uint64_t p1, p2; };
struct Snapshot {
    std::uint64_t roomEpoch = 7, localMember = 10;
    std::array<Table, 2> tables{{{0,10,20},{1,30,40}}};
    std::vector<Member> members{{10,0},{20,1},{30,2},{40,3}};
};
int main() {
    using sf4e::room::CaptureReadyOpponent;
    Snapshot s;
    const auto consent = CaptureReadyOpponent(s);
    CHECK(consent.table == 0 && consent.seat == 0 && consent.opponent == 20 && consent.fighter == 1);
    s.members[0].fighter = 43; CHECK(CaptureReadyOpponent(s) == consent);
    s.members[2].fighter = 10; CHECK(CaptureReadyOpponent(s) == consent);
    s.members[1].fighter = 2; CHECK(CaptureReadyOpponent(s) != consent);
    s.members[1].fighter = 1; CHECK(CaptureReadyOpponent(s) == consent);
    s.tables[0].p2 = 40; CHECK(CaptureReadyOpponent(s) != consent);
    s.tables[0].p2 = 20;
    s.localMember = 20; CHECK(CaptureReadyOpponent(s) != consent);
    s.localMember = 10; ++s.roomEpoch; CHECK(CaptureReadyOpponent(s) != consent);
    --s.roomEpoch; s.tables[0].p1 = 0; CHECK(CaptureReadyOpponent(s) != consent);
    s.tables[0].p1 = 10; s.members[1].fighter = -1;
    const auto unknown = CaptureReadyOpponent(s);
    CHECK(unknown != consent);
    s.members[1].fighter = 1; CHECK(CaptureReadyOpponent(s) != unknown);
    s.roomEpoch = 0; CHECK(CaptureReadyOpponent(s) == sf4e::room::ReadyOpponent{});
    return 0;
}
