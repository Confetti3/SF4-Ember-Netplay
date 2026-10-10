#include "ProblemReport.hxx"
#include "ReportUnicodeTables.hxx"
#include "../common/JoinLink.hxx"
#include "../common/BoundedRead.hxx"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <limits>

namespace sf4e { namespace reports {
namespace {
std::string Hex(std::uint64_t value) {
    std::ostringstream out; out << "0x" << std::hex << value; return out.str();
}
bool HexDigits(const std::string& text, std::size_t count) {
    return text.size() == count && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}
bool ValidUtf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i++]);
        if (c < 0x80) { if (!c) return false; continue; }
        unsigned count; std::uint32_t value, minimum;
        if (c >= 0xC2 && c <= 0xDF) { count=1; value=c&31; minimum=0x80; }
        else if (c >= 0xE0 && c <= 0xEF) { count=2; value=c&15; minimum=0x800; }
        else if (c >= 0xF0 && c <= 0xF4) { count=3; value=c&7; minimum=0x10000; }
        else return false;
        if (text.size()-i < count) return false;
        while (count--) { const auto next=static_cast<unsigned char>(text[i++]); if ((next&0xC0)!=0x80) return false; value=(value<<6)|(next&63); }
        if (value<minimum || value>0x10FFFF || (value>=0xD800&&value<=0xDFFF)) return false;
    }
    return true;
}
bool SafeField(const std::string& text, std::size_t limit) {
    return !text.empty() && text.size()<=limit && ValidUtf8(text) &&
        std::none_of(text.begin(),text.end(),[](unsigned char c){return c<32||c==127;});
}
bool Ipv6Address(std::string_view address) {
    const auto compressed = address.find("::");
    if (compressed != std::string_view::npos && address.find("::", compressed + 2) != std::string_view::npos) return false;
    if (address.find(":::") != std::string_view::npos) return false;
    if (!address.empty() && ((address.front() == ':' && compressed != 0) ||
        (address.back() == ':' && (compressed == std::string_view::npos || compressed + 2 != address.size())))) return false;
    unsigned groups = 0;
    for (std::size_t start = 0; start < address.size();) {
        if (address[start] == ':') { ++start; continue; }
        const auto end = address.find(':', start);
        const auto group = address.substr(start, end == std::string_view::npos ? address.size() - start : end - start);
        if (group.find('.') != std::string_view::npos) {
            if (end != std::string_view::npos) return false;
            unsigned parts = 0;
            for (std::size_t first = 0; first < group.size();) {
                const auto dot = group.find('.', first);
                const auto part = group.substr(first, dot == std::string_view::npos ? group.size() - first : dot - first);
                unsigned value = 0;
                if (part.empty() || part.size() > 3) return false;
                for (char c : part) { if (c < '0' || c > '9') return false; value = value * 10 + c - '0'; }
                if (value > 255) return false;
                ++parts;
                if (dot == std::string_view::npos) break;
                first = dot + 1;
                if (first == group.size()) return false;
            }
            if (parts != 4) return false;
            groups += 2;
        } else {
            if (group.size() > 4 || !std::all_of(group.begin(), group.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) return false;
            ++groups;
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    if (compressed != std::string_view::npos) return groups < 8;
    return groups == 8 && address.front() != ':' && address.back() != ':';
}
// The last `limit` bytes of a log as it was opened, and whether they start
// after its first byte. Both come from the one open file, so a log rotated or
// cut short meanwhile cannot hide that the first line read is partial.
struct Tail { std::string bytes; bool cut = false; };
Tail ReadTail(const std::filesystem::path& path, std::size_t limit) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const auto end=file.tellg(); if (end<0) return {};
    const auto size=static_cast<std::uint64_t>(end);
    const auto kept=static_cast<std::size_t>((std::min)(size,static_cast<std::uint64_t>(limit)));
    Tail tail; tail.cut=size>kept;
    tail.bytes.assign(kept,'\0'); file.seekg(static_cast<std::streamoff>(size-kept));
    if (kept && !file.read(tail.bytes.data(),static_cast<std::streamsize>(kept))) return {};
    return tail;
}
std::string RedactLog(std::string_view bytes, const std::function<bool()>& cancelled, const Account& account) {
    std::string out;
    while (!bytes.empty()) {
        if (cancelled && cancelled()) throw std::runtime_error("cancelled");
        const auto end=bytes.find('\n');
        out+=RedactLine(bytes.substr(0,end),account);
        if (end==std::string_view::npos) break;
        out+='\n'; bytes.remove_prefix(end+1);
    }
    return Utf8Tail(out);
}
// A line as Unicode scalar values for identity matching, each with the bytes
// of the line it came from, so a match is cut from the original line.
struct Decoded { std::vector<char32_t> points; std::vector<std::size_t> begin, end; };
void Push(Decoded& out, char32_t point, std::size_t begin, std::size_t end) {
    out.points.push_back(point); out.begin.push_back(begin); out.end.push_back(end);
}
// One UTF-8 character at `at`, or U+FFFD for a byte that starts none.
std::size_t DecodeUtf8At(std::string_view text, std::size_t at, char32_t& point) {
    const auto c = static_cast<unsigned char>(text[at]);
    const std::size_t length = c < 0x80 ? 1 : c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
    if (length == 1) { point = c; return 1; }
    if (!length || at + length > text.size() || !ValidUtf8(text.substr(at, length))) { point = 0xFFFD; return 1; }
    point = c & (length == 2 ? 0x1F : length == 3 ? 0x0F : 0x07);
    for (std::size_t i = 1; i < length; ++i) point = (point << 6) | (static_cast<unsigned char>(text[at + i]) & 0x3F);
    return length;
}
// The character a JSON escape after a backslash stands for, or 0 for none.
char32_t JsonEscape(char escape) {
    switch (escape) {
    case '"': return U'"'; case '\\': return U'\\'; case '/': return U'/';
    case 'b': return U'\b'; case 'f': return U'\f'; case 'n': return U'\n'; case 'r': return U'\r'; case 't': return U'\t';
    default: return 0;
    }
}
// Four hex digits at `at`, as a UTF-16 unit, or nothing.
std::optional<char32_t> HexUnit(std::string_view text, std::size_t at) {
    if (at + 4 > text.size()) return std::nullopt;
    char32_t unit = 0;
    for (std::size_t i = at; i < at + 4; ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (!std::isxdigit(c)) return std::nullopt;
        unit = unit * 16 + static_cast<char32_t>(std::isdigit(c) ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return unit;
}
// The line as written, and the line with JSON string escapes read as the
// characters they stand for, a surrogate pair of hex escapes as one character.
// Both are searched, so a Windows path whose folder name starts with "n" is
// still matched where the JSON reading takes its backslash and "n" as a newline.
Decoded DecodeLine(std::string_view text, bool json) {
    Decoded out;
    for (std::size_t at = 0; at < text.size();) {
        if (json && text[at] == '\\' && at + 1 < text.size()) {
            if (const auto simple = JsonEscape(text[at + 1])) { Push(out, simple, at, at + 2); at += 2; continue; }
            if (text[at + 1] == 'u') {
                if (const auto unit = HexUnit(text, at + 2)) {
                    if (*unit >= 0xD800 && *unit <= 0xDBFF && at + 12 <= text.size() && text[at + 6] == '\\' && text[at + 7] == 'u') {
                        const auto low = HexUnit(text, at + 8);
                        if (low && *low >= 0xDC00 && *low <= 0xDFFF) {
                            Push(out, 0x10000 + ((*unit - 0xD800) << 10) + (*low - 0xDC00), at, at + 12); at += 12; continue;
                        }
                    }
                    Push(out, *unit >= 0xD800 && *unit <= 0xDFFF ? char32_t(0xFFFD) : *unit, at, at + 6); at += 6; continue;
                }
            }
        }
        char32_t point = 0; const auto length = DecodeUtf8At(text, at, point);
        Push(out, point, at, at + length); at += length;
    }
    return out;
}
// Letters, marks, numbers and connector punctuation ('_') continue a word in
// any script, supplementary planes included; other punctuation does not.
bool WordPoint(char32_t point) {
    if (point < 0x80) return std::isalnum(static_cast<int>(point)) || point == U'_';
    const auto& ranges = unicode::WordRanges;
    const auto found = std::upper_bound(std::begin(ranges), std::end(ranges), point,
        [](char32_t value, const unicode::Range& range) { return value < range.first; });
    return found != std::begin(ranges) && point <= std::prev(found)->last;
}
// `points` under Unicode full case folding, so the sharp s, its capital and
// "ss" all compare equal. Each folded value records which point it came from,
// so a match must cover whole points: "s" alone does not match the "ss" that
// the sharp s folds to.
struct Folded { std::vector<char32_t> points; std::vector<std::size_t> from; };
Folded Fold(const std::vector<char32_t>& points) {
    Folded out;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const char32_t point = points[i];
        const auto& folds = unicode::Folds;
        const auto found = std::lower_bound(std::begin(folds), std::end(folds), point,
            [](const unicode::Fold& fold, char32_t value) { return fold.point < value; });
        if (found != std::end(folds) && found->point == point) {
            for (const char32_t to : found->to) if (to) { out.points.push_back(to); out.from.push_back(i); }
        } else { out.points.push_back(point); out.from.push_back(i); }
    }
    return out;
}
// Each of the account's names that stands alone in `line`, as byte ranges of
// the line. Every name counts, however short: they are this PC's own.
std::vector<std::pair<std::size_t, std::size_t>> IdentitySpans(std::string_view line, const Account& account) {
    std::vector<std::vector<char32_t>> names;
    for (const auto& name : account.names)
        if (!name.empty()) names.push_back(Fold(DecodeLine(name, false).points).points);
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    if (names.empty()) return spans;
    for (const bool json : {false, true}) {
        const auto decoded = DecodeLine(line, json);
        const auto folded = Fold(decoded.points);
        const auto size = folded.points.size();
        for (const auto& name : names) {
            for (std::size_t at = 0; at + name.size() <= size; ++at) {
                // Starts at a whole point, ends at the end of one.
                const auto first = folded.from[at], last = folded.from[at + name.size() - 1];
                if ((at && folded.from[at - 1] == first) || (at + name.size() < size && folded.from[at + name.size()] == last)) continue;
                if (!std::equal(name.begin(), name.end(), folded.points.begin() + at)) continue;
                if ((first && WordPoint(decoded.points[first - 1])) ||
                    (last + 1 < decoded.points.size() && WordPoint(decoded.points[last + 1]))) continue;
                spans.emplace_back(decoded.begin[first], decoded.end[last]);
            }
        }
    }
    return spans;
}
std::string ShortCode(const std::string& text) {
    if (text.empty()||text.size()>40) return {};
    for (const char c:text) if (!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_')) return {};
    return text;
}
}
std::size_t CharacterCount(std::string_view text) {
    std::size_t count=0; for (unsigned char c:text) if ((c&0xC0)!=0x80) ++count; return count;
}
std::string Utf8Tail(std::string_view bytes, std::size_t limit) {
    std::size_t begin=bytes.size()>limit?bytes.size()-limit:0;
    while (begin<bytes.size()&&(static_cast<unsigned char>(bytes[begin])&0xC0)==0x80) ++begin;
    std::string out;
    for (std::size_t i=begin;i<bytes.size();) {
        const auto c=static_cast<unsigned char>(bytes[i]);
        const std::size_t length=c<0x80?1:c>=0xC2&&c<=0xDF?2:c>=0xE0&&c<=0xEF?3:c>=0xF0&&c<=0xF4?4:0;
        if (length&&i+length<=bytes.size()&&ValidUtf8(bytes.substr(i,length))) { out.append(bytes.substr(i,length)); i+=length; }
        else { out+='?'; ++i; }
    }
    return out;
}
std::string RedactLine(std::string_view line, const Account& account) {
    struct Span { std::size_t begin,end; const char* replacement; };
    const std::string text(line);
    std::vector<Span> spans;
    const auto recognize=[&](const std::regex& rule,const char* replacement,std::size_t group=0) {
        for(auto it=std::sregex_iterator(text.begin(),text.end(),rule);it!=std::sregex_iterator();++it) {
            const auto begin=static_cast<std::size_t>(it->position(group));
            const auto end=begin+static_cast<std::size_t>(it->length(group));
            if(begin<end) spans.push_back({begin,end,replacement});
        }
    };
    // Prefixes identify invitation payloads; the explicit URI alphabet leaves
    // wrapper punctuation outside the span, including a sentence's final dot.
    static const std::regex invite(R"(\b((https?://)?embernetplay\.link/(j|s|v1)[/#]|sf4e[0-9]+:|emd[0-9]+:|ember://))",std::regex::icase);
    for(auto it=std::sregex_iterator(text.begin(),text.end(),invite);it!=std::sregex_iterator();++it) {
        const auto begin=static_cast<std::size_t>(it->position());
        const auto payload=begin+static_cast<std::size_t>(it->length());
        auto end=payload;
        while(end<text.size()&&(std::isalnum(static_cast<unsigned char>(text[end]))||
            std::string_view("_./+~%?#=&:-").find(text[end])!=std::string_view::npos)) ++end;
        while(end>payload&&text[end-1]=='.') --end;
        spans.push_back({begin,end,"[invite]"});
    }
    // Only values are sensitive: labels, whitespace and quotes belong to the
    // original line. Values stop at whitespace or surrounding punctuation.
    static const std::regex bearer(R"(\bBearer\s+([^\s<>"',;()\[\]{}]+))",std::regex::icase);
    static const std::regex token(R"(\bem[sk][A-Za-z0-9_.+/=-]*)",std::regex::icase);
    static const std::regex fieldSecret(R"(\b(write[_ -]?token|locator|invite[_ -]?code|session[_ -]?token|discord[_ -]?token|authorization|access[_ -]?token|refresh[_ -]?token)["']?\s*[:=]\s*["']?([^\s<>"',;()\[\]{}]+))",std::regex::icase);
    static const std::regex discord(R"(\bmfa\.[A-Za-z0-9_-]+|\b[A-Za-z0-9_-]{20,}\.[A-Za-z0-9_-]{6,}\.[A-Za-z0-9_-]{20,})");
    recognize(bearer,"[token]",1); recognize(token,"[token]");
    recognize(fieldSecret,"[secret]",2); recognize(discord,"[token]");
    // One path rule owns the username. A descendant separator ends it; a
    // quoted terminal username ends at its quote. Unquoted spaces stay in the
    // username unless they introduce another field, never a later slash.
    static const std::regex profile(R"([A-Z]:[\\/]+Users[\\/]+)",std::regex::icase);
    static const std::regex field(R"([A-Za-z_][A-Za-z0-9_.-]*[ \t]*[:=])");
    for(auto it=std::sregex_iterator(text.begin(),text.end(),profile);it!=std::sregex_iterator();++it) {
        const auto root=static_cast<std::size_t>(it->position());
        if(root&& (std::isalnum(static_cast<unsigned char>(text[root-1]))||text[root-1]=='_')) continue;
        const auto begin=root+static_cast<std::size_t>(it->length());
        const char quote=root&&(text[root-1]=='"'||text[root-1]=='\'')?text[root-1]:0;
        auto end=begin;
        while(end<text.size()) {
            const char c=text[end];
            if(c=='\\'||c=='/'||static_cast<unsigned char>(c)<32||c==127) break;
            if(quote?c==quote:std::string_view("\"'<>;,()[]{}|:=").find(c)!=std::string_view::npos) break;
            if(!quote&&c==' ') {
                auto next=end; while(next<text.size()&&text[next]==' ') ++next;
                std::smatch match;
                if(std::regex_search(text.cbegin()+next,text.cend(),match,field,std::regex_constants::match_continuous)) break;
            }
            ++end;
        }
        while(end>begin&&text[end-1]==' ') --end;
        if(begin<end) spans.push_back({begin,end,"<user>"});
    }
    // The account's names wherever one stands alone, so a profile outside a
    // Users folder, its 8.3 form, the PC name and the player name go too:
    // in any case, between any punctuation, and inside JSON strings.
    for(const auto& span:IdentitySpans(text,account)) spans.push_back({span.first,span.second,"<user>"});
    static const std::regex email(R"([A-Z0-9.!#$%&'*+/=?^_`{|}~-]+@[A-Z0-9-]+(\.[A-Z0-9-]+)+)",std::regex::icase);
    static const std::regex opaque(R"(\b[A-Za-z0-9_+/=-]{20,}\b)");
    recognize(email,"[email]"); recognize(opaque,"[secret]");
    // Use the invitation parser itself, including aliases, case and arbitrary
    // dash placement. Never consume a substring of a longer word or identifier.
    static const std::regex codeToken(R"([A-Za-z0-9_-]+)");
    for(auto it=std::sregex_iterator(text.begin(),text.end(),codeToken);it!=std::sregex_iterator();++it) {
        if(!join_link::ParseCode(it->str()).empty()) {
            const auto begin=static_cast<std::size_t>(it->position());
            spans.push_back({begin,begin+static_cast<std::size_t>(it->length()),"[invite]"});
        }
    }
    // Start at hex groups rather than swallowing a label such as "ip:".
    // Compressed addresses and full eight-group addresses are distinct from
    // timestamps and C++ namespaces. Zones stay with their address.
    static const std::regex candidate(R"([0-9A-Fa-f]{1,4}:[0-9A-Fa-f:.]*:[0-9A-Fa-f:.]*|::[0-9A-Fa-f:.]*)");
    std::smatch match; std::size_t search=0;
    while(std::regex_search(text.cbegin()+search,text.cend(),match,candidate)) {
        const auto start=search+static_cast<std::size_t>(match.position());
        auto token=match.str(); search=start+token.size();
        // Consume the zone explicitly, stopping before surrounding punctuation.
        if(search<text.size()&&text[search]=='%') {
            auto zoneEnd=text.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-",search+1);
            if(zoneEnd==std::string::npos) zoneEnd=text.size();
            if(zoneEnd>search+1) { token.append(text,search,zoneEnd-search); search=zoneEnd; }
        }
        // A sentence's final period is punctuation, even after a mapped IPv4
        // suffix. Leave it in the output rather than part of the replacement.
        const auto length=token.find_last_not_of('.');
        if (length!=std::string::npos) token.resize(length+1);
        const auto zone=token.find('%'); const auto address=token.substr(0,zone);
        const auto end=start+token.size();
        const bool left=start==0||(!std::isalnum(static_cast<unsigned char>(text[start-1]))&&text[start-1]!='_');
        const bool right=end==text.size()||(!std::isalnum(static_cast<unsigned char>(text[end]))&&text[end]!='_');
        if(left&&right&&Ipv6Address(address)) spans.push_back({start,end,"[ip]"});
    }
    static const std::regex ipv4(R"(\b([0-9]{1,3}\.){3}[0-9]{1,3}\b)");
    recognize(ipv4,"[ip]");
    // Earliest start wins; at that start the longest span wins, with recognizer
    // order breaking exact ties. Union overlaps so no recognized byte leaks.
    // Adjacent spans remain separate. Only this emitter copies original text.
    std::stable_sort(spans.begin(),spans.end(),[](const Span& a,const Span& b) {
        return a.begin!=b.begin?a.begin<b.begin:a.end>b.end;
    });
    std::string out; std::size_t copied=0;
    for(std::size_t i=0;i<spans.size();) {
        const auto span=spans[i++]; auto end=span.end;
        while(i<spans.size()&&spans[i].begin<end) { end=(std::max)(end,spans[i].end); ++i; }
        out.append(text,copied,span.begin-copied); out+=span.replacement; copied=end;
    }
    out.append(text,copied,std::string::npos);
    return out;
}
nlohmann::json MetaJson(const Metadata& meta, const std::string& comment) {
    nlohmann::json value={{"schema",1},{"kind",meta.kind},{"app_version",meta.appVersion},{"channel",meta.channel},
        {"build_id",meta.buildId},{"source_revision",meta.sourceRevision},{"windows_version",meta.windowsVersion}};
    if (meta.exitCode) value["exit_code"]=*meta.exitCode;
    if (meta.exceptionCode) value["exception_code"]=Hex(*meta.exceptionCode);
    if (meta.crashAddress) value["crash_address"]=Hex(*meta.crashAddress);
    if (!comment.empty()) value["comment"]=comment;
    return value;
}
bool BuildMultipart(const Report& report, const Submission& submission, const std::string& boundary, Multipart& result, std::string& error) {
    result={}; error.clear();
    const auto reject=[&]{error="Invalid or oversized report.";return false;};
    const auto& m=report.meta;
    if ((m.kind!="crash"&&m.kind!="problem") || (m.channel!="stable"&&m.channel!="beta"&&m.channel!="nightly") ||
        !SafeField(m.appVersion,128)||!SafeField(m.sourceRevision,128)||!SafeField(m.windowsVersion,256)||!HexDigits(m.buildId,64)||
        !ValidUtf8(submission.comment)||CharacterCount(submission.comment)>2000 ||
        (m.kind=="problem"&&submission.comment.find_first_not_of(" \t\r\n")==std::string::npos)||report.logs.size()>4 ||
        boundary.empty()||boundary.size()>70||!std::all_of(boundary.begin(),boundary.end(),[](unsigned char c){return std::isalnum(c)||c=='-';})) return reject();
    const std::string meta=MetaJson(m,submission.comment).dump();
    if (meta.size()>16*1024) return reject();
    const auto collision=[&](const std::string& bytes){return bytes.find(boundary)!=std::string::npos;};
    if (collision(meta)) return reject();
    std::set<std::string> names;
    for (const auto& log:report.logs) {
        if ((log.name!="sf4e.log"&&log.name!="launcher.log"&&log.name!="sf4e-crash.log"&&log.name!="sf4-net.log")||
            !names.insert(log.name).second||log.text.size()>LogLimit||!ValidUtf8(log.text)||collision(log.text)) return reject();
    }
    if (submission.includeDump&&!report.minidump.empty() &&
        (report.minidump.size()>DumpLimit||report.minidump.compare(0,4,"MDMP")!=0||collision(report.minidump))) return reject();
    std::string body;
    const auto part=[&](const char* name,const std::string& filename,const char* type,const std::string& bytes) {
        body+="--"+boundary+"\r\nContent-Disposition: form-data; name=\""+name+"\"";
        if (!filename.empty()) body+="; filename=\""+filename+"\"";
        body+="\r\nContent-Type: "+std::string(type)+"\r\n\r\n"+bytes+"\r\n";
    };
    part("meta",{},"application/json",meta);
    for (const auto& log:report.logs) part("log",log.name,"text/plain; charset=utf-8",log.text);
    if (submission.includeDump&&!report.minidump.empty()) part("minidump","crash.dmp","application/octet-stream",report.minidump);
    body+="--"+boundary+"--\r\n";
    if (body.size()>BodyLimit) return reject();
    result={"multipart/form-data; boundary="+boundary,std::move(body)}; return true;
}
const char* ReportChannel(updates::UpdateChannel channel) {
    return channel==updates::UpdateChannel::Nightly?"nightly":channel==updates::UpdateChannel::Beta?"beta":"stable";
}
Report Collect(const std::filesystem::path& directory, Metadata meta, const CrashContext& crash, const std::function<bool()>& cancelled,
    const Account& account, const std::function<void(const std::filesystem::path&)>& afterRead) {
    Report report; report.meta=std::move(meta);
    if (report.meta.kind=="crash") {
        report.meta.exitCode=crash.exitCode; report.meta.exceptionCode=crash.exceptionCode; report.meta.crashAddress=crash.address;
        std::error_code ec;
        if (std::filesystem::last_write_time(directory/L"sf4e-crash.log",ec)>=crash.started&&!ec) {
            std::ifstream file(directory/L"sf4e-crash.log",std::ios::binary); std::array<char,2048> bytes{};
            file.read(bytes.data(),static_cast<std::streamsize>(bytes.size()));
            const std::string header(bytes.data(),static_cast<std::size_t>(file.gcount()));
            const auto fact=[&](const char* key)->std::optional<std::uint64_t> {
                const auto start=header.find(key); if(start==std::string::npos)return {};
                try { return std::stoull(header.substr(start+std::char_traits<char>::length(key)),nullptr,16); } catch (...) { return {}; }
            };
            if (!report.meta.exceptionCode) { const auto code=fact(" code=0x"); if(code&&*code<=UINT32_MAX)report.meta.exceptionCode=static_cast<std::uint32_t>(*code); }
            if (!report.meta.crashAddress) report.meta.crashAddress=fact(" address=0x");
        }
    }
    for (const char* name:{"sf4e.log","launcher.log","sf4e-crash.log","sf4-net.log"}) {
        if (cancelled&&cancelled()) throw std::runtime_error("cancelled");
        std::error_code ec; if (!std::filesystem::is_regular_file(directory/name,ec)||ec) continue;
        const auto tail=ReadTail(directory/name,LogLimit);
        if (afterRead) afterRead(directory/name);
        auto text=Utf8Tail(tail.bytes);
        // A tail that starts mid-file drops its first, partial line.
        if (tail.cut) {
            const auto newline=text.find('\n'); text=newline==std::string::npos?std::string():text.substr(newline+1);
        }
        report.logs.push_back({name,RedactLog(text,cancelled,account)});
    }
    if (report.meta.kind=="crash"&&!crash.smallDump.empty()) {
        const auto read=durable::ReadBounded(crash.smallDump,DumpLimit);
        if (read.status==durable::ReadStatus::Read) report.minidump.assign(read.bytes.begin(),read.bytes.end());
        if (report.minidump.compare(0,4,"MDMP")!=0) report.minidump.clear();
    }
    return report;
}
void Append(std::vector<Record>& sent, Record record) {
    sent.push_back(std::move(record));
    if (sent.size()>SentKept) sent.erase(sent.begin(),sent.end()-SentKept);
}
namespace {
const char* StatusName(Status status) {
    return status==Status::Pending?"pending":status==Status::Received?"received":status==Status::Refused?"refused":"not_sent";
}
}
std::string SentJson(const std::vector<Record>& sent) {
    auto reports=nlohmann::json::array();
    for (std::size_t i=sent.size()>SentKept?sent.size()-SentKept:0;i<sent.size();++i) {
        const auto& r=sent[i];
        reports.push_back({{"time",r.time},{"status",StatusName(r.status)},{"kind",r.kind},{"id",r.id},{"reason",r.reason},
            {"attempt",r.attempt},{"dump",r.dump},{"automatic",r.automatic}});
    }
    return nlohmann::json{{"schema",1},{"reports",reports}}.dump(1);
}
std::optional<std::vector<Record>> ParseSent(std::string_view text) {
    if (text.size()>SentFileLimit) return std::nullopt;
    const auto json=nlohmann::json::parse(text.begin(),text.end(),nullptr,false);
    if (!json.is_object()||!json.contains("schema")||json["schema"]!=1||!json.contains("reports")||!json["reports"].is_array()||
        json["reports"].size()>SentKept) return std::nullopt;
    std::vector<Record> sent;
    const auto string=[](const nlohmann::json& entry,const char* key,std::string& out){
        if (!entry.contains(key)||!entry[key].is_string()) return false;
        out=entry[key].get<std::string>(); return true;
    };
    const auto flag=[](const nlohmann::json& entry,const char* key,bool& out){
        if (!entry.contains(key)||!entry[key].is_boolean()) return false;
        out=entry[key].get<bool>(); return true;
    };
    for (const auto& entry:json["reports"]) {
        Record r; std::string status;
        if (!entry.is_object()||!entry.contains("time")||!entry["time"].is_number_integer()||
            (entry["time"].is_number_unsigned()&&entry["time"].get<std::uint64_t>()>static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))||
            !string(entry,"status",status)||
            !string(entry,"kind",r.kind)||!string(entry,"id",r.id)||!string(entry,"reason",r.reason)||!string(entry,"attempt",r.attempt)||
            !flag(entry,"dump",r.dump)||!flag(entry,"automatic",r.automatic)) return std::nullopt;
        r.time=entry["time"].get<std::int64_t>();
        if (status=="pending") r.status=Status::Pending;
        else if (status=="received") r.status=Status::Received;
        else if (status=="refused") r.status=Status::Refused;
        else if (status=="not_sent") r.status=Status::NotSent;
        else return std::nullopt;
        if ((r.kind!="crash"&&r.kind!="problem")||(!r.id.empty()&&(!HexDigits(r.id,32)||ShortCode(r.id)!=r.id))||
            (!r.reason.empty()&&ShortCode(r.reason)!=r.reason)||(!r.attempt.empty()&&(!HexDigits(r.attempt,16)||ShortCode(r.attempt)!=r.attempt)))
            return std::nullopt;
        sent.push_back(std::move(r));
    }
    return sent;
}
bool AutomaticAllowed(const std::vector<Record>& sent, std::int64_t now) {
    const auto recent=std::count_if(sent.begin(),sent.end(),[&](const Record& r){return r.time>now-24*60*60;});
    return recent<AutomaticPerDay;
}
} }
