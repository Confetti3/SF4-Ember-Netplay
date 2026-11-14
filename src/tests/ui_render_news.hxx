#pragma once
// The News hub's shots of ui_render_test.cxx. `draw` and `page` are the test's own; `retheme` applies the theme at an
// Interface size (the factor on the display scale) with the device objects remade. The releases are the UI lab's
// (src/ui/lab/UiLabNews.hxx): five bodies at the update check's 2,000-byte cap, then the same list with a release whose
// title and tag are far longer than a card and whose notes hold a word with no space in it.
//
// Overlap and clipping are read from the boxes the cards and the pane report (SetMenuCardProbe: news-chip, news-date,
// news-new and news-title inside each card; news-hero-chip, news-hero-title, news-hero-date and news-note in the pane) and
// from the text probe, which the test's overflow set already reads for the date (a cut date fails the run). A part that
// leaves its card, two that cross, a hero chip over its title, a note over the date or over the note before it, a pane
// drawn over the open Reader: each fails with the size, the Interface size, the release and the two parts.
#include "ui_render_support.hxx"
#include "../ui/ApplicationShell.hxx"
#include "../ui/MenuProbes.hxx"
#include "../ui/Theme.hxx"
#include "../ui/V2.hxx"
#include "../ui/lab/UiLabNews.hxx"
#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
namespace {
struct NewsBox {
    std::string id;
    ImVec2 min, max;
};
// A pixel of slack: a measured text is rounded up.
inline bool NewsCross(const NewsBox& a, const NewsBox& b) {
    return a.min.x < b.max.x - 1 && b.min.x < a.max.x - 1 && a.min.y < b.max.y - 1 && b.min.y < a.max.y - 1;
}
inline bool NewsInside(const NewsBox& part, ImVec2 min, ImVec2 max) {
    return part.min.x >= min.x - 1 && part.max.x <= max.x + 1 && part.min.y >= min.y - 1 && part.max.y <= max.y + 1;
}
// One frame's reports, split into the cards (each with its parts) and the pane's.
struct NewsFrame {
    struct Card {
        NewsBox box;
        std::vector<NewsBox> parts;
    };
    std::vector<Card> cards;
    std::vector<NewsBox> pane;
    void Add(const char* id, ImVec2 min, ImVec2 max) {
        const std::string name = id;
        if (name.compare(0, 5, "news-") != 0) return;
        const bool card = name.size() > 5 && name[5] >= '0' && name[5] <= '9';
        if (card) cards.push_back({{name, min, max}, {}});
        else if (name.compare(0, 10, "news-hero-") == 0 || name == "news-note") pane.push_back({name, min, max});
        else if (!cards.empty()) cards.back().parts.push_back({name, min, max});
    }
};
// What the frame drew must hold together: `where` says the size, the Interface size and the release.
inline void CheckNewsFrame(const NewsFrame& frame, const std::string& where) {
    for (const auto& card : frame.cards) {
        for (std::size_t i = 0; i < card.parts.size(); ++i) {
            const auto& part = card.parts[i];
            Require(NewsInside(part, card.box.min, card.box.max), (where + ": " + part.id + " leaves its card " + card.box.id).c_str());
            for (std::size_t j = i + 1; j < card.parts.size(); ++j)
                Require(!NewsCross(part, card.parts[j]), (where + ": " + part.id + " crosses " + card.parts[j].id + " in " + card.box.id).c_str());
        }
    }
    const NewsBox* chip = nullptr;
    const NewsBox* title = nullptr;
    const NewsBox* date = nullptr;
    const NewsBox* before = nullptr;
    for (const auto& box : frame.pane) {
        if (box.id == "news-hero-chip") chip = &box;
        else if (box.id == "news-hero-title") title = &box;
        else if (box.id == "news-hero-date") date = &box;
    }
    if (!title) return;
    Require(chip && date, (where + ": the pane has a title and no chip or date").c_str());
    Require(!NewsCross(*chip, *title), (where + ": the hero's chip crosses its title").c_str());
    Require(chip->max.y <= title->min.y + 1, (where + ": the hero's chip is not above its title").c_str());
    Require(date->min.y >= title->max.y - 1, (where + ": the date and tag start inside the hero").c_str());
    before = date;
    for (const auto& box : frame.pane) {
        if (box.id != "news-note") continue;
        Require(box.min.y >= before->max.y - 1, (where + ": a note starts over the " + (before == date ? "date" : "note before it")).c_str());
        before = &box;
    }
}
// News walked at the current theme: every release focused in turn (its card, its pane), the Reader opened on the first.
template<class Draw, class Page>
void WalkNews(sf4e::ui::ApplicationShell& shell, const Draw& draw, const Page& page, NewsFrame& frame, const std::string& where, const std::string& shot,
    std::size_t count, bool paneExpected) {
    using namespace sf4e;
    using namespace ui;
    page("news");
    Require(shell.Navigation().Screen() == "news" && shell.Navigation().Focus() == "news-0", (where + ": News did not open on its first release").c_str());
    bool paneSeen = false;
    for (std::size_t i = 0; i < count; ++i) {
        if (i) {
            draw(nullptr, MenuInput::Down, 1);
            draw(nullptr, 0, 1);
        }
        Require(shell.Navigation().Focus() == "news-" + std::to_string(i), (where + ": Down did not reach release " + std::to_string(i)).c_str());
        draw(nullptr, 0, 2);
        frame = NewsFrame{};
        draw(i == 0 || i + 1 == count ? (shot + "-" + std::to_string(i)).c_str() : nullptr, 0, 1);
        CheckNewsFrame(frame, where + " release " + std::to_string(i));
        for (const auto& box : frame.pane) paneSeen = paneSeen || box.id == "news-hero-title";
    }
    Require(!paneExpected || paneSeen, (where + ": the focused release's pane was not drawn").c_str());
    // Select reads the focused release in full; the pane steps aside for it (it would print the title and notes twice).
    shell.Navigation().Home();
    page("news");
    draw(nullptr, MenuInput::Select, 1);
    draw(nullptr, 0, 2);
    Require(shell.Navigation().Reading(), (where + ": Select did not open the release in the Reader").c_str());
    frame = NewsFrame{};
    draw((shot + "-reader").c_str(), 0, 1);
    Require(frame.pane.empty(), (where + ": the pane was drawn over the open Reader").c_str());
    draw(nullptr, MenuInput::Down, 12);
    draw(nullptr, MenuInput::Back, 1);
    draw(nullptr, 0, 2);
    Require(!shell.Navigation().Reading(), (where + ": Back did not close the Reader").c_str());
}
template<class Draw, class Page, class Retheme>
void ShootNews(sf4e::ui::ApplicationShell& shell, sf4e::ui::ShellView& view, const Draw& draw, const Page& page, const Retheme& retheme,
    bool sweepInterfaceSizes, int width, int height, float dpi) {
    using namespace sf4e;
    using namespace ui;
    const auto saved = view.services.update.news;
    NewsFrame frame;
    SetMenuCardProbe([&](const char* id, ImVec2 min, ImVec2 max) { frame.Add(id, min, max); });
    // At 1280x720 and 1920x1080 the Interface size is swept; every other size is drawn as it is.
    const std::vector<float> sizes = sweepInterfaceSizes ? std::vector<float>{.7f, 1.f, 1.5f} : std::vector<float>{1.f};
    const std::string at = std::to_string(width) + "x" + std::to_string(height) + "@" + std::to_string(static_cast<int>(dpi * 100));
    for (const float interfaceSize : sizes) {
        if (sweepInterfaceSizes) {
            retheme(interfaceSize);
            draw(nullptr, 0, 4);
        }
        const std::string size = at + " interface " + std::to_string(static_cast<int>(interfaceSize * 100 + .5f)) + "%";
        const std::string tag = "s" + std::to_string(static_cast<int>(interfaceSize * 100 + .5f));
        // The pane needs a content width of 820 logical pixels (GameMenu::DrawCards); the sizes that give it must show it.
        const float room = static_cast<float>(width) - 2 * ui::v2::SideMargin(static_cast<float>(width), ui::Scale());
        const bool paneExpected = room >= 840 * ui::Scale();
        for (const bool stress : {false, true}) {
            view.services.update.news = uilab::NewsReleases();
            if (stress) uilab::AddNewsStress(view.services.update.news);
            const std::size_t count = view.services.update.news.size();
            for (const auto& release : view.services.update.news)
                Require(release.notes.size() > 1000 && release.notes.size() <= 2003, "A News sample release is not a body at the update check's cap");
            WalkNews(shell, draw, page, frame, size + (stress ? " stress" : " five releases"), "news-" + tag + (stress ? "-stress" : ""), count, paneExpected);
            shell.Navigation().Home();
        }
        // The other tabs, with the list as it is.
        for (const char* tab : {"news-updates", "news-changelog", "notifications"}) {
            page(tab);
            Require(shell.Navigation().Screen() == tab, (size + ": the " + tab + " tab did not open").c_str());
            draw(nullptr, 0, 2);
        }
        shell.Navigation().Home();
    }
    if (sweepInterfaceSizes) {
        retheme(1.f);
        draw(nullptr, 0, 4);
    }
    SetMenuCardProbe({});
    view.services.update.news = saved;
    shell.Navigation().Home();
}
}
