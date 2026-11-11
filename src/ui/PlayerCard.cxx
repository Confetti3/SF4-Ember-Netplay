#include "PlayerCard.hxx"
#include <algorithm>

namespace sf4e { namespace ui { namespace card {
std::vector<int> PlayedFighters(const Own& own) {
    std::vector<int> played;
    for (int fighter = 0; fighter < static_cast<int>(own.fighters.size()); ++fighter)
        if (own.fighters[fighter].level) played.push_back(fighter);
    std::sort(played.begin(), played.end(), [&](int a, int b) {
        const auto &x = own.fighters[a], &y = own.fighters[b];
        if (x.level != y.level) return x.level > y.level;
        return x.games != y.games ? x.games > y.games : a < b;
    });
    return played;
}

std::uint64_t Points(const FighterLevel& level) { return level.games + level.wins; }

float Part(const FighterLevel& level) {
    if (!level.next) return 1.f;
    const auto points = Points(level);
    if (points <= level.floor || level.next <= level.floor) return 0.f;
    return (std::min)(1.f, static_cast<float>(points - level.floor) / static_cast<float>(level.next - level.floor));
}

std::uint64_t GamesAway(const FighterLevel& level) {
    const auto points = Points(level);
    if (!level.next || !level.games || !points) return 0;
    const auto remaining = level.next > points ? level.next - points : 0;
    if (!remaining) return 0;
    // Games are points over this fighter's own points per game, rounded up.
    return (remaining * level.games + points - 1) / points;
}

void Fill(PlayerCardView& view, const netplay::Answered<Own>& answer, int main) {
    view.fighters.clear();
    view.progress = {};
    view.facts = {};
    if (!answer.loaded) {
        // A read that failed with nothing before it has nothing to show.
        view.levels = answer.loading || answer.error.empty() ? CardLevels::Pending : CardLevels::Unavailable;
        return;
    }
    const Own& own = answer.value;
    view.levels = own.kind == Own::Kind::NoAccount ? CardLevels::NoAccount : own.kind == Own::Kind::Counted ? CardLevels::Counted : CardLevels::Unavailable;
    if (view.levels != CardLevels::Counted) return;
    view.levelsShown = own.shown;
    const auto add = [&](int fighter) { view.fighters.push_back({fighter, own.fighters[fighter].level, own.fighters[fighter].games}); };
    const bool hasMain = main >= 0 && main < static_cast<int>(own.fighters.size());
    if (hasMain) add(main);
    for (const int fighter : PlayedFighters(own)) {
        if (view.fighters.size() >= 3) break;
        if (fighter != main) add(fighter);
    }
    if (hasMain && own.fighters[main].level) {
        const auto& level = own.fighters[main];
        view.progress = {level.level, Points(level), level.floor, level.next};
    }
    std::uint64_t wins = 0;
    for (const auto& level : own.fighters) {
        view.facts.games += level.games;
        wins += level.wins;
        view.facts.fightersPlayed += level.level ? 1 : 0;
    }
    view.facts.winPercent = view.facts.games ? static_cast<int>((wins * 100 + view.facts.games / 2) / view.facts.games) : 0;
}
} } }
