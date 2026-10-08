#pragma once
#include "ComboBook.hxx"
#include <string>
#include <utility>
#include <vector>

// Row edits on a combo's moves, each move one canonical step text: what the
// move editor and the timing screen do to the list. Every edit keeps the list
// a combo: at least one move, at most MaxSteps, and a first move that links.
namespace sf4e { namespace combo {
// A copy's name fits the book's text limit without splitting a UTF-8 character.
inline std::string CopyName(const std::string& name, const std::string& suffix) {
    std::string out = Clean(name + " " + suffix);
    if (out.size() > MaxText) {
        std::size_t length = MaxText;
        while (length && (static_cast<unsigned char>(out[length]) & 0xC0) == 0x80) --length;
        out.resize(length);
    }
    return Clean(out);
}
// A first move has nothing to cancel or follow, so it links.
inline void LinkFirst(std::vector<std::string>& steps) {
    if (steps.empty()) return;
    if (steps[0].compare(0, 3, "xx ") == 0) steps[0] = steps[0].substr(3);
    else if (steps[0].compare(0, 2, "~ ") == 0) steps[0] = steps[0].substr(2);
}
// A copy leaves its "#frame" behind: two moves cannot press on one frame.
inline std::string Unframed(const std::string& text) {
    Step step; std::string error;
    if (!ParseStep(text, step, error) || step.at < 0) return text;
    step.at = -1;
    return Canonical(step);
}
// Puts moves in before row at; at == size adds them at the end.
inline bool InsertSteps(std::vector<std::string>& steps, std::size_t at, const std::vector<std::string>& added) {
    if (added.empty() || at > steps.size() || steps.size() + added.size() > MaxSteps) return false;
    steps.insert(steps.begin() + at, added.begin(), added.end());
    LinkFirst(steps);
    return true;
}
// Takes count rows out from row from. Never all of them.
inline bool RemoveSteps(std::vector<std::string>& steps, std::size_t from, std::size_t count) {
    if (!count || from >= steps.size() || count > steps.size() - from || count == steps.size()) return false;
    steps.erase(steps.begin() + from, steps.begin() + from + count);
    LinkFirst(steps);
    return true;
}
// The count rows from row from again, times over, right after themselves.
inline bool RepeatSteps(std::vector<std::string>& steps, std::size_t from, std::size_t count, std::size_t times) {
    if (!count || !times || from >= steps.size() || count > steps.size() - from || count * times > MaxSteps - steps.size()) return false;
    std::vector<std::string> copies;
    for (std::size_t time = 0; time < times; ++time)
        for (std::size_t i = 0; i < count; ++i) copies.push_back(Unframed(steps[from + i]));
    steps.insert(steps.begin() + from + count, copies.begin(), copies.end());
    return true;
}
// Swaps a row with the one before (delta < 0) or after it. A "#frame" stays
// with its place in the order, so the pattern's frames keep rising.
inline bool MoveStep(std::vector<std::string>& steps, std::size_t index, int delta) {
    if (!delta || index >= steps.size() || (delta < 0 ? index == 0 : index + 1 >= steps.size())) return false;
    const std::size_t other = delta < 0 ? index - 1 : index + 1;
    Step a, b; std::string error;
    if (ParseStep(steps[index], a, error) && ParseStep(steps[other], b, error) && (a.at >= 0 || b.at >= 0)) {
        std::swap(a.at, b.at);
        steps[index] = Canonical(a); steps[other] = Canonical(b);
    }
    std::swap(steps[index], steps[other]);
    LinkFirst(steps);
    return true;
}
} }
