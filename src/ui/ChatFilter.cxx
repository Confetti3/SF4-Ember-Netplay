#include "ChatFilter.hxx"
#include "ChatFilterLists.hxx"
#include "../common/Utf8Text.hxx"
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace sf4e { namespace ui {
namespace {
// Full-width ASCII forms as ASCII, before anything else reads a character, so
// "４５５" is a number as "455" is and "＠ss" a word as "@ss" is.
std::uint32_t Narrow(std::uint32_t c) { return c >= 0xFF01 && c <= 0xFF5E ? c - 0xFEE0 : c; }

bool Digit(std::uint32_t c) { return c >= '0' && c <= '9'; }

// A character of a word, narrowed: ASCII letters and digits, the symbols that
// stand in for letters, Latin letters with accents, Cyrillic and Hangul. Kana
// and Han characters are not: Japanese and Chinese have no spaces to find
// words by, so their entries are matched as text.
bool WordCharacter(std::uint32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || Digit(c) || c == '@' || c == '$' ||
        (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) || c == 0x1E9E || (c >= 0x400 && c <= 0x4FF) || (c >= 0xAC00 && c <= 0xD7A3);
}

// A narrowed character in lower case, with a look-alike digit or symbol as its
// letter (5h1t, @ss).
std::uint32_t Fold(std::uint32_t c) {
    switch (c) {
    case '0': return 'o'; case '1': return 'i'; case '3': return 'e'; case '4': return 'a';
    case '5': return 's'; case '7': return 't'; case '@': return 'a'; case '$': return 's';
    default: break;
    }
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    // Latin Extended-A pairs its capitals and small letters, in two phases.
    if (((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) && c % 2 == 0) return c + 1;
    if (((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) && c % 2 == 1) return c + 1;
    if (c == 0x178) return 0xFF;
    if (c == 0x1E9E) return 0xDF;  // capital sharp s, as in SCHEIẞE
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}

bool Unspaced(std::uint32_t c) {
    return (c >= 0x3040 && c <= 0x30FF) || (c >= 0x3400 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || c >= 0x20000;
}

// Text as characters: as written, narrowed, folded, and where its bytes start.
// The last entry marks the end. Text that is not well-formed UTF-8 decodes as
// nothing.
struct Character { std::uint32_t c, narrow, folded; std::size_t at; };
std::vector<Character> Decode(const std::string& text) {
    std::vector<Character> out;
    for (std::size_t at = 0; at < text.size();) {
        std::uint32_t c = 0;
        const std::size_t bytes = Utf8CharacterBytes(text, at, &c);
        if (!bytes) return {};
        const auto narrow = Narrow(c);
        out.push_back({c, narrow, Fold(narrow), at});
        at += bytes;
    }
    out.push_back({0, 0, 0, text.size()});
    return out;
}

// Each run of one letter as a single letter.
std::u32string Squeeze(const std::u32string& word) {
    std::u32string out;
    for (const auto c : word) if (out.empty() || out.back() != c) out.push_back(c);
    return out;
}

bool Stretched(const std::u32string& word) {
    for (std::size_t i = 2; i < word.size(); ++i) if (word[i] == word[i - 1] && word[i] == word[i - 2]) return true;
    return false;
}

// One word of decoded text: its characters [first, last), folded, squeezed
// when a letter is stretched to three or more (else empty), and whether it is
// digits alone, a number such as a ping of 455.
struct Word {
    std::size_t first = 0, last = 0;
    std::u32string text, squeezed;
    bool number = true;
};
std::vector<Word> Words(const std::vector<Character>& text) {
    std::vector<Word> out;
    const std::size_t n = text.empty() ? 0 : text.size() - 1;
    for (std::size_t i = 0; i < n;) {
        if (!WordCharacter(text[i].narrow)) { ++i; continue; }
        Word word;
        word.first = i;
        for (; i < n && WordCharacter(text[i].narrow); ++i) {
            word.text.push_back(text[i].folded);
            word.number = word.number && Digit(text[i].narrow);
        }
        word.last = i;
        if (Stretched(word.text)) word.squeezed = Squeeze(word.text);
        out.push_back(std::move(word));
    }
    return out;
}

std::u32string FoldedText(const std::vector<Character>& text) {
    std::u32string out;
    for (std::size_t i = 0; i + 1 < text.size(); ++i) out.push_back(text[i].folded);
    return out;
}

// The lists, compiled once. An entry is matched by what it is written in: in
// kana or Han characters, or with no word in it (an emoji), it is folded text
// found anywhere; otherwise it is a phrase of folded words, found by its first
// word.
struct Phrase { std::vector<std::u32string> words; };
struct Lists {
    std::vector<Phrase> phrases;
    // Phrases by first word, as written and squeezed (the key a stretched
    // message word looks itself up by).
    std::map<std::u32string, std::vector<std::size_t>> byFirst, bySqueezedFirst;
    std::vector<std::u32string> anywhere;
    // Ember's stems (stems.txt), found as written inside a word, and squeezed
    // inside a stretched word's squeezed form (fuuuucked).
    std::vector<std::u32string> stems, squeezedStems;
};

std::vector<std::string> Lines(const unsigned char* bytes) {
    std::vector<std::string> out;
    std::string line;
    for (const char* p = reinterpret_cast<const char*>(bytes);; ++p) {
        if (*p == '\n' || *p == 0) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty() && line[0] != '#') out.push_back(line);
            line.clear();
            if (*p == 0) break;
        } else line.push_back(*p);
    }
    return out;
}

void AddPhrase(Lists& lists, const std::vector<Word>& words) {
    Phrase phrase;
    for (const auto& word : words) phrase.words.push_back(word.text);
    lists.byFirst[phrase.words.front()].push_back(lists.phrases.size());
    lists.bySqueezedFirst[Squeeze(phrase.words.front())].push_back(lists.phrases.size());
    lists.phrases.push_back(std::move(phrase));
}

const Lists& Loaded() {
    static const Lists lists = [] {
        Lists out;
        std::set<std::u32string> allowed;
        for (const auto& line : Lines(chat_filter::allow)) allowed.insert(FoldedText(Decode(line)));
        for (const auto* list : chat_filter::lists) {
            for (const auto& line : Lines(list)) {
                const auto text = Decode(line);
                const auto folded = FoldedText(text);
                if (folded.empty() || allowed.count(folded)) continue;
                bool unspaced = false;
                for (const auto& character : text) unspaced = unspaced || Unspaced(character.c);
                const auto words = Words(text);
                bool longWord = false;
                // A Hangul syllable is a word by itself (씹); a Latin letter is not.
                for (const auto& word : words)
                    longWord = longWord || word.last - word.first > 1 || (text[word.first].c >= 0xAC00 && text[word.first].c <= 0xD7A3);
                if (unspaced) {
                    // One character alone (糞, 性) is inside too many ordinary words.
                    if (folded.size() > 1) out.anywhere.push_back(folded);
                } else if (words.empty()) {
                    out.anywhere.push_back(folded);
                } else if (longWord) {
                    // Words of single letters ("s&m") would match ordinary text.
                    AddPhrase(out, words);
                }
            }
        }
        for (const auto& line : Lines(chat_filter::stems)) {
            out.stems.push_back(FoldedText(Decode(line)));
            out.squeezedStems.push_back(Squeeze(out.stems.back()));
        }
        return out;
    }();
    return lists;
}

// A stretched message word against an entry word, letter run by letter run: a
// run of three or more matches a run of the same letter no longer than it, any
// other run only one of its own length, so "fuuuck" is "fuck" while "connnn" is not "coon" and "as"
// is never "ass".
bool SameRuns(const std::u32string& word, const std::u32string& entry) {
    std::size_t i = 0, j = 0;
    while (i < word.size() && j < entry.size()) {
        if (word[i] != entry[j]) return false;
        std::size_t a = i, b = j;
        while (a < word.size() && word[a] == word[i]) ++a;
        while (b < entry.size() && entry[b] == entry[j]) ++b;
        if (b - j > a - i || (a - i < 3 && a - i != b - j)) return false;
        i = a; j = b;
    }
    return i == word.size() && j == entry.size();
}

// A message word against an entry's word: the same as written, or run by run
// when the word is stretched (an entry written with three of a letter, xxx,
// matches as written), or, for the entry's last word, the entry with -s, or -es after
// the letters English adds it to ("spic" is not "spices").
bool SameWord(const Word& word, const std::u32string& entry, bool last) {
    const auto same = [&](std::size_t cut) {
        const auto stem = word.text.substr(0, word.text.size() - cut);
        return stem == entry || (!word.squeezed.empty() && SameRuns(stem, entry));
    };
    if (same(0)) return true;
    if (!last || word.text.size() <= entry.size()) return false;
    const char32_t end = entry.back();
    if (word.text.back() == 's' && same(1)) return true;
    const auto size = word.text.size();
    return size > 2 && word.text.compare(size - 2, 2, U"es") == 0 && (end == 's' || end == 'h' || end == 'x') && same(2);
}

// The message's words from `at` against a phrase. A number alone is never a
// one-word entry; a phrase may start with one.
bool PhraseAt(const std::vector<Word>& words, std::size_t at, const Phrase& phrase) {
    const std::size_t n = phrase.words.size();
    if (at + n > words.size() || (n == 1 && words[at].number)) return false;
    for (std::size_t k = 0; k < n; ++k)
        if (!SameWord(words[at + k], phrase.words[k], k + 1 == n)) return false;
    return true;
}

// The phrases a word may start: those whose first word is it, or it less an
// ending a last word may take.
void Candidates(const std::map<std::u32string, std::vector<std::size_t>>& index, const std::u32string& word,
                std::set<std::size_t>& out) {
    for (std::size_t cut = 0; cut <= 2 && cut < word.size(); ++cut) {
        const auto found = index.find(word.substr(0, word.size() - cut));
        if (found != index.end()) out.insert(found->second.begin(), found->second.end());
    }
}

bool HasStem(const Lists& lists, const Word& word) {
    if (word.number) return false;
    for (std::size_t k = 0; k < lists.stems.size(); ++k) {
        if (word.text.find(lists.stems[k]) != std::u32string::npos) return true;
        if (!word.squeezed.empty() && word.squeezed.find(lists.squeezedStems[k]) != std::u32string::npos) return true;
    }
    return false;
}
}

std::string FilterChat(const std::string& text) {
    const auto characters = Decode(text);
    if (characters.empty()) return text;
    const std::size_t n = characters.size() - 1;
    const auto& lists = Loaded();
    // Every match counts, wherever it starts and however long it is.
    std::vector<bool> hidden(n, false);
    const auto hide = [&](std::size_t first, std::size_t last) { for (std::size_t i = first; i < last; ++i) hidden[i] = true; };

    const auto words = Words(characters);
    std::set<std::size_t> candidates;
    for (std::size_t at = 0; at < words.size(); ++at) {
        const auto& word = words[at];
        if (HasStem(lists, word)) hide(word.first, word.last);
        candidates.clear();
        Candidates(lists.byFirst, word.text, candidates);
        if (!word.squeezed.empty()) Candidates(lists.bySqueezedFirst, word.squeezed, candidates);
        for (const auto index : candidates) {
            const auto& phrase = lists.phrases[index];
            if (!PhraseAt(words, at, phrase)) continue;
            // The words are hidden; what is between them stays.
            for (std::size_t k = 0; k < phrase.words.size(); ++k) hide(words[at + k].first, words[at + k].last);
        }
    }
    const auto folded = FoldedText(characters);
    for (const auto& entry : lists.anywhere)
        for (auto found = folded.find(entry); found != std::u32string::npos; found = folded.find(entry, found + 1))
            hide(found, found + entry.size());

    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (hidden[i] && characters[i].narrow != ' ') out.push_back('*');
        else out.append(text, characters[i].at, characters[i + 1].at - characters[i].at);
    }
    return out;
}
} }
