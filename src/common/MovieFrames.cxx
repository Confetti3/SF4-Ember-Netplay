#include "MovieFrames.hxx"
#include "MenuBackdropRules.hxx"

#include <algorithm>
#include <set>
#include <utility>

namespace sf4e { namespace movie_frames {

const std::vector<Frame>& Curated() {
    // Chosen from contact sheets of every prologue and the two openings
    // (2026-11-20): scenery, wide shots and small figures; no lettering, no
    // fades, no face filling the screen, at least half a second from a cut.
    // No ending, ever. Kate may trim it; the tests keep it to the rules.
    static const std::vector<Frame> frames = {
        {"movie/prologue/ADN.wmv", 6000}, // boxing arena ringside, Adon small
        {"movie/prologue/BLK.wmv", 8000}, // night ship deck under clouds
        {"movie/prologue/BLR.wmv", 8000}, // neon skyline through window, silhouette
        {"movie/prologue/BSN.wmv", 28000}, // cyan neon rooftop, small figures
        {"movie/prologue/CHB.wmv", 8000}, // desert road and mountains, sidecar bike
        {"movie/prologue/CHB.wmv", 48000}, // seaside road, turquoise ocean, bike
        {"movie/prologue/CHB.wmv", 84000}, // sunlit desert dunes, sidecar bike
        {"movie/prologue/CMY.wmv", 36000}, // night dock, liner ship, Cammy leaping
        {"movie/prologue/DDL.wmv", 8000}, // English manor garden, gazebo, butler
        {"movie/prologue/DJY.wmv", 28500}, // warm wooden bar interior, patrons small
        {"movie/prologue/DSM.wmv", 12000}, // sunlit village square, crowd, Dhalsim small
        {"movie/prologue/GKI.wmv", 4000}, // night mountain range under stars
        {"movie/prologue/GKN.wmv", 20000}, // forest cliff, two fighters small
        {"movie/prologue/GKN.wmv", 26000}, // rocky cliff and mountains, figure small
        {"movie/prologue/GUL.wmv", 56000}, // forest clearing, gravestone, Guile walking
        {"movie/prologue/GUY.wmv", 8000}, // moon over scaffolding, tiny figure
        {"movie/prologue/GUY.wmv", 36000}, // moonlit city skyline, green mist
        {"movie/prologue/GUY.wmv", 48000}, // moonlit rooftops, running silhouette
        {"movie/prologue/HKN.wmv", 4000}, // sunlit street, domes, crowd, Hakan
        {"movie/prologue/HWK.wmv", 4000}, // sunlit canyon, village circle
        {"movie/prologue/HWK.wmv", 38000}, // sunlit forest glade, figures seated
        {"movie/prologue/IBK.wmv", 20000}, // moonlit rooftop, Ibuki crouching small
        {"movie/prologue/IBK.wmv", 53000}, // full moon over tiled temple roof
        {"movie/prologue/JHA.wmv", 36000}, // rainbow over old town bridge, figure
        {"movie/prologue/KEN.wmv", 4000}, // red rock desert canyon aerial
        {"movie/prologue/KEN.wmv", 14000}, // desert gas station and cars aerial
        {"movie/prologue/MKT.wmv", 4000}, // rainy mountain dojo exterior
        {"movie/prologue/ROS.wmv", 14000}, // metal-walled station, woman standing small
        {"movie/prologue/RYU.wmv", 18000}, // waterfall cliff path, Ryu small
        {"movie/prologue/RYU.wmv", 26000}, // forest canopy, distant mountain cliffs
        {"movie/prologue/RYU.wmv", 38000}, // sunlit forest, Ryu walking small
        {"movie/prologue/SGT.wmv", 8000}, // jungle clearing, Sagat small, mountains
        {"movie/prologue/SGT.wmv", 40000}, // jungle rock under blue sky, Sagat
        {"movie/prologue/SKR.wmv", 14000}, // ship deck over blue sea, crate
        {"movie/prologue/SKR.wmv", 42000}, // sunset over open sea
        {"movie/prologue/SKR.wmv", 47000}, // dusk sea, last light
        {"movie/prologue/ZGF.wmv", 20000}, // snowy pines, bear pack, Zangief small
        {"movie/prologue/ZGF.wmv", 40000}, // snow slope, bears, Zangief standing
        {"dlc/03_character/movie/prologue/GKX.wmv", 6000}, // red moon over dark forest, figure
        {"dlc/03_character/movie/prologue/GKX.wmv", 14000}, // blue spirit forest floor
        {"dlc/03_character/movie/prologue/YAN.wmv", 29500}, // sunset skyline over city towers
        {"dlc/03_character/movie/prologue/YAN.wmv", 40000}, // sunset lakeside city, Yang walking
        {"dlc/03_character/movie/prologue/YUN.wmv", 10500}, // city street, two figures small
        {"dlc/04_ae2/movie/prologue/DCP.wmv", 64000}, // teal chamber, small figure, debris
        {"dlc/04_ae2/movie/prologue/ELN.wmv", 4000}, // baobab trees and tiled roof, blue sky
        {"dlc/04_ae2/movie/prologue/ELN.wmv", 60000}, // savanna trees, three figures low angle
        {"dlc/04_ae2/movie/prologue/HUG.wmv", 4000}, // night lot under overpass, thugs by train
        {"dlc/04_ae2/movie/prologue/HUG.wmv", 16000}, // night lot, row of thugs small
        {"dlc/04_ae2/movie/prologue/PSN.wmv", 52000}, // boxing ring from above, teal, two fighters
        {"dlc/04_ae2/movie/prologue/PSN.wmv", 61000}, // ring ropes, Poison small
        {"dlc/04_ae2/movie/prologue/RLN.wmv", 8000}, // battlefield, red flags, soldiers
        {"dlc/04_ae2/movie/prologue/RLN.wmv", 26000}, // desert sunset, smoke plume
        {"movie/opening/opening_r.wmv", 79800}, // sepia pine branch, small figure
        {"movie/opening/opening_r.wmv", 84300}, // golden hillside town at sunset
        {"movie/opening/opening_r.wmv", 90000}, // sunlit tiled rooftops and wall
        {"movie/opening/opening_r.wmv", 100000}, // warm hills, fighter on tiled ridge
        {"dlc/04_ae2/movie/opening/opening.wmv", 12800}, // ink grass field, Ryu small
        {"dlc/04_ae2/movie/opening/opening.wmv", 33800}, // dusk brick street, Poison, windows
        {"dlc/04_ae2/movie/opening/opening.wmv", 57000}, // sunny grassland, Elena leaping
    };
    return frames;
}

int Find(std::string_view movie, std::uint32_t ms) {
    const auto& frames = Curated();
    for (std::size_t i = 0; i < frames.size(); ++i)
        if (frames[i].ms == ms && movie == frames[i].movie) return static_cast<int>(i);
    return -1;
}

namespace {

// The path's names, split at '/'.
std::vector<std::string_view> Names(std::string_view path) {
    std::vector<std::string_view> names;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = (std::min)(path.find('/', start), path.size());
        names.push_back(path.substr(start, slash - start));
        start = slash + 1;
    }
    return names;
}

bool NameChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

constexpr std::string_view kExtension = ".wmv";

bool EndsWith(std::string_view text, std::string_view end) {
    return text.size() >= end.size() && text.substr(text.size() - end.size()) == end;
}

} // namespace

bool PathSafe(std::string_view movie) {
    if (movie.empty() || movie.size() > kLongestPath || !EndsWith(movie, kExtension)) return false;
    const auto names = Names(movie);
    for (std::size_t i = 0; i < names.size(); ++i) {
        std::string_view name = names[i];
        // The last name's ".wmv" is the only '.' a path holds.
        if (i + 1 == names.size()) name.remove_suffix(kExtension.size());
        if (name.empty()) return false;
        for (const char c : name)
            if (!NameChar(c)) return false;
    }
    return true;
}

MovieKind KindOf(std::string_view movie) {
    if (!PathSafe(movie)) return MovieKind::Other;
    auto names = Names(movie);
    // A pack's movies sit under dlc/<pack>/ as the game's own do at the top.
    std::size_t at = 0;
    if (names.size() == 5 && names[0] == "dlc") at = 2;
    if (names.size() - at != 3 || names[at] != "movie") return MovieKind::Other;
    if (names[at + 1] == "prologue") return MovieKind::Prologue;
    if (names[at + 1] == "opening") return MovieKind::Opening;
    return MovieKind::Other;
}

bool ListValid(const std::vector<Frame>& frames, std::string& why) {
    if (frames.empty()) return why = "the list is empty", false;
    std::set<std::pair<std::string, std::uint32_t>> seen;
    for (const auto& frame : frames) {
        const std::string movie = frame.movie ? frame.movie : "";
        if (!PathSafe(movie)) return why = movie + " is not a movie's path", false;
        if (KindOf(movie) == MovieKind::Other) return why = movie + " is neither a prologue nor an opening", false;
        if (frame.ms == 0 || frame.ms > 10 * 60 * 1000) return why = movie + " at " + std::to_string(frame.ms) + " ms is not within its first ten minutes", false;
        if (!seen.insert({movie, frame.ms}).second) return why = movie + " at " + std::to_string(frame.ms) + " ms is listed twice", false;
    }
    why.clear();
    return true;
}

int EntryCount() {
    return 1 + static_cast<int>(Curated().size());
}

int FrameOf(int entry) {
    return entry > kStillEntry && entry < EntryCount() ? entry - 1 : -1;
}

void DropFrames(backdrop::Cycle& cycle) {
    for (int entry = kStillEntry + 1; entry < EntryCount(); ++entry) cycle.Drop(entry);
}

std::string FileName(const Frame& frame) {
    std::string name(frame.movie ? frame.movie : "");
    if (EndsWith(name, kExtension)) name.resize(name.size() - kExtension.size());
    for (char& c : name)
        if (c == '/') c = '_';
    return name + "_" + std::to_string(frame.ms) + ".jpg";
}

void FitWithin(int width, int height, int& fittedWidth, int& fittedHeight) {
    fittedWidth = width > 0 ? width : 0;
    fittedHeight = height > 0 ? height : 0;
    if (fittedWidth <= kMostWidth && fittedHeight <= kMostHeight) return;
    // The smaller of the two shares keeps both within the most.
    const double share = (std::min)(double(kMostWidth) / fittedWidth, double(kMostHeight) / fittedHeight);
    fittedWidth = (std::max)(1, static_cast<int>(fittedWidth * share + .5));
    fittedHeight = (std::max)(1, static_cast<int>(fittedHeight * share + .5));
    fittedWidth = (std::min)(fittedWidth, kMostWidth);
    fittedHeight = (std::min)(fittedHeight, kMostHeight);
}

} }
