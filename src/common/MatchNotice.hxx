#pragma once
#include <array>
#include <cstdint>
#include <cstring>

#include "NoticeSeverity.hxx"

namespace sf4e {

// The single current netplay notice. Only the game thread touches it; other
// threads read a copy through the published presentation snapshot. The text
// is stored inline, so replacing or clearing a notice never frees memory a
// reader could still hold. One slot is enough: the newest event is the one
// the player needs, and severity decides how long it stays.
class MatchNoticeState {
public:
    static constexpr std::uint64_t InfoMs = 6000;
    static constexpr std::uint64_t WarningMs = 12000;

    // Longer text is cut at the last whole UTF-8 character that fits.
    void Push(const char* text, NoticeSeverity severity, std::uint64_t now) {
        if (!text || !text[0]) return;
        std::size_t length = std::strlen(text);
        if (length >= text_.size()) {
            length = text_.size() - 1;
            while (length && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) --length;
        }
        std::memcpy(text_.data(), text, length);
        text_[length] = 0;
        severity_ = severity;
        shownAtMs_ = now;
    }
    void Clear() { *this = MatchNoticeState(); }
    // An Error stays until a newer notice replaces it or a new session starts.
    void ClearTransient() { if (severity_ != NoticeSeverity::Error) Clear(); }
    void Expire(std::uint64_t now) {
        if (Empty() || now < shownAtMs_) return;
        const std::uint64_t age = now - shownAtMs_;
        if ((severity_ == NoticeSeverity::Info && age >= InfoMs) ||
            (severity_ == NoticeSeverity::Warning && age >= WarningMs)) Clear();
    }

    bool Empty() const { return !text_[0]; }
    const char* Text() const { return text_.data(); }
    NoticeSeverity Severity() const { return severity_; }

private:
    std::array<char, 256> text_{};
    NoticeSeverity severity_ = NoticeSeverity::Info;
    std::uint64_t shownAtMs_ = 0;
};

}
