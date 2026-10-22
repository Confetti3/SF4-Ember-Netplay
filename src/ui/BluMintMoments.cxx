#include "BluMintMoments.hxx"
#include "../common/Localization.hxx"

namespace sf4e { namespace ui {
NoticeDialog BluMintNotice(const BluMintScene& scene) {
    NoticeDialog notice;
    const auto& s = scene;
    // "FRaccie 2 - 1 Akira · First to 2" in its own box, the round and the game or set over it.
    notice.facts = loc::Tf("blumint.score", s.names[0], s.wins[0], s.wins[1], s.names[1], s.gamesToWin);
    const unsigned played = s.wins[0] + s.wins[1];
    const auto eyebrow = [&](const std::string& what) { notice.eyebrow = s.round.empty() ? what : s.round + " \xC2\xB7 " + what; };
    const auto tone = [](Tone t) { return ImGui::ColorConvertFloat4ToU32(ToneColor(t)); };
    const auto& c = v2::Colors();
    const bool player = s.you == 0 || s.you == 1;
    const int leader = s.wins[0] == s.wins[1] ? -1 : s.wins[0] > s.wins[1] ? 0 : 1;
    switch (s.moment) {
    case BluMintMoment::GameDone:
        eyebrow(loc::Tf("blumint.eyebrow_game", played));
        notice.chip = {loc::Tf("blumint.first_to", s.gamesToWin), c.accent};
        notice.heading = loc::Tf("blumint.game_heading", played);
        notice.text = loc::Tf("blumint.game_next", played + 1);
        if (player) notice.buttons = {{loc::T("blumint.ready_next"), NoticeResult::BluMintReady},
            {loc::T("blumint.change_fighter"), NoticeResult::BluMintFighter}, {loc::T("blumint.not_yet")}};
        break;
    case BluMintMoment::SetDone: {
        const bool won = player && leader == s.you;
        eyebrow(loc::T("blumint.eyebrow_set"));
        notice.chip = player ? v2::HeaderChip{loc::T(won ? "blumint.chip_set_won" : "blumint.set_lost"), won ? c.accent : c.muted}
                             : v2::HeaderChip{loc::T("blumint.stage_decided"), c.accent};
        notice.heading = player && leader >= 0 ? loc::T(won ? "blumint.set_won" : "blumint.set_lost")
                                               : loc::Tf("blumint.set_winner", leader >= 0 ? s.names[leader] : std::string());
        notice.text = std::string(loc::T("blumint.set_reported")) + " " +
            (s.next.empty() ? std::string(loc::T(player ? "blumint.next_waiting" : "blumint.watch_next")) : std::string(loc::T("blumint.next_ready")));
        if (player && !s.next.empty()) notice.buttons = {{loc::T("blumint.play_next"), NoticeResult::BluMintNext}, {loc::T("blumint.later")}};
        break;
    }
    case BluMintMoment::Restart:
        eyebrow(loc::Tf("blumint.eyebrow_game", played + 1));
        notice.chip = {loc::T("blumint.chip_restart"), tone(Tone::Pending)};
        notice.heading = loc::T("blumint.restart_heading");
        notice.text = loc::Tf("blumint.restart_text", s.wins[0], s.wins[1]);
        break;
    case BluMintMoment::HostTook:
        eyebrow(loc::T("blumint.eyebrow_set"));
        notice.chip = {loc::T("blumint.role_host"), tone(Tone::Pending)};
        notice.heading = loc::T("blumint.host_heading");
        notice.text = loc::T("blumint.host_text");
        break;
    case BluMintMoment::Expired:
        eyebrow(loc::T("blumint.eyebrow_set"));
        notice.chip = {loc::T("blumint.chip_expired"), tone(Tone::Error)};
        notice.heading = loc::T("blumint.expired_heading");
        notice.text = loc::T("blumint.expired_text");
        break;
    case BluMintMoment::Called:
        // Not yet in the match's room: no score, only the round and the set length.
        notice.facts.clear();
        notice.eyebrow = loc::T("blumint.eyebrow");
        if (!s.round.empty()) notice.eyebrow += " \xC2\xB7 " + s.round;
        if (s.gamesToWin) notice.chip = {loc::Tf("blumint.first_to", s.gamesToWin), c.accent};
        notice.heading = loc::T("blumint.called_heading");
        notice.text = loc::T(s.inRoom ? "blumint.called_text_room" : "blumint.called_text");
        notice.buttons = {{loc::T(s.inRoom ? "blumint.leave_play" : "blumint.play_now"), NoticeResult::BluMintNext}, {loc::T("blumint.later")}};
        break;
    case BluMintMoment::Failed:
        eyebrow(loc::T("blumint.eyebrow_set"));
        notice.chip = {loc::T("blumint.chip_stopped"), tone(Tone::Error)};
        notice.heading = loc::T("blumint.failed_heading");
        notice.text = loc::Tf("blumint.failed_text", s.failure);
        break;
    }
    // A watcher only reads: one button that closes it.
    if (notice.buttons.empty()) notice.buttons = {{loc::T("common.ok")}};
    return notice;
}
} }
