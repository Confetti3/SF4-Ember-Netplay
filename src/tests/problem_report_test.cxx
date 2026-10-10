#include "../platform/ProblemReport.hxx"
#include "../netplay/BoolPreferences.hxx"
#include <algorithm>
#include <limits>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <utility>

namespace {
void Check(bool ok,int line) { if(!ok)throw std::runtime_error("Problem report check failed at "+std::to_string(line)); }
#define CHECK(value) Check((value),__LINE__)
using namespace sf4e::reports;
Report Fixture() {
    Report report; report.meta={"crash","1.2.3","beta",std::string(64,'a'),"abc123-dirty","Windows 10.0 build 26100"};
    report.meta.exitCode=0xC0000005u; report.meta.exceptionCode=0xC0000005u; report.meta.crashAddress=0x401234;
    report.logs={{"sf4e.log","A readable log\n"},{"launcher.log","Started\n"}};
    report.minidump="MDMP"+std::string(64,'\0'); return report;
}
void Redaction() {
    for(const std::string secret:{"sf4e1:ABCD", "sf4e2:ABCD", "sf4e3:ABCD", "emd2:ABCD", "ember://join/ABCD",
        "https://embernetplay.link/j#7K3M-0X1R-T9PZ", "embernetplay.link/j/7K3M0X1RT9PZ",
        "https://embernetplay.link/s/v1/AbCdEfGhIjKlMnOpQrStUv", "7K3M-0X1R-T9PZ", "7K3M0X1RT9PZ", "7K3M-OX1R-T9PZ", "7K3M0XIRT9PZ", "7k3m-lxlr-t9pz", "ems1_aSecret", "emk1_aSecret",
        "Bearer secret-token", "locator=locator-secret", "write_token=write-secret", "sessionToken=session-secret",
        "discord_token=discord-secret", "mfa.ABCD", "abcdefghijklmnopqrstuvwx.ABCDEF.abcdefghijklmnopqrstuvwx",
        "192.168.1.45", "2001:db8::1234", "::1", "fe80::abcd%12", "2001:db8:0:1:2:3:4:5",
        "Kate@example.com", "AbCdEfGhIjKlMnOpQrStUv"}) {
        const auto redacted=RedactLine("before "+secret+" after");
        CHECK(redacted.find(secret)==std::string::npos); CHECK(redacted.find("before ")==0); CHECK(redacted.compare(redacted.size()-6,6," after")==0);
    }
    CHECK(RedactLine("ip:fe80::abcd%12")=="ip:[ip]");
    CHECK(RedactLine("(ip:fe80::abcd%12),")=="(ip:[ip]),");
    CHECK(RedactLine("ip:fe80::abcd.")=="ip:[ip].");
    CHECK(RedactLine("[::ffff:192.168.1.45]")=="[[ip]]");
    CHECK(RedactLine("(7K3M-OX1R-T9PZ), 7K3M0XIRT9PZ!")=="([invite]), [invite]!");
    CHECK(RedactLine("7K3M--OX1R-T9PZ")=="[invite]");
    CHECK(RedactLine("x7K3M0XIRT9PZy")=="x7K3M0XIRT9PZy");
    CHECK(RedactLine("7K3M-OX1R-T9P")=="7K3M-OX1R-T9P");
    CHECK(RedactLine("ip:fe80::")=="ip:[ip]");
    CHECK(RedactLine(R"(C:\Users\Kate)")==R"(C:\Users\<user>)");
    CHECK(RedactLine(R"("C:\Users\Kate",)")==R"("C:\Users\<user>",)");
    CHECK(RedactLine(R"((D:/Users/Kate);)")==R"((D:/Users/<user>);)");
    CHECK(RedactLine(R"("C:\Users\Kate Smith")")==R"("C:\Users\<user>")");
    CHECK(RedactLine(R"(C:\Users\Kate Smith\logs)")==R"(C:\Users\<user>\logs)");
    CHECK(RedactLine("C:\\Users\\Kate\\logs\\sf4e.log")=="C:\\Users\\<user>\\logs\\sf4e.log");
    CHECK(RedactLine("D:/Users/Someone/logs")=="D:/Users/<user>/logs");
    CHECK(RedactLine(R"(C:\\Users\\Kate\\logs)")==R"(C:\\Users\\<user>\\logs)");
    const std::pair<const char*,const char*> boundaries[]={
        {R"(profile=C:\Users\Kate Smith)",R"(profile=C:\Users\<user>)"},
        {R"(profile=C:\Users\Kate status=ready; other=D:/cache)",R"(profile=C:\Users\<user> status=ready; other=D:/cache)"},
        {"(sf4e3:ABCD), next","([invite]), next"},
        {R"(profile=C:\Users\Kate Smith status=ready; other=D:/cache)",R"(profile=C:\Users\<user> status=ready; other=D:/cache)"},
        {R"(profile="C:\Users\Kate Smith"; next=ready)",R"(profile="C:\Users\<user>"; next=ready)"},
        {R"(profile=D:/Users/Kate Smith/logs; status=ready)",R"(profile=D:/Users/<user>/logs; status=ready)"},
        {"sf4e3:ABCD,emd2:EFGH;locator=hidden; status=ready","[invite],[invite];locator=[secret]; status=ready"},
        {"(https://embernetplay.link/j#7K3M-OX1R-T9PZ), next!","([invite]), next!"},
        {"[ember://join/7K3M0XIRT9PZ]. next","[[invite]]. next"},
        {"before (Bearer hidden), locator=private; after","before (Bearer [token]), locator=[secret]; after"},
        {R"(C:\Users\mfa.ABCD@example.com\logs next=ready)",R"(C:\Users\<user>\logs next=ready)"},
        {"12345678901234567890@example.com; next","[email]; next"},
        {"locator=sf4e3:ABCD; next","locator=[invite]; next"},
        {"locator=mfa.ABCD; next","locator=[secret]; next"},
        {"locator=hidden,write_token=private; next","locator=[secret],write_token=[secret]; next"},
        {"notlocator=visible; status=ready","notlocator=visible; status=ready"},
        {"sf4e3:ABCD;","[invite];"},
        {"sf4e3:AB","[invite]"},
        {"(sf4e3:), next","([invite]), next"},
        {R"(profile=C:\Users\Kate Sm)",R"(profile=C:\Users\<user>)"},
        {R"(profile=C:\Users\Kate status=)",R"(profile=C:\Users\<user> status=)"},
        {R"(profile=C:\Users\Kate Smith status = ready; other=D:/cache)",R"(profile=C:\Users\<user> status = ready; other=D:/cache)"},
        {"profile=C:\\Users\\Kate Smith\tstatus=ready\r", "profile=C:\\Users\\<user>\tstatus=ready\r"},
        {R"(profile='C:\Users\Kate Smith'; next=ready)",R"(profile='C:\Users\<user>'; next=ready)"},
        {"locator=; profile=C:\\Users\\", "locator=; profile=C:\\Users\\"},
        {"(7K3M-OX1R-T9PZ), final suffix","([invite]), final suffix"},
        {"before 7K3M-OX1R-T9PZ,7K3M0XIRT9PZ after","before [invite],[invite] after"},
        {"", ""},
        {"unchanged final suffix", "unchanged final suffix"}
    };
    // Exact output checks cover both secret removal and every original gap,
    // wrapper and final suffix, including secrets cut at the end of a line.
    for(const auto& test:boundaries) CHECK(RedactLine(test.first)==test.second);
    for(const std::string line:{"Game exited with code 0xc0000005", "RTT 42 ms, input delay 3", "sf4e::Session ready at 12:34:56",
        "Windows 10.0 build 26100", "module.dll+0x1234", "https://github.com/owner/repo", "No credentials here."}) CHECK(RedactLine(line)==line);
}
void Tails() {
    CHECK(Utf8Tail(std::string(LogLimit,'a')).size()==LogLimit);
    CHECK(Utf8Tail("prefix"+std::string(LogLimit,'b'))==std::string(LogLimit,'b'));
    const std::string utf8="\xF0\x9F\x8C\x9F";
    CHECK(Utf8Tail("abc"+utf8+"def",6)=="def");
    CHECK(Utf8Tail(utf8+std::string(LogLimit-2,'x'))==std::string(LogLimit-2,'x'));
    CHECK(Utf8Tail(std::string("a\0b",3))=="a?b");
    CHECK(Utf8Tail("abc\xFF")=="abc?"); CHECK(Utf8Tail("abc\xE2\x82")=="abc??");
    CHECK(CharacterCount(utf8+"abc")==4);
}
void MultipartCases() {
    auto report=Fixture(); Submission submission{"A problem after joining.",true}; Multipart body; std::string error;
    CHECK(BuildMultipart(report,submission,"EmberTestBoundary",body,error));
    CHECK(body.contentType=="multipart/form-data; boundary=EmberTestBoundary");
    CHECK(body.body.find("--EmberTestBoundary\r\nContent-Disposition: form-data; name=\"meta\"\r\n") == 0);
    CHECK(body.body.find("name=\"log\"; filename=\"sf4e.log\"")!=std::string::npos);
    CHECK(body.body.find("name=\"minidump\"; filename=\"crash.dmp\"")!=std::string::npos);
    CHECK(body.body.find(report.minidump)!=std::string::npos);
    const std::string ending="--EmberTestBoundary--\r\n"; CHECK(body.body.compare(body.body.size()-ending.size(),ending.size(),ending)==0);
    const auto meta=MetaJson(report.meta,submission.comment);
    CHECK(meta.size()==11&&meta["schema"]==1&&meta["kind"]=="crash"&&meta["build_id"]==std::string(64,'a'));
    CHECK(meta["app_version"]=="1.2.3"&&meta["channel"]=="beta"&&meta["source_revision"]=="abc123-dirty");
    CHECK(meta["windows_version"]=="Windows 10.0 build 26100"&&meta["exit_code"]==0xC0000005u);
    CHECK(meta["exception_code"]=="0xc0000005"&&meta["crash_address"]=="0x401234"&&meta["comment"]==submission.comment);
    submission.includeDump=false; CHECK(BuildMultipart(report,submission,"EmberTestBoundary",body,error));
    CHECK(body.body.find("name=\"minidump\"")==std::string::npos);
    report.logs[0].text=std::string(LogLimit,'x'); CHECK(BuildMultipart(report,submission,"EmberTestBoundary",body,error));
    report.logs[0].text+='x'; CHECK(!BuildMultipart(report,submission,"EmberTestBoundary",body,error)); CHECK(body.body.empty());
    report=Fixture(); report.minidump="MDMP"+std::string(DumpLimit-4,'\0'); submission.includeDump=true;
    CHECK(BuildMultipart(report,submission,"EmberTestBoundary",body,error));
    report.minidump+='x'; CHECK(!BuildMultipart(report,submission,"EmberTestBoundary",body,error));
    report=Fixture(); report.minidump="BAD!"; CHECK(!BuildMultipart(report,submission,"EmberTestBoundary",body,error));
    report=Fixture(); report.logs.push_back(report.logs.front()); CHECK(!BuildMultipart(report,submission,"EmberTestBoundary",body,error));
    report=Fixture(); report.logs[0].name="../sf4e.log"; CHECK(!BuildMultipart(report,submission,"EmberTestBoundary",body,error));
    report=Fixture(); report.logs[0].text="EmberTestBoundary"; CHECK(!BuildMultipart(report,submission,"EmberTestBoundary",body,error));
    report=Fixture(); CHECK(!BuildMultipart(report,submission,"bad\r\nboundary",body,error));
    report.meta.buildId="bad"; CHECK(!BuildMultipart(report,submission,"EmberTestBoundary",body,error));
    report=Fixture(); report.meta.channel="prerelease"; CHECK(!BuildMultipart(report,submission,"EmberTestBoundary",body,error));
    report=Fixture(); report.meta.kind="problem"; report.meta.exitCode.reset(); report.meta.exceptionCode.reset(); report.meta.crashAddress.reset();
    CHECK(MetaJson(report.meta,"").size()==7); submission.comment=" \t\n"; CHECK(!BuildMultipart(report,submission,"EmberTestBoundary",body,error));
    submission.comment=std::string(2000,'a'); CHECK(BuildMultipart(report,submission,"EmberTestBoundary",body,error));
    submission.comment+='a'; CHECK(!BuildMultipart(report,submission,"EmberTestBoundary",body,error));
    submission.comment.clear(); for(int i=0;i<2000;++i)submission.comment+="\xF0\x9F\x8C\x9F";
    CHECK(BuildMultipart(report,submission,"EmberTestBoundary",body,error));
    using sf4e::updates::UpdateChannel;
    CHECK(std::string(ReportChannel(UpdateChannel::Stable))=="stable"&&std::string(ReportChannel(UpdateChannel::Beta))=="beta");
    CHECK(std::string(ReportChannel(UpdateChannel::Nightly))=="nightly");
}
void Collection() {
    const auto path=std::filesystem::temp_directory_path()/std::filesystem::path("ember-report-test-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(path);
    struct Cleanup { std::filesystem::path path; ~Cleanup(){std::error_code ec;std::filesystem::remove_all(path,ec);} } cleanup{path};
    const auto write=[&](const char* name,const std::string& bytes){std::ofstream file(path/name,std::ios::binary);file.write(bytes.data(),bytes.size());};
    write("sf4e.log","old\n"+std::string(LogLimit,'x')+"\nBearer private\nGame ready\n");
    write("sf4e-crash.log","kind=unhandled_exception code=0xC0000005 address=0x401234 module=Sidecar.dll\n");
    write("small.dmp","MDMPpayload"); CrashContext context; context.smallDump=path/"small.dmp"; context.started=(std::filesystem::file_time_type::min)();
    auto report=Collect(path,Fixture().meta,context,{});
    CHECK(report.logs.size()==2&&report.logs[0].text.find("private")==std::string::npos);
    CHECK(report.logs[0].text.find("Game ready")!=std::string::npos&&report.logs[0].text.size()<=LogLimit);
    CHECK(report.meta.exceptionCode==0xC0000005u&&report.meta.crashAddress==0x401234&&report.minidump=="MDMPpayload");
    context.started=(std::filesystem::file_time_type::max)(); report=Collect(path,Fixture().meta,context,{}); CHECK(!report.meta.exceptionCode);
    write("small.dmp","MDMP"+std::string(DumpLimit,'x')); report=Collect(path,Fixture().meta,context,{}); CHECK(report.minidump.empty());
    // A tail cut through an invitation/profile/address discards the partial
    // first line, rather than offering an unrecognizable secret fragment.
    write("sf4e.log", "7K3M-OX1R-T9PZ C:\\Users\\Kate ip:fe80::abcd%12 " + std::string(LogLimit,'x') + "\nSafe line\n");
    report=Collect(path,Fixture().meta,context,{});
    CHECK(report.logs[0].text=="Safe line\n");
    // An oversized single line has no complete suffix line to offer.
    write("sf4e.log","sf4e3:ABCD profile=C:\\Users\\Kate Smith "+std::string(LogLimit,'x'));
    report=Collect(path,Fixture().meta,context,{}); CHECK(report.logs[0].text.empty());
    // A complete line that fits exactly is redacted without tail discarding.
    const std::string last="\n(sf4e3:ABCD), next\n";
    write("sf4e.log",std::string(LogLimit-last.size(),' ')+last);
    report=Collect(path,Fixture().meta,context,{});
    CHECK(report.logs[0].text==std::string(LogLimit-last.size(),' ')+"\n([invite]), next\n");
    bool cancelled=false; try { Collect(path,Fixture().meta,context,[]{return true;}); } catch (const std::runtime_error&) { cancelled=true; } CHECK(cancelled);
    // A log rotated, replaced or cut short after its tail was read: the partial
    // first line is still dropped, decided from the file as it was read.
    const std::string rotated="sf4e3:ABCD C:\\Users\\Kate "+std::string(LogLimit,'x')+"\nSafe line\n";
    for(const char* after:{"","small\n"}) {
        write("sf4e.log",rotated);
        report=Collect(path,Fixture().meta,context,{},{},[&](const std::filesystem::path& read){
            if(read.filename()=="sf4e.log") write("sf4e.log",after);});
        CHECK(report.logs[0].text=="Safe line\n");
    }
    write("sf4e.log",rotated);
    report=Collect(path,Fixture().meta,context,{},{},[&](const std::filesystem::path& read){
        if(read.filename()=="sf4e.log") std::filesystem::remove(read);});
    CHECK(report.logs[0].text=="Safe line\n");
}
// The account's names wherever one stands alone: the profile folder in its
// long and 8.3 forms, the user and PC names and the player name, in any case
// for ASCII and in the case forms given for other characters.
Account Kate() { Account account; account.names={"KATE-PC","KATE~1","Kate"}; return account; }
void AccountRedaction() {
    const auto account=Kate();
    const std::pair<const char*,const char*> cases[]={
        {R"(Game directory: C:\Users\Kate\Games\SF4)",R"(Game directory: C:\Users\<user>\Games\SF4)"},
        {R"(json: "C:\\Users\\Kate\\AppData\\Roaming")",R"(json: "C:\\Users\\<user>\\AppData\\Roaming")"},
        {"url: c:/users/kate/x.dmp","url: c:/users/<user>/x.dmp"},
        {R"(short: C:\Users\KATE~1\AppData\Local\Temp)",R"(short: C:\Users\<user>\AppData\Local\Temp)"},
        {R"(elsewhere: D:\PROFIL~1\KATE~1\x and D:\Profiles\kate\y)",R"(elsewhere: D:\PROFIL~1\<user>\x and D:\Profiles\<user>\y)"},
        {"user=Kate pc=KATE-PC.","user=<user> pc=<user>."},
        {R"(D:\Games\kate\Steam)",R"(D:\Games\<user>\Steam)"},
        // Another account's folder and longer words keep their names.
        {R"(C:\Data\Katerina\x)",R"(C:\Data\Katerina\x)"},
        {"Kate2 Kate_x xKate","Kate2 Kate_x xKate"},
        {"plain line","plain line"}};
    for(const auto& test:cases) if(RedactLine(test.first,account)!=test.second) throw std::runtime_error("Account redaction gave: "+RedactLine(test.first,account));
    // Every name of this PC goes, however short, wherever it stands alone.
    Account shortNames; shortNames.names={"Al","K"};
    const std::pair<const char*,const char*> shortCases[]={
        {R"(Al said C:\Users\Al\x)",R"(<user> said C:\Users\<user>\x)"},
        {"player=Al; pc=K","player=<user>; pc=<user>"},
        {R"({"player":"Al","pc":"K"})",R"({"player":"<user>","pc":"<user>"})"},
        {R"(\\server\profiles\Al\logs)",R"(\\server\profiles\<user>\logs)"},
        {R"(C:\Users\Al\AppData\Roaming\sf4e)",R"(C:\Users\<user>\AppData\Roaming\sf4e)"},
        {R"(D:\Profiles\K\x)",R"(D:\Profiles\<user>\x)"},
        {"K said OK to Alpha and Kal","<user> said OK to Alpha and Kal"}};
    for(const auto& test:shortCases) if(RedactLine(test.first,shortNames)!=test.second) throw std::runtime_error("Short name redaction gave: "+RedactLine(test.first,shortNames));
    // The other rules still apply beside the names.
    CHECK(RedactLine("Kate joined sf4e3:ABCD from 192.168.1.45",account)=="<user> joined [invite] from [ip]");
    // Unicode names match in any case from the one form Windows gives, and
    // punctuation in any script ends a word; glued to another letter of its
    // script a name is part of a longer word, as with ASCII.
    const std::string cyrillic="\xD0\xAF\xD1\x80\xD0\xBE\xD1\x81\xD0\xBB\xD0\xB0\xD0\xB2";      // Ярослав
    const std::string mixed="\xD1\x8F\xD0\xA0\xD0\xBE\xD1\x81\xD0\xBB\xD0\xB0\xD0\xB2";         // яРослав
    const std::string upper="\xD0\xAF\xD0\xA0\xD0\x9E\xD0\xA1\xD0\x9B\xD0\x90\xD0\x92";         // ЯРОСЛАВ
    const std::string japanese="\xE5\xB1\xB1\xE7\x94\xB0\xE5\xA4\xAA";                         // 山田太
    Account unicode; unicode.names={cyrillic,japanese};
    const std::pair<std::string,std::string> fixtures[]={
        {"path D:\\Home\\"+cyrillic+"\\AppData name "+mixed+", "+upper,"path D:\\Home\\<user>\\AppData name <user>, <user>"},
        {"ja "+japanese+" end","ja <user> end"},
        {"name=\xE3\x80\x8C"+japanese+"\xE3\x80\x8D","name=\xE3\x80\x8C<user>\xE3\x80\x8D"},              // name=「山田太」
        {"ja-word "+japanese+"\xE5\xA4\xAA","ja-word "+japanese+"\xE5\xA4\xAA"},                          // 山田太太 is another name
        {"emoji "+japanese+"\xF0\x9F\x8E\xAE","emoji <user>\xF0\x9F\x8E\xAE"}};
    for(const auto& test:fixtures) if(RedactLine(test.first,unicode)!=test.second) throw std::runtime_error("Unicode redaction gave: "+RedactLine(test.first,unicode));
    // Unicode full case folding: the sharp s, its capital and "ss" are one,
    // in plain text and in JSON escapes; a match covers whole characters, and
    // a letter from a supplementary plane continues a word like any other.
    Account street; street.names={"Stra\xC3\x9F" "e"};
    Account strass; strass.names={"Stras"};
    const std::pair<std::string,std::string> folding[]={
        {"a STRASSE b","a <user> b"}, {"a strasse b","a <user> b"},
        {"a STRA\xE1\xBA\x9E" "E b","a <user> b"}, {"a Stra\xC3\x9F" "e b","a <user> b"},
        {R"({"n":"Stra\u00dfe","m":"STRA\u1E9EE"})",R"({"n":"<user>","m":"<user>"})"},
        {"a Stra\xC3\x9F" "en b","a Stra\xC3\x9F" "en b"},
        {"\xF0\x90\x90\xA8Strasse","\xF0\x90\x90\xA8Strasse"},
        {"Strasse\xF0\x90\x90\xA8","Strasse\xF0\x90\x90\xA8"},
        {"\xF0\x9F\x8E\xAEStrasse!","\xF0\x9F\x8E\xAE<user>!"},
        {R"({"n":"\ud801\udc28Strasse"})",R"({"n":"\ud801\udc28Strasse"})"}};
    for(const auto& test:folding) if(RedactLine(test.first,street)!=test.second) throw std::runtime_error("Folding redaction gave: "+RedactLine(test.first,street));
    // Part of what one character folds to is not a match.
    CHECK(RedactLine("a Stra\xC3\x9F" "e b",strass)=="a Stra\xC3\x9F" "e b");
    // Inside JSON strings: escaped quotes and backslashes, and hex escapes in
    // either case, are read as the characters they stand for.
    Account escaped; escaped.names={"A\"B","C\\D","Zo\xC3\xAB",cyrillic};
    const std::pair<std::string,std::string> json[]={
        {R"({"name":"A\"B","x":1})",R"({"name":"<user>","x":1})"},
        {R"({"name":"C\\D"})",R"({"name":"<user>"})"},
        {R"({"name":"Zo\u00eb"})",R"({"name":"<user>"})"},
        {R"({"name":"ZO\u00CB!"})",R"({"name":"<user>!"})"},
        {R"({"name":"\u042f\u0440\u043e\u0441\u043b\u0430\u0432"})",R"({"name":"<user>"})"},
        {"plain Zo\xC3\xAB and A\"B","plain <user> and <user>"},
        // A path whose folder starts with "n" still matches as written, where
        // the JSON reading would see a newline.
        {R"(D:\games\nate\x)",R"(D:\games\<user>\x)"}};
    Account nate; nate.names={"nate"};
    for(const auto& test:json) {
        const auto& who=test.first.find("nate")!=std::string::npos?nate:escaped;
        if(RedactLine(test.first,who)!=test.second) throw std::runtime_error("JSON redaction gave: "+RedactLine(test.first,who));
    }
}
void SentRecord() {
    std::vector<Record> sent;
    for(int i=0;i<60;++i) {
        Record r; r.time=1000+i;
        r.status=i%4==0?Status::Received:i%4==1?Status::Refused:i%4==2?Status::NotSent:Status::Pending;
        r.id=r.status==Status::Received?std::string(32,'a'):std::string();
        r.reason=r.status==Status::Refused?"rate_limit":""; r.kind=i%2?"problem":"crash";
        r.attempt=std::string(16,'0'+i%10); r.dump=i%2==0; r.automatic=i%5==0;
        Append(sent,r);
    }
    CHECK(sent.size()==SentKept&&sent.front().time==1010&&sent.back().time==1059);
    const auto again=ParseSent(SentJson(sent));
    CHECK(again.has_value()&&again->size()==sent.size());
    for(std::size_t i=0;i<again->size();++i) {
        const auto& a=(*again)[i]; const auto& b=sent[i];
        CHECK(a.time==b.time&&a.status==b.status&&a.id==b.id&&a.reason==b.reason&&a.kind==b.kind&&a.attempt==b.attempt&&a.dump==b.dump&&a.automatic==b.automatic);
    }
    CHECK(ParseSent(R"({"schema":1,"reports":[]})").has_value()&&ParseSent(R"({"schema":1,"reports":[]})")->empty());
    // Anything this code would not write is unreadable as a whole, never a
    // shorter record: a damaged entry must not give the day's sends back.
    const std::string entry=R"({"time":5,"status":"received","kind":"crash","id":"","reason":"","attempt":"","dump":false,"automatic":true})";
    CHECK(ParseSent(R"({"schema":1,"reports":[)"+entry+"]}").has_value());
    for(const std::string bad:{std::string(""),std::string("[]"),std::string(R"({"schema":2,"reports":[]})"),
        R"({"schema":1,"reports":[)"+entry+",3]}",
        std::string(R"({"schema":1,"reports":[{"time":"6","status":"received","kind":"crash","id":"","reason":"","attempt":"","dump":false,"automatic":true}]})"),
        std::string(R"({"schema":1,"reports":[{"time":7,"status":"lost","kind":"crash","id":"","reason":"","attempt":"","dump":false,"automatic":true}]})"),
        std::string(R"({"schema":1,"reports":[{"time":8,"status":"received","kind":"crash","id":"../../x","reason":"","attempt":"","dump":false,"automatic":true}]})"),
        std::string(R"({"schema":1,"reports":[{"time":8,"status":"received","kind":"crash","id":"","reason":"A B","attempt":"","dump":false,"automatic":true}]})"),
        std::string(R"({"schema":1,"reports":[{"time":8,"status":"received","kind":"crash","id":"","reason":"","attempt":"","dump":"yes","automatic":true}]})"),
        std::string(R"({"schema":1,"reports":[{"time":8,"status":"received","kind":"crash","id":"","reason":"","attempt":"","dump":false}]})"),
        std::string(R"({"schema":1,"reports":[{"time":18446744073709551615,"status":"received","kind":"crash","id":"","reason":"","attempt":"","dump":false,"automatic":true}]})"),
        std::string(R"({"schema":1,"reports":[{"time":9223372036854775808,"status":"received","kind":"crash","id":"","reason":"","attempt":"","dump":false,"automatic":true}]})"),
        std::string(SentFileLimit+1,' ')})
        CHECK(!ParseSent(bad).has_value());
}
void Timestamps() {
    // The largest time the record can hold is read, and as a time ahead of now
    // it counts toward the day's sends; one past it makes the record unreadable.
    const auto entry=[](const char* time){return std::string(R"({"schema":1,"reports":[{"time":)")+time+
        R"(,"status":"received","kind":"crash","id":"","reason":"","attempt":"","dump":false,"automatic":true}]})";};
    const auto largest=ParseSent(entry("9223372036854775807"));
    CHECK(largest.has_value()&&largest->size()==1&&(*largest)[0].time==(std::numeric_limits<std::int64_t>::max)());
    std::vector<Record> future(3,(*largest)[0]);
    CHECK(!AutomaticAllowed(future,1'000'000));
    CHECK(!ParseSent(entry("18446744073709551615")).has_value()&&!ParseSent(entry("9223372036854775808")).has_value());
    CHECK(ParseSent(entry("-5")).has_value());
}
void AutomaticCap() {
    const std::int64_t now=1'000'000;
    std::vector<Record> sent;
    CHECK(AutomaticAllowed(sent,now));
    Record old; old.time=now-24*60*60;
    for(int i=0;i<10;++i) Append(sent,old);
    CHECK(AutomaticAllowed(sent,now));
    Record recent; recent.time=now-60;
    Append(sent,recent); Append(sent,recent);
    CHECK(AutomaticAllowed(sent,now));
    // A clock set back counts the report from "the future" too.
    Record ahead; ahead.time=now+3600; Append(sent,ahead);
    CHECK(!AutomaticAllowed(sent,now));
}
void SubmissionDefaults() {
    // Every report starts without the dump.
    CHECK(!Submission{}.includeDump);
}
void SettingKey() {
    using sf4e::netplay::BoolPreferences;
    using sf4e::netplay::PlayerPreferences;
    const auto found=std::find_if(std::begin(BoolPreferences),std::end(BoolPreferences),
        [](const sf4e::netplay::BoolPreference& p){return p.member==&PlayerPreferences::sendProblemReports;});
    CHECK(found!=std::end(BoolPreferences)&&std::string(found->key)=="sendProblemReports");
    CHECK(!PlayerPreferences().sendProblemReports);
}
}
int main() {
    try { Redaction(); AccountRedaction(); Tails(); MultipartCases(); Collection(); SentRecord(); Timestamps(); AutomaticCap(); SubmissionDefaults(); SettingKey();
        std::cout<<"Problem report checks passed.\n"; return 0; }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
