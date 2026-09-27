#pragma once

#include <string>
#include <vector>

namespace sf4e { namespace steam {

// One lexical element of Steam's text KeyValues (libraryfolders.vdf,
// appmanifest_*.acf). Braces are their own kinds, so a quoted "{" stays text.
struct Token {
    enum Kind { Text, Open, Close } kind;
    std::string text;
};

// Splits KeyValues text into quoted strings and braces. A UTF-8 BOM,
// whitespace and // comments are skipped, and \\ and \" inside a string become
// \ and ". Anything else, such as an unquoted word, a #include or #base line,
// or a string cut off before its closing quote, fails.
bool Tokenize(const std::string& text, std::vector<Token>& tokens);

} } // namespace sf4e::steam
