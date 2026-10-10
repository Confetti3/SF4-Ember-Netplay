#pragma once
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

namespace sf4e { namespace updates {
namespace notes_detail {
inline bool Space(char c) { return c==' '||c=='\t'; }
inline bool Alnum(char c) { return (c>='0'&&c<='9')||(c>='a'&&c<='z')||(c>='A'&&c<='Z')||static_cast<unsigned char>(c)>=0x80; }
inline bool Punctuation(char c) { return c>32&&c<127&&!Alnum(c); }
inline bool StartsWith(const std::string& s,std::size_t at,const char* prefix) { return s.compare(at,std::char_traits<char>::length(prefix),prefix)==0; }
inline std::string Trim(const std::string& s) {
    std::size_t a=0,b=s.size();
    while(a<b&&Space(s[a]))++a;
    while(b>a&&Space(s[b-1]))--b;
    return s.substr(a,b-a);
}
// Whether a character is shown: one the menu fonts can draw in the Basic
// Multilingual Plane that is neither a control character nor a formatting
// control. The formatting controls draw nothing and some change how the
// text around them reads: the soft hyphen, the Arabic letter mark, the
// Mongolian vowel separator, the zero-width spaces, joiners and directional
// marks, the bidirectional embeddings, overrides and isolates, the word
// joiner and invisible operators, the deprecated format characters, the
// variation selectors, the byte order mark and the interlinear annotation
// marks. U+FFFD, which stands for a character lost, goes too.
inline bool Displayed(unsigned codepoint) {
    if(codepoint<0xA0||codepoint>0xFFFF||(codepoint>=0xD800&&codepoint<=0xDFFF))return false;
    static const struct { unsigned from, to; } hidden[]={{0x00AD,0x00AD},{0x061C,0x061C},{0x180E,0x180E},{0x200B,0x200F},
        {0x202A,0x202E},{0x2060,0x2064},{0x2066,0x206F},{0xFE00,0xFE0F},{0xFEFF,0xFEFF},{0xFFF9,0xFFFB},{0xFFFD,0xFFFD}};
    for(const auto& range:hidden)if(codepoint>=range.from&&codepoint<=range.to)return false;
    return true;
}
// Valid UTF-8 of what Displayed allows. A cut through a character (the
// client keeps the first 2,000 bytes), an overlong form or a surrogate is not
// valid and goes; so do emoji past the Basic Multilingual Plane, which the
// fonts do not draw. A newline stays and a tab reads as a space.
inline std::string DrawableUtf8(const std::string& text) {
    std::string out;out.reserve(text.size());
    for(std::size_t i=0;i<text.size();) {
        const auto lead=static_cast<unsigned char>(text[i]);
        if(lead<0x80) {
            if(lead=='\n')out+='\n';
            else if(lead=='\t')out+=' ';
            else if(lead>=0x20&&lead!=0x7F)out+=static_cast<char>(lead);
            ++i;continue;
        }
        const int length=lead>=0xF0&&lead<=0xF4?4:lead>=0xE0&&lead<=0xEF?3:lead>=0xC2&&lead<=0xDF?2:0;
        if(!length||i+length>text.size()){++i;continue;}
        unsigned codepoint=lead&(0xFFu>>(length+1));
        bool valid=true;
        for(int n=1;n<length;++n) {
            const auto next=static_cast<unsigned char>(text[i+n]);
            if((next&0xC0)!=0x80){valid=false;break;}
            codepoint=(codepoint<<6)|(next&0x3Fu);
        }
        if(!valid){++i;continue;}
        // Valid: not overlong, not a surrogate, not past U+10FFFF.
        const unsigned least[]={0,0,0x80,0x800,0x10000};
        valid=codepoint>=least[length]&&codepoint<=0x10FFFF&&!(codepoint>=0xD800&&codepoint<=0xDFFF);
        if(valid&&Displayed(codepoint))out.append(text,i,length);
        i+=length;
    }
    return out;
}
inline std::string Inline(const std::string& line);
// A run of `marker` that opens emphasis at `at`, `count` long: the closing
// run's position, or npos when the run is only text (2 * 3, snake_case).
inline std::size_t EmphasisClose(const std::string& s,std::size_t at,std::size_t count,char marker) {
    const std::size_t after=at+count;
    if(after>=s.size()||Space(s[after]))return std::string::npos;
    if(marker=='_'&&at>0&&Alnum(s[at-1]))return std::string::npos;
    for(std::size_t j=after+1;j+count<=s.size();++j) {
        if(s[j]!=marker)continue;
        std::size_t run=0;while(j+run<s.size()&&s[j+run]==marker)++run;
        if(run==count&&!Space(s[j-1])&&!(marker=='_'&&j+run<s.size()&&Alnum(s[j+run])))return j;
        j+=run-1;
    }
    return std::string::npos;
}
// Inline Markdown as the words it shows: emphasis marks go, a link or an
// image reads as its text, HTML tags go and a few entities are spelled out.
// Code spans and every other literal span are Tokenize's: they reach this as
// marks, which it passes through as they are.
inline std::string Inline(const std::string& s) {
    std::string out;
    for(std::size_t i=0;i<s.size();) {
        const char c=s[i];
        if(c=='\\'&&i+1<s.size()&&Punctuation(s[i+1])){out+=s[i+1];i+=2;continue;}
        if(c=='['||(c=='!'&&i+1<s.size()&&s[i+1]=='[')) {
            const std::size_t open=c=='!'?i+1:i;
            const auto close=s.find(']',open+1);
            if(close!=std::string::npos) {
                const std::string text=s.substr(open+1,close-open-1);
                std::size_t end=close+1;
                if(end<s.size()&&s[end]=='('){const auto target=s.find(')',end);if(target!=std::string::npos)end=target+1;else end=std::string::npos;}
                else if(end<s.size()&&s[end]=='['){const auto label=s.find(']',end);if(label!=std::string::npos)end=label+1;else end=std::string::npos;}
                else if(c=='!')end=std::string::npos;
                if(end!=std::string::npos){out+=Inline(text);i=end;continue;}
            }
            out+=c;++i;continue;
        }
        if(c=='<') {
            const auto close=s.find('>',i+1);
            if(close!=std::string::npos) {
                const std::string inner=s.substr(i+1,close-i-1);
                if(StartsWith(inner,0,"http://")||StartsWith(inner,0,"https://")){out+=inner;i=close+1;continue;}
                const bool tag=!inner.empty()&&(inner[0]=='/'||inner[0]=='!'||(inner[0]>='a'&&inner[0]<='z')||(inner[0]>='A'&&inner[0]<='Z'));
                if(tag){
                    // A line break or a paragraph tag still separates words.
                    if((StartsWith(inner,0,"br")||StartsWith(inner,0,"/p"))&&!out.empty()&&out.back()!=' ')out+=' ';
                    i=close+1;continue;
                }
            }
            out+=c;++i;continue;
        }
        if(c=='&') {
            static const struct { const char* name; const char* text; } entities[]={{"&amp;","&"},{"&lt;","<"},{"&gt;",">"},{"&quot;","\""},{"&#39;","'"},{"&apos;","'"},{"&nbsp;"," "}};
            bool matched=false;
            for(const auto& entity:entities)if(StartsWith(s,i,entity.name)){out+=entity.text;i+=std::char_traits<char>::length(entity.name);matched=true;break;}
            if(!matched){out+=c;++i;}
            continue;
        }
        if(c=='*'||c=='_'||c=='~') {
            std::size_t run=0;while(i+run<s.size()&&s[i+run]==c)++run;
            if(run<=3&&(c!='~'||run==2)) {
                const auto close=EmphasisClose(s,i,run,c);
                if(close!=std::string::npos){out+=Inline(s.substr(i+run,close-i-run));i=close+run;continue;}
            }
            out.append(run,c);i+=run;continue;
        }
        out+=c;++i;
    }
    return out;
}
// Only these characters, with at least three of the first, and spaces: a rule or a table's divider.
inline bool RuleLine(const std::string& s,const char* marks) {
    std::size_t count=0;
    for(const char c:s){if(c==marks[0])++count;else if(!Space(c)&&!std::char_traits<char>::find(marks,std::char_traits<char>::length(marks),c))return false;}
    return count>=3;
}
// A fenced code block's fence: its marker, a backtick or a tilde, and how
// many of it opened the block.
struct Fence { char marker = 0; std::size_t length = 0; };
// The line opens a fence: after its indent, three or more of one marker, and
// for backticks no backtick after them (that is inline code, not a fence).
inline bool OpensFence(const std::string& line,Fence& fence) {
    std::size_t at=0;while(at<line.size()&&Space(line[at]))++at;
    if(at>=line.size()||(line[at]!='`'&&line[at]!='~'))return false;
    std::size_t run=0;while(at+run<line.size()&&line[at+run]==line[at])++run;
    if(run<3||(line[at]=='`'&&line.find('`',at+run)!=std::string::npos))return false;
    fence.marker=line[at];fence.length=run;
    return true;
}
// The line closes fence: after its indent, at least as many of the same
// marker, and nothing but spaces after them.
inline bool ClosesFence(const std::string& line,const Fence& fence) {
    std::size_t at=0;while(at<line.size()&&Space(line[at]))++at;
    std::size_t run=0;while(at+run<line.size()&&line[at+run]==fence.marker)++run;
    if(run<fence.length)return false;
    for(std::size_t i=at+run;i<line.size();++i)if(!Space(line[i]))return false;
    return true;
}
// A line of the notes as the one traversal reads them. fence: a fence line,
// which is not shown; code: a line inside a fenced block, shown as it is.
// Otherwise text, in which every span the Markdown pass must leave alone (an
// inline code span's contents, a backtick run that opens none, a comment
// that never closes, up to the next fence or the end) stands as a mark, \x01,
// its number and \x02, that Restore puts back once the pass is done.
struct NoteLine { std::string text; bool fence = false, code = false; };
struct NoteTokens { std::vector<NoteLine> lines; std::vector<std::string> kept; };
inline void Keep(NoteTokens& tokens,std::string& text,std::string span) {
    text+='\x01';text+=std::to_string(tokens.kept.size());text+='\x02';
    tokens.kept.push_back(std::move(span));
}
// Comments, code spans and fences in one traversal, so no later pass undoes
// what one of them decided. A fenced block runs to a fence of its own marker
// at least as long, or to the end. A code span closes with a run of backticks
// of its own length on its line. A comment closes with "-->", on its line or
// a later one, never across a fence; it goes, leaving the lines it took
// empty. A comment that never closes, and a backtick run that closes no
// span, are ambiguous and stay as the text they are: the comment from its
// opener up to the next fence, or the end, with no Markdown read in it.
inline NoteTokens Tokenize(const std::string& s) {
    NoteTokens tokens;
    std::vector<std::string> raw;
    for(std::size_t start=0;start<=s.size();) {
        auto end=s.find('\n',start);if(end==std::string::npos)end=s.size();
        raw.push_back(s.substr(start,end-start));start=end+1;
    }
    Fence fence;bool fenced=false;
    // A comment that closed on a later line: that line goes on after "-->".
    bool resume=false;std::size_t resumeAt=0;
    for(std::size_t line=0;line<raw.size();++line) {
        const std::string& text=raw[line];
        std::size_t i=0;
        if(resume){resume=false;i=resumeAt;}
        else if(fenced) {
            if(ClosesFence(text,fence)){fenced=false;tokens.lines.push_back({std::string(),true,false});}
            else tokens.lines.push_back({text,false,true});
            continue;
        }
        else {
            Fence opened;
            if(OpensFence(text,opened)){fenced=true;fence=opened;tokens.lines.push_back({std::string(),true,false});continue;}
        }
        NoteLine current;
        bool jumped=false;
        while(i<text.size()&&!jumped) {
            const char c=text[i];
            // An escaped mark is text, as Inline reads it.
            if(c=='\\'&&i+1<text.size()&&Punctuation(text[i+1])){current.text.append(text,i,2);i+=2;continue;}
            if(c=='`') {
                std::size_t run=0;while(i+run<text.size()&&text[i+run]=='`')++run;
                std::size_t close=std::string::npos;
                for(std::size_t at=text.find('`',i+run);at!=std::string::npos;) {
                    std::size_t length=0;while(at+length<text.size()&&text[at+length]=='`')++length;
                    if(length==run){close=at;break;}
                    at=text.find('`',at+length);
                }
                if(close==std::string::npos)Keep(tokens,current.text,std::string(run,'`'));
                else Keep(tokens,current.text,Trim(text.substr(i+run,close-i-run)));
                i=close==std::string::npos?i+run:close+run;continue;
            }
            if(StartsWith(text,i,"<!--")) {
                // Where it closes: here, or on a later line before any fence opens.
                std::size_t closeLine=line,closeAt=text.find("-->",i+4);
                Fence any;
                while(closeAt==std::string::npos&&++closeLine<raw.size()&&!OpensFence(raw[closeLine],any))closeAt=raw[closeLine].find("-->");
                if(closeAt==std::string::npos) {
                    // It never closes: from its opener to the fence that
                    // stopped the search, or the end, every line is text as
                    // it stands, each kept whole so no Markdown is read in it.
                    Keep(tokens,current.text,text.substr(i));
                    tokens.lines.push_back(std::move(current));
                    for(std::size_t kept=line+1;kept<closeLine;++kept) {
                        NoteLine literal;Keep(tokens,literal.text,raw[kept]);
                        tokens.lines.push_back(std::move(literal));
                    }
                    line=closeLine-1;jumped=true;continue;
                }
                if(closeLine==line){i=closeAt+3;continue;}
                // The lines it took are empty, and its last goes on after it.
                tokens.lines.push_back(std::move(current));
                for(std::size_t empty=line+1;empty<closeLine;++empty)tokens.lines.push_back(NoteLine{});
                resume=true;resumeAt=closeAt+3;line=closeLine-1;jumped=true;continue;
            }
            current.text+=c;++i;
        }
        if(!jumped)tokens.lines.push_back(std::move(current));
    }
    return tokens;
}
// The text with each mark put back as the span it kept.
inline std::string Restore(const std::string& text,const NoteTokens& tokens) {
    std::string out;
    for(std::size_t i=0;i<text.size();) {
        if(text[i]=='\x01') {
            const auto end=text.find('\x02',i);
            if(end!=std::string::npos) {
                const auto index=static_cast<std::size_t>(std::strtoul(text.substr(i+1,end-i-1).c_str(),nullptr,10));
                if(index<tokens.kept.size()){out+=tokens.kept[index];i=end+1;continue;}
            }
        }
        out+=text[i++];
    }
    return out;
}
// A heading's words: an ATX heading's closing hashes go only as a closing
// sequence, apart from the words by a space, so "C#" keeps its own.
inline std::string HeadingWords(const std::string& heading) {
    std::size_t end=heading.size();
    while(end>0&&heading[end-1]=='#')--end;
    if(end==0)return std::string();
    if(end<heading.size()&&Space(heading[end-1]))return Trim(heading.substr(0,end));
    return heading;
}
}
// GitHub release notes are Markdown. The updater shows them as plain text
// that reads cleanly: headings as their words, list items behind a dash, links
// and images as their text, no emphasis, code or HTML marks, at most one
// blank line in a row. The result is only ever drawn as text, never as a
// format string or markup. The update check makes it once, as the notes
// arrive (github_release_client.cxx), so nothing drawn parses them again.
inline std::string PlainReleaseNotes(const std::string& markdown) {
    using namespace notes_detail;
    // Line ends as \n, and none of the marks Tokenize uses.
    std::string text;text.reserve(markdown.size());
    for(const char c:markdown)if(c!='\r'&&c!='\x01'&&c!='\x02')text+=c;
    const NoteTokens tokens=Tokenize(text);
    // Inline Markdown on text whose kept spans are marks, which it leaves be.
    const auto show=[&](const std::string& s){return Restore(Inline(s),tokens);};
    std::vector<std::string> lines;
    for(const auto& line:tokens.lines) {
        if(line.fence)continue;
        if(line.code){lines.push_back(line.text);continue;}
        const std::string& raw=line.text;
        std::size_t indent=0,at=0;
        while(at<raw.size()&&Space(raw[at])){indent+=raw[at]=='\t'?4:1;++at;}
        std::string body=raw.substr(at);
        while(!body.empty()&&body[0]=='>'){body.erase(0,1);if(!body.empty()&&body[0]==' ')body.erase(0,1);}
        body=Trim(body);
        // A table's divider row goes; a rule or a heading's underline leaves a blank line.
        if(body.find('|')!=std::string::npos&&RuleLine(body,"-|:"))continue;
        if(RuleLine(body,"-")||RuleLine(body,"*")||RuleLine(body,"_")||RuleLine(body,"=")){lines.emplace_back();continue;}
        std::size_t hashes=0;while(hashes<body.size()&&body[hashes]=='#')++hashes;
        if(hashes>=1&&hashes<=6&&(hashes==body.size()||Space(body[hashes]))) {
            const std::string heading=HeadingWords(Trim(body.substr(hashes)));
            if(!lines.empty()&&!lines.back().empty())lines.emplace_back();
            lines.push_back(show(heading));
            continue;
        }
        const std::string pad((indent/2)*2,' ');
        if(body.size()>=2&&(body[0]=='-'||body[0]=='*'||body[0]=='+')&&Space(body[1])) {
            std::string item=Trim(body.substr(2));
            if(StartsWith(item,0,"[ ] ")||StartsWith(item,0,"[x] ")||StartsWith(item,0,"[X] "))item.erase(0,4);
            lines.push_back(pad+"- "+show(item));
            continue;
        }
        if(body.find('|')!=std::string::npos&&body.front()=='|') {
            std::string row;std::size_t cell=1;
            while(cell<body.size()){auto bar=body.find('|',cell);if(bar==std::string::npos)bar=body.size();
                const auto value=Trim(body.substr(cell,bar-cell));if(!value.empty()){if(!row.empty())row+=", ";row+=show(value);}cell=bar+1;}
            lines.push_back(pad+row);
            continue;
        }
        lines.push_back(body.empty()?std::string():pad+show(body));
    }
    std::string out;
    for(const auto& line:lines) {
        std::string trimmed=line;while(!trimmed.empty()&&Space(trimmed.back()))trimmed.pop_back();
        if(trimmed.empty()&&(out.empty()||(out.size()>=2&&out.compare(out.size()-2,2,"\n\n")==0)))continue;
        out+=trimmed;out+='\n';
    }
    while(!out.empty()&&out.back()=='\n')out.pop_back();
    return DrawableUtf8(out);
}
} }
