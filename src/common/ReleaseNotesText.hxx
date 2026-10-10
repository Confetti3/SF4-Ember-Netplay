#pragma once
#include <cstddef>
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
// Valid UTF-8 the menu fonts can draw. A cut through a character (the
// client keeps the first 2,000 bytes), control characters, emoji past the
// Basic Multilingual Plane and the invisible joiners and variation selectors
// that go with them are dropped; a tab reads as a space.
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
        const unsigned least[]={0,0,0x80,0x800,0x10000};
        const bool drawable=codepoint>=least[length]&&codepoint<=0xFFFF&&!(codepoint>=0xD800&&codepoint<=0xDFFF)&&
            codepoint>=0xA0&&!(codepoint>=0x200B&&codepoint<=0x200F)&&!(codepoint>=0x2060&&codepoint<=0x2064)&&
            !(codepoint>=0xFE00&&codepoint<=0xFE0F)&&codepoint!=0xFEFF&&codepoint!=0xFFFD;
        if(drawable)out.append(text,i,length);
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
// Inline Markdown as the words it shows: emphasis and code marks go, a link
// or an image reads as its text, HTML tags go and a few entities are spelled out.
inline std::string Inline(const std::string& s) {
    std::string out;
    for(std::size_t i=0;i<s.size();) {
        const char c=s[i];
        if(c=='\\'&&i+1<s.size()&&Punctuation(s[i+1])){out+=s[i+1];i+=2;continue;}
        if(c=='`') {
            std::size_t run=0;while(i+run<s.size()&&s[i+run]=='`')++run;
            const auto close=s.find(std::string(run,'`'),i+run);
            if(close==std::string::npos){out.append(run,'`');i+=run;continue;}
            out+=Trim(s.substr(i+run,close-i-run));i=close+run;continue;
        }
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
}
// GitHub release notes are Markdown. The updater shows them as plain text
// that reads cleanly: headings as their words, list items behind a dash, links
// and images as their text, no emphasis, code or HTML marks, at most one
// blank line in a row. The result is only ever drawn as text, never as a
// format string or markup.
inline std::string PlainReleaseNotes(const std::string& markdown) {
    using namespace notes_detail;
    std::string text;text.reserve(markdown.size());
    for(std::size_t i=0;i<markdown.size();) {
        if(StartsWith(markdown,i,"<!--")){const auto end=markdown.find("-->",i+4);i=end==std::string::npos?markdown.size():end+3;continue;}
        if(markdown[i]!='\r')text+=markdown[i];
        ++i;
    }
    std::vector<std::string> lines;
    bool fenced=false;
    for(std::size_t start=0;start<=text.size();) {
        auto end=text.find('\n',start);if(end==std::string::npos)end=text.size();
        const std::string raw=text.substr(start,end-start);start=end+1;
        std::size_t indent=0,at=0;
        while(at<raw.size()&&Space(raw[at])){indent+=raw[at]=='\t'?4:1;++at;}
        std::string body=raw.substr(at);
        if(StartsWith(body,0,"```")||StartsWith(body,0,"~~~")){fenced=!fenced;continue;}
        if(fenced){lines.push_back(raw);continue;}
        while(!body.empty()&&body[0]=='>'){body.erase(0,1);if(!body.empty()&&body[0]==' ')body.erase(0,1);}
        body=Trim(body);
        // A table's divider row goes; a rule or a heading's underline leaves a blank line.
        if(body.find('|')!=std::string::npos&&RuleLine(body,"-|:"))continue;
        if(RuleLine(body,"-")||RuleLine(body,"*")||RuleLine(body,"_")||RuleLine(body,"=")){lines.emplace_back();continue;}
        std::size_t hashes=0;while(hashes<body.size()&&body[hashes]=='#')++hashes;
        if(hashes>=1&&hashes<=6&&(hashes==body.size()||Space(body[hashes]))) {
            std::string heading=Trim(body.substr(hashes));
            while(!heading.empty()&&heading.back()=='#')heading.pop_back();
            if(!lines.empty()&&!lines.back().empty())lines.emplace_back();
            lines.push_back(Inline(Trim(heading)));
            continue;
        }
        const std::string pad((indent/2)*2,' ');
        if(body.size()>=2&&(body[0]=='-'||body[0]=='*'||body[0]=='+')&&Space(body[1])) {
            std::string item=Trim(body.substr(2));
            if(StartsWith(item,0,"[ ] ")||StartsWith(item,0,"[x] ")||StartsWith(item,0,"[X] "))item.erase(0,4);
            lines.push_back(pad+"- "+Inline(item));
            continue;
        }
        if(body.find('|')!=std::string::npos&&body.front()=='|') {
            std::string row;std::size_t cell=1;
            while(cell<body.size()){auto bar=body.find('|',cell);if(bar==std::string::npos)bar=body.size();
                const auto value=Trim(body.substr(cell,bar-cell));if(!value.empty()){if(!row.empty())row+=", ";row+=Inline(value);}cell=bar+1;}
            lines.push_back(pad+row);
            continue;
        }
        lines.push_back(body.empty()?std::string():pad+Inline(body));
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
