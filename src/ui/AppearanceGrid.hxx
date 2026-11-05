#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>
namespace sf4e { namespace ui {
// How the costume, colour and Ultra grids (AppearanceGallery) lay their cards out:
// every card at once, as large as the area lets them be. A card is its
// picture, `aspect` times as tall as it is wide, over a caption `caption`
// tall, and no wider than `widest`. The columns are the count that makes the
// cards largest; of counts that tie (the cards held at `widest`, or by the
// height), the one with the fewest lines and then the fewest columns, so a
// short set reads as one line and a tall one stays square. Cards that would
// be narrower than `narrowest` keep that width and the grid scrolls instead.
struct GalleryGrid {
    int columns = 1;
    float width = 0, height = 0;
    // The lines `cards` cards take.
    std::size_t Lines(std::size_t cards) const { return (cards + Columns() - 1) / Columns(); }
    // Where card `index` starts, from the area's top-left: line by line, the
    // whole grid centred across an area `areaWidth` wide.
    struct Offset { float x, y; };
    Offset Cell(std::size_t index, float areaWidth, float gap) const {
        const float across = static_cast<float>(columns) * width + static_cast<float>(columns - 1) * gap;
        const float left = (std::max)(0.f, (areaWidth - across) / 2);
        return {left + static_cast<float>(index % Columns()) * (width + gap), static_cast<float>(index / Columns()) * (height + gap)};
    }
private:
    std::size_t Columns() const { return static_cast<std::size_t>((std::max)(1, columns)); }
};
inline GalleryGrid FitGallery(std::size_t cards, float width, float height, float gap, float aspect, float caption,
                              float narrowest, float widest) {
    GalleryGrid best;
    if (!cards || width <= 0 || height <= 0 || aspect <= 0) return best;
    std::size_t bestLines = 0;
    for (std::size_t columns = 1; columns <= cards; ++columns) {
        const std::size_t lines = (cards + columns - 1) / columns;
        const float room = (height - gap * static_cast<float>(lines - 1)) / static_cast<float>(lines);
        const float across = (width - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
        const float w = (std::min)({widest, across, (room - caption) / aspect});
        if (w <= 0) continue;
        if (w > best.width || (w == best.width && lines < bestLines)) {
            best.columns = static_cast<int>(columns); best.width = w; best.height = w * aspect + caption; bestLines = lines;
        }
    }
    if (best.width < narrowest) {
        best.width = (std::min)(narrowest, width);
        const auto fit = static_cast<std::size_t>((width + gap) / (best.width + gap));
        best.columns = static_cast<int>((std::max)(std::size_t(1), (std::min)(cards, fit)));
        best.height = best.width * aspect + caption;
    }
    return best;
}
// A grid's cards, in unscaled pixels: the picture's height for its width
// (`aspect`), and the narrowest and widest a card goes (FitGallery). A
// costume or colour shows the whole figure, taller than wide; an Ultra its
// 16:9 picture, wide enough for its input's symbols and words under it, and as
// wide as three across a 1080p screen let it be.
struct CardShape { float aspect, narrowest, widest; };
constexpr CardShape FigureCard{1.3f, 64, 220};
constexpr CardShape UltraCard{9.f / 16, 160, 560};
// The colour a costume's card shows it in, and the colour its colour grid
// opens on: the colour worn now where the costume has it, else its first;
// 0 where it has none.
inline int GalleryColor(const std::vector<int>& colors, int worn) {
    if (std::find(colors.begin(), colors.end(), worn) != colors.end()) return worn;
    return colors.empty() ? 0 : colors.front();
}
} }
