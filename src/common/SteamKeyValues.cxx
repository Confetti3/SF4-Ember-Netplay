#include "SteamKeyValues.hxx"

#include <utility>

namespace sf4e { namespace steam {

bool Tokenize(const std::string& text, std::vector<Token>& tokens) {
    std::size_t i = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') {
            ++i;
        } else if (c == '{' || c == '}') {
            tokens.push_back(Token{c == '{' ? Token::Open : Token::Close, std::string()});
            ++i;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            i = text.find('\n', i);
            if (i == std::string::npos) i = text.size();
        } else if (c == '"') {
            std::string value;
            for (++i; i < text.size() && text[i] != '"'; ++i) {
                if (text[i] == '\\') {
                    if (i + 1 >= text.size()) return false;
                    if (text[i + 1] == '\\' || text[i + 1] == '"') ++i;
                }
                value += text[i];
            }
            if (i >= text.size()) return false;
            ++i;
            tokens.push_back(Token{Token::Text, std::move(value)});
        } else {
            return false;
        }
    }
    return true;
}

} } // namespace sf4e::steam
