#pragma once
#include "../platform/ReportWorkflow.hxx"
#include "../netplay/SettingsStore.hxx"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

namespace report_workflow_tests {
using namespace sf4e::reports;
inline std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
inline Identity Known() { Identity identity; identity.ok = true; identity.account.names = {"Tester"}; return identity; }

// The preview, its submission and every answer the intake can give.
inline void Previews(const std::filesystem::path& root) {
    Report fixture; fixture.meta = {"problem", "1.2.3", "stable", std::string(64, 'a'), "revision", "Windows test"};
    fixture.logs = {{"sf4e.log", "redacted preview"}};
    WorkflowState state; int collected = 0, uploaded = 0;
    bool cancelled = false;
    sf4e::HttpResponse response;
    WorkflowDependencies dependencies;
    dependencies.history = std::make_shared<ReportHistory>(root / L"previews.json");
    dependencies.identity = [](const std::function<bool()>&) { return Known(); };
    dependencies.collect = [&](const Preparation&, const Account& account, const std::function<bool()>&) {
        CHECK(account.names == Known().account.names); ++collected; return fixture;
    };
    dependencies.upload = [&](const Multipart& body, const std::function<bool()>&) {
        ++uploaded; CHECK(body.body.find("redacted preview") != std::string::npos); return response;
    };
    const auto cancel = [&] { return cancelled; };
    const Operation prepare = Preparation{};
    CHECK(ReportWorkflow::Begin(prepare, state));
    CHECK(state.phase == Phase::Preparing && !state.preview && state.preparation == 1);
    CHECK(!ReportWorkflow::Begin(prepare, state));
    state = ReportWorkflow::Execute(prepare, state, cancel, dependencies);
    CHECK(state.phase == Phase::Preview && collected == 1 && uploaded == 0);
    auto preview = state.preview;
    Submission submission{"A problem", false, preview};
    auto foreign = submission; foreign.preview = std::make_shared<const Report>(*preview);
    CHECK(!ReportWorkflow::Begin(foreign, state));
    CHECK(ReportWorkflow::Begin(submission, state));
    CHECK(state.phase == Phase::Submitting);
    const auto rejected = ReportWorkflow::Execute(foreign, state, cancel, dependencies);
    CHECK(rejected.phase == Phase::Failed && uploaded == 0);
    response.request.statusCode = 429; response.retryAfter = "0";
    state = ReportWorkflow::Execute(submission, state, cancel, dependencies);
    CHECK(state.phase == Phase::Retry && state.preview == preview && uploaded == 1);
    CHECK(state.retryAt > std::chrono::steady_clock::now());
    CHECK(!ReportWorkflow::Begin(submission, state));
    state.retryAt = {}; CHECK(ReportWorkflow::Begin(submission, state));
    response.retryAfter = "Wed, 01 Jan 2020 00:00:00 GMT";
    state = ReportWorkflow::Execute(submission, state, cancel, dependencies);
    CHECK(state.phase == Phase::Retry && state.retryAt <= std::chrono::steady_clock::now() + std::chrono::seconds(1));
    state.retryAt = {}; CHECK(ReportWorkflow::Begin(submission, state));
    response.request.statusCode = 503;
    state = ReportWorkflow::Execute(submission, state, cancel, dependencies);
    CHECK(state.phase == Phase::Failed && state.preview == preview);
    CHECK(ReportWorkflow::Begin(submission, state));
    cancelled = true;
    const auto before = uploaded;
    state = ReportWorkflow::Execute(submission, state, cancel, dependencies);
    CHECK(state.phase == Phase::Cancelled && state.preview == preview && uploaded == before);
    cancelled = false; CHECK(ReportWorkflow::Begin(submission, state));
    response.request.ok = true; response.body = "{\"id\":\"bad\"}";
    state = ReportWorkflow::Execute(submission, state, cancel, dependencies);
    CHECK(state.phase == Phase::Failed && state.id.empty());
    CHECK(ReportWorkflow::Begin(submission, state));
    response.body = "{\"id\":\"" + std::string(32, 'b') + "\"}";
    // A receipt wins a cancellation that raced completion/teardown.
    dependencies.upload = [&](const Multipart&, const std::function<bool()>&) { cancelled = true; return response; };
    state = ReportWorkflow::Execute(submission, state, cancel, dependencies);
    CHECK(state.phase == Phase::Sent && state.id == std::string(32, 'b') && state.preview == preview);
    CHECK(!ReportWorkflow::Begin(submission, state));
    cancelled = false;
    CHECK(ReportWorkflow::Begin(prepare, state));
    CHECK(state.phase == Phase::Preparing && !state.preview && state.id.empty() && state.preparation == 2);
    state = ReportWorkflow::Execute(prepare, state, [] { return true; }, dependencies);
    CHECK(state.phase == Phase::Cancelled && !state.preview && collected == 1 && !state.record);
    // Every send that reached the network is in the record, completed.
    const auto history = dependencies.history->Load();
    CHECK(history.readable && history.sent.size() == 5);
    CHECK(std::none_of(history.sent.begin(), history.sent.end(), [](const Record& r) { return r.status == Status::Pending || r.automatic; }));
    CHECK(history.sent.back().status == Status::Received && history.sent.back().id == std::string(32, 'b') && history.sent.back().kind == "problem");
    // Without the player's names nothing is prepared.
    dependencies.identity = [](const std::function<bool()>&) { return Identity{}; };
    CHECK(ReportWorkflow::Begin(prepare, state));
    state = ReportWorkflow::Execute(prepare, state, cancel, dependencies);
    CHECK(state.phase == Phase::Failed && !state.preview && collected == 1);
}

// What each upload adds to the sent reports, and the dump: it goes only with
// the tick for that report. A record that cannot be written stops a send.
inline void Records(const std::filesystem::path& root) {
    Report crash; crash.meta = {"crash", "1.2.3", "nightly", std::string(64, 'c'), "revision", "Windows test"};
    crash.logs = {{"sf4e.log", "log"}}; crash.minidump = "MDMP" + std::string(32, 'd');
    sf4e::HttpResponse response; std::string lastBody; int uploads = 0; bool publishes = true;
    WorkflowDependencies dependencies;
    dependencies.history = std::make_shared<ReportHistory>(root / L"records.json", [&](const std::filesystem::path& path, const std::string& text) {
        if (!publishes) return false;
        std::ofstream file(path, std::ios::binary | std::ios::trunc); file << text; return static_cast<bool>(file);
    });
    dependencies.identity = [](const std::function<bool()>&) { return Known(); };
    dependencies.collect = [&](const Preparation&, const Account&, const std::function<bool()>&) { return crash; };
    dependencies.upload = [&](const Multipart& body, const std::function<bool()>&) { ++uploads; lastBody = body.body; return response; };
    WorkflowState state;
    const auto prepare = [&] {
        Preparation preparation; preparation.crash = CrashContext{};
        CHECK(ReportWorkflow::Begin(preparation, state));
        state = ReportWorkflow::Execute(preparation, state, {}, dependencies);
        CHECK(state.phase == Phase::Preview && !state.record);
    };
    const auto send = [&](Submission submission) {
        submission.preview = state.preview;
        CHECK(ReportWorkflow::Begin(submission, state));
        state = ReportWorkflow::Execute(submission, state, {}, dependencies);
    };
    response.request.ok = true; response.body = "{\"id\":\"" + std::string(32, 'e') + "\"}";
    // Small dump turned off: no dump. As the preview starts, on: the dump goes
    // and the record says so.
    prepare(); send({"", false, nullptr});
    CHECK(lastBody.find("name=\"minidump\"") == std::string::npos && state.record && !state.record->dump && !state.record->automatic);
    CHECK(state.record->status == Status::Received && state.record->id == std::string(32, 'e') && state.record->kind == "crash");
    prepare(); send({});
    CHECK(lastBody.find("name=\"minidump\"") != std::string::npos && state.record && state.record->dump);
    // Refusals and failures are recorded with a short reason only.
    prepare(); response = {}; response.request.statusCode = 429;
    send({}); CHECK(state.phase == Phase::Retry && state.record->status == Status::Refused && state.record->reason == "rate_limit");
    prepare(); response = {}; response.request.statusCode = 400; response.body = "{\"reason\":\"bad_meta\"}";
    send({}); CHECK(state.record->status == Status::Refused && state.record->reason == "bad_meta");
    prepare(); response.body = "{\"reason\":\"<b>x</b>\"}";
    send({}); CHECK(state.record->reason == "http_400");
    prepare(); response = {}; response.request.error = sf4e::HttpErrorKind::Timeout;
    send({}); CHECK(state.phase == Phase::Failed && state.record->status == Status::NotSent && state.record->reason == "timeout");
    prepare(); response = {}; response.cancelled = true;
    send({}); CHECK(state.phase == Phase::Cancelled && state.record->status == Status::NotSent && state.record->reason == "cancelled");
    auto history = dependencies.history->Load();
    CHECK(history.readable && history.sent.size() == 7 && history.sent.back().reason == "cancelled");
    // Nothing went to the network, so nothing is recorded.
    auto before = uploads;
    prepare();
    Submission invalid; invalid.preview = state.preview; invalid.comment = std::string(2001, 'x');
    CHECK(ReportWorkflow::Begin(invalid, state));
    state = ReportWorkflow::Execute(invalid, state, {}, dependencies);
    CHECK(state.phase == Phase::Failed && !state.record && uploads == before);
    // A send that cannot be counted does not go.
    publishes = false; prepare(); send({});
    CHECK(state.phase == Phase::Failed && !state.record && uploads == before && dependencies.history->Load().sent.size() == 7);
    publishes = true;
}

// A crash's report sent without a press: allowed by the saved setting or by
// Always send, counted before anything is read, with the small dump when the
// crash wrote one, sent once. Only when the dump keeps it out does it go once
// more, without the dump, as the same counted send.
inline void Automatic(const std::filesystem::path& root) {
    // The real collection over a logs folder and the crash's small dump.
    const auto logs = root / L"automatic-logs";
    std::filesystem::create_directories(logs);
    { std::ofstream file(logs / L"sf4e.log", std::ios::binary); file << "log\n"; }
    const std::string dump = "MDMP" + std::string(32, 'd');
    const auto writeDump = [&] { std::ofstream file(root / L"small.dmp", std::ios::binary | std::ios::trunc); file << dump; };
    writeDump();
    std::optional<bool> on = true; bool saves = true, identityOk = true;
    int uploads = 0, turnedOn = 0, collected = 0, progressed = 0; std::vector<std::string> bodies;
    std::filesystem::path dumpAsked = L"unset";
    sf4e::HttpResponse received; received.request.ok = true; received.body = "{\"id\":\"" + std::string(32, 'f') + "\"}";
    // Answers in order, then 202 for every later upload.
    std::deque<sf4e::HttpResponse> answers;
    WorkflowDependencies dependencies;
    const auto path = root / L"automatic.json";
    dependencies.history = std::make_shared<ReportHistory>(path);
    dependencies.reportsOn = [&] { return on; };
    dependencies.turnReportsOn = [&] { ++turnedOn; return saves; };
    dependencies.identity = [&](const std::function<bool()>&) { return identityOk ? Known() : Identity{}; };
    dependencies.collect = [&](const Preparation& preparation, const Account& account, const std::function<bool()>& cancelled) {
        ++collected; dumpAsked = preparation.crash ? preparation.crash->smallDump : L"none";
        return Collect(logs, Metadata{"crash", "1.2.3", "nightly", std::string(64, 'c'), "revision", "Windows test"},
            preparation.crash.value_or(CrashContext{}), cancelled, account);
    };
    dependencies.upload = [&](const Multipart& body, const std::function<bool()>&) {
        ++uploads; bodies.push_back(body.body);
        if (answers.empty()) return received;
        const auto answer = answers.front(); answers.pop_front(); return answer;
    };
    dependencies.progress = [&](const WorkflowState& shown) { ++progressed; CHECK(shown.automatic && shown.authorized && shown.phase == Phase::Submitting); };
    const auto hasDump = [](const std::string& body) { return body.find("name=\"minidump\"; filename=\"crash.dmp\"") != std::string::npos; };
    CrashContext context; context.exitCode = 0xC0000005u; context.smallDump = root / L"small.dmp";
    WorkflowState state;
    const auto run = [&](AutomaticCrash::Consent consent) {
        const Operation operation = AutomaticCrash{consent, context, sf4e::updates::UpdateChannel::Stable};
        CHECK(ReportWorkflow::Begin(operation, state));
        CHECK(state.automatic && !state.authorized && !state.offer && state.message.empty());
        state = ReportWorkflow::Execute(operation, state, {}, dependencies);
    };
    // Off, or settings that cannot be read: offered, nothing read or sent.
    for (const auto setting : {std::optional<bool>(false), std::optional<bool>()}) {
        on = setting; run(AutomaticCrash::Consent::Existing);
        CHECK(state.offer && !state.authorized && state.phase == Phase::Idle && uploads == 0 && collected == 0 && turnedOn == 0);
        CHECK(!std::filesystem::exists(path));
    }
    // On: counted, the crash's small dump read and sent with it, once.
    on = true; run(AutomaticCrash::Consent::Existing);
    CHECK(state.phase == Phase::Sent && !state.offer && state.authorized && uploads == 1 && progressed == 1);
    CHECK(dumpAsked == context.smallDump && hasDump(bodies.back()) && bodies.back().find(dump) != std::string::npos);
    CHECK(bodies.back().find("\"comment\"") == std::string::npos);
    CHECK(state.record && state.record->automatic && state.record->dump && state.record->status == Status::Received);
    auto history = dependencies.history->Load();
    CHECK(history.sent.size() == 1 && history.sent[0].status == Status::Received && history.sent[0].automatic && history.sent[0].dump);
    // Without the player's names nothing is sent, and the counted send is closed as not sent.
    identityOk = false; run(AutomaticCrash::Consent::Existing);
    CHECK(state.phase == Phase::Failed && uploads == 1 && state.record && state.record->status == Status::NotSent && state.record->reason == "not_prepared");
    identityOk = true;
    // The day's third, its dump gone from the PC: the report still goes,
    // without it. Then the limit: offered instead.
    std::filesystem::remove(context.smallDump);
    run(AutomaticCrash::Consent::Existing);
    CHECK(state.phase == Phase::Sent && uploads == 2 && !hasDump(bodies.back()) && state.record && !state.record->dump);
    run(AutomaticCrash::Consent::Existing);
    CHECK(state.offer && uploads == 2 && dependencies.history->Load().sent.size() == 3);
    // Always send saves the setting first; the limit still holds.
    run(AutomaticCrash::Consent::Confirmed);
    CHECK(turnedOn == 1 && state.offer && uploads == 2 && !state.message.empty());
    // A crash that wrote no small dump at all: the report goes without one.
    std::filesystem::remove(path);
    const auto withDump = context; context.smallDump.clear();
    run(AutomaticCrash::Consent::Confirmed);
    CHECK(turnedOn == 2 && state.phase == Phase::Sent && uploads == 3 && dumpAsked.empty() && !hasDump(bodies.back()));
    CHECK(state.record->automatic && !state.record->dump);
    context = withDump; writeDump();
    // Always send that cannot be saved sends nothing and offers the report.
    saves = false; run(AutomaticCrash::Consent::Confirmed);
    CHECK(turnedOn == 3 && state.offer && state.phase == Phase::Failed && uploads == 3 && dependencies.history->Load().sent.size() == 1);
    saves = true;
    // The intake timed out or refused the size with the dump: the same report
    // once more without it, under the one reservation. A rate limit or any
    // other refusal is not about the dump, so nothing goes again.
    const auto answer = [](int status, sf4e::HttpErrorKind error) {
        sf4e::HttpResponse refused; refused.request.statusCode = status; refused.request.error = error; return refused;
    };
    for (const auto& first : {answer(408, sf4e::HttpErrorKind::HttpStatus), answer(413, sf4e::HttpErrorKind::HttpStatus),
        answer(0, sf4e::HttpErrorKind::Timeout)}) {
        std::filesystem::remove(path); answers = {first};
        const auto before = uploads;
        run(AutomaticCrash::Consent::Existing);
        CHECK(state.phase == Phase::Sent && uploads == before + 2 && hasDump(bodies[bodies.size() - 2]) && !hasDump(bodies.back()));
        CHECK(state.record && state.record->status == Status::Received && !state.record->dump);
        history = dependencies.history->Load();
        CHECK(history.sent.size() == 1 && history.sent[0].status == Status::Received && !history.sent[0].dump);
    }
    for (const auto& first : {answer(429, sf4e::HttpErrorKind::HttpStatus), answer(400, sf4e::HttpErrorKind::HttpStatus),
        answer(0, sf4e::HttpErrorKind::ConnectFailed)}) {
        std::filesystem::remove(path); answers = {first};
        const auto before = uploads;
        run(AutomaticCrash::Consent::Existing);
        CHECK(state.phase != Phase::Sent && uploads == before + 1 && hasDump(bodies.back()) && state.record && state.record->dump);
        CHECK(dependencies.history->Load().sent.size() == 1);
    }
    // A dump the report cannot carry, here one past the limit: refused as the
    // report is built, so the report goes without it.
    std::filesystem::remove(path);
    const auto collectFile = dependencies.collect;
    dependencies.collect = [&](const Preparation& preparation, const Account& account, const std::function<bool()>& cancelled) {
        auto report = collectFile(preparation, account, cancelled);
        report.minidump = "MDMP" + std::string(DumpLimit - 3, 'x'); return report;
    };
    const auto before = uploads;
    run(AutomaticCrash::Consent::Existing);
    CHECK(state.phase == Phase::Sent && uploads == before + 1 && !hasDump(bodies.back()) && !state.record->dump);
    dependencies.collect = collectFile;
    std::filesystem::remove(path);
    // A record that cannot be read never gives the allowance back: neither a
    // damaged entry nor times past what the record can hold.
    const std::string overflow = R"({"time":18446744073709551615,"status":"received","kind":"crash","id":"","reason":"","attempt":"","dump":false,"automatic":true})";
    for (const std::string damage : {std::string("{\"schema\":1,\"reports\":[3]}"),
        "{\"schema\":1,\"reports\":[" + overflow + "," + overflow + "," + overflow + "]}"}) {
        { std::ofstream file(path, std::ios::binary | std::ios::trunc); file << damage; }
        const auto sentBefore = uploads, collectedBefore = collected;
        run(AutomaticCrash::Consent::Existing);
        CHECK(state.offer && !state.authorized && uploads == sentBefore && collected == collectedBefore);
    }
    // A preview keeps the offer; a send from it ends the offer.
    Preparation preview; preview.crash = context;
    CHECK(ReportWorkflow::Begin(preview, state) && state.offer && !state.automatic);
    state = ReportWorkflow::Execute(preview, state, {}, dependencies);
    CHECK(state.offer && state.phase == Phase::Preview && state.preview->minidump == dump);
    Submission submission{"", false, state.preview};
    CHECK(ReportWorkflow::Begin(submission, state) && !state.offer);
}

// The record itself: missing is empty, damaged is unreadable and refuses,
// a failed write changes nothing, an interrupted send stays counted, and
// writers in parallel never lose one another's sends.
inline void HistoryStore(const std::filesystem::path& root) {
    const auto now = Now();
    {
        ReportHistory missing(root / L"missing.json");
        const auto loaded = missing.Load();
        CHECK(loaded.readable && loaded.sent.empty());
        Record attempt; CHECK(missing.Reserve(attempt, true, now) == Reservation::Reserved);
        CHECK(attempt.status == Status::Pending && attempt.attempt.size() == 16 && attempt.automatic);
        // Interrupted: never completed, still counted.
        ReportHistory again(root / L"missing.json");
        const auto after = again.Load();
        CHECK(after.sent.size() == 1 && after.sent[0].status == Status::Pending);
        Record second, third;
        CHECK(again.Reserve(second, true, now) == Reservation::Reserved && again.Reserve(third, true, now) == Reservation::Reserved);
        Record fourth; CHECK(again.Reserve(fourth, true, now) == Reservation::LimitReached);
        // A send with a press is still counted past the limit.
        Record manual; CHECK(again.Reserve(manual, false, now) == Reservation::Reserved);
        // Completing keeps the reservation's time, so the day's count holds.
        Record done = attempt; done.status = Status::Received; done.id = std::string(32, 'a'); done.time = now - 3 * 24 * 60 * 60;
        History completed; CHECK(again.Complete(done, &completed));
        CHECK(completed.sent.size() == 4 && completed.sent[0].status == Status::Received && completed.sent[0].time == now);
        CHECK(!AutomaticAllowed(completed.sent, now));
    }
    for (const char* damage : {"", "not json", "{\"schema\":1,\"reports\":[3]}", "{\"schema\":2,\"reports\":[]}"}) {
        const auto path = root / L"damaged.json";
        { std::ofstream file(path, std::ios::binary | std::ios::trunc); file << damage; }
        ReportHistory damaged(path);
        CHECK(!damaged.Load().readable);
        Record attempt; CHECK(damaged.Reserve(attempt, true, now) == Reservation::Unreadable);
        CHECK(damaged.Reserve(attempt, false, now) == Reservation::Unreadable);
        std::ifstream file(path, std::ios::binary); std::string kept((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        CHECK(kept == damage);
    }
    {
        // Too large to be a record this code writes.
        const auto path = root / L"large.json";
        { std::ofstream file(path, std::ios::binary); file << std::string(SentFileLimit + 1, ' '); }
        CHECK(!ReportHistory(path).Load().readable);
        CHECK(!ReportHistory({}).Load().readable);
    }
    {
        // A write that fails changes nothing.
        const auto path = root / L"unwritable.json";
        ReportHistory writable(path);
        Record first; CHECK(writable.Reserve(first, true, now) == Reservation::Reserved);
        ReportHistory failing(path, [](const std::filesystem::path&, const std::string&) { return false; });
        Record attempt; CHECK(failing.Reserve(attempt, true, now) == Reservation::Failed);
        Record done = first; done.status = Status::NotSent; CHECK(!failing.Complete(done));
        const auto kept = writable.Load();
        CHECK(kept.sent.size() == 1 && kept.sent[0].status == Status::Pending);
    }
    {
        // Another process holding the record: the change waits, then fails.
        const auto path = root / L"locked.json";
        const HANDLE held = CreateFileW((path.wstring() + L".lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(held != INVALID_HANDLE_VALUE);
        ReportHistory waiting(path, {}, std::chrono::milliseconds(100));
        Record attempt; CHECK(waiting.Reserve(attempt, false, now) == Reservation::Failed);
        CloseHandle(held);
        CHECK(waiting.Reserve(attempt, false, now) == Reservation::Reserved);
    }
    {
        // Writers in parallel: automatic sends never pass the limit, and no
        // send is lost. Every send counts toward the limit, so the automatic
        // ones go first and the ones with a press after.
        const auto path = root / L"parallel.json";
        std::atomic<int> reserved{0}, limited{0}, failed{0};
        const auto writers = [&](bool automatic, int count) {
            std::vector<std::thread> threads;
            for (int i = 0; i < count; ++i) threads.emplace_back([&, automatic] {
                ReportHistory history(path, {}, std::chrono::milliseconds(10000));
                Record attempt;
                const auto result = history.Reserve(attempt, automatic, now);
                if (result == Reservation::Reserved) {
                    ++reserved;
                    Record done = attempt; done.status = Status::NotSent; done.reason = "no_connection";
                    if (!history.Complete(done)) ++failed;
                } else if (result == Reservation::LimitReached) ++limited;
                else ++failed;
            });
            for (auto& thread : threads) thread.join();
        };
        writers(true, 8);
        CHECK(failed == 0 && reserved == AutomaticPerDay && limited == 8 - AutomaticPerDay);
        writers(false, 8);
        CHECK(failed == 0 && reserved == AutomaticPerDay + 8 && limited == 8 - AutomaticPerDay);
        const auto history = ReportHistory(path).Load();
        CHECK(history.readable && history.sent.size() == AutomaticPerDay + 8u);
        CHECK(std::count_if(history.sent.begin(), history.sent.end(), [](const Record& r) { return r.automatic; }) == AutomaticPerDay);
        CHECK(std::none_of(history.sent.begin(), history.sent.end(), [](const Record& r) { return r.status == Status::Pending; }));
    }
}

// Send problem reports is saved under the key it took when reports began to
// carry the small dump. The setting saved before, under the old key, reads as
// off, so the next crash asks again; Always send writes only the new key.
inline void Consent(const std::filesystem::path& root) {
    const auto directory = root / L"consent";
    std::filesystem::create_directories(directory);
    {
        std::ofstream file(directory / L"settings.json", std::ios::binary | std::ios::trunc);
        file << R"({"schemaVersion":1,"profile":{},"netplay":{"sendProblemReports":true},"overlay":{},"legacyLauncher":{}})";
    }
    CHECK(ReportsOn(directory.wstring()) == false);
    CHECK(TurnReportsOn(directory.wstring()) && ReportsOn(directory.wstring()) == true);
    nlohmann::json saved; std::string error;
    CHECK(sf4e::netplay::SettingsStore(directory.wstring()).LoadLauncher(saved, error));
    CHECK(saved["sendProblemReportsWithDump"] == true && saved["sendProblemReports"] == true);
    // Settings that cannot be read are neither on nor off.
    CHECK(!ReportsOn(L"") && !TurnReportsOn(L""));
}

// The player's names come from the settings, waited for while another process
// holds them; when they stay unreadable, there is no identity at all.
inline void Identities(const std::filesystem::path& root) {
    const auto directory = root / L"settings";
    std::filesystem::create_directories(directory);
    std::string error;
    CHECK(sf4e::netplay::SettingsStore(directory.wstring()).SaveLauncher({{"displayName", "Tester"}}, error));
    const auto names = [](const Identity& identity, const char* name) {
        return std::find(identity.account.names.begin(), identity.account.names.end(), name) != identity.account.names.end();
    };
    auto identity = CollectIdentity(directory.wstring(), {}, std::chrono::milliseconds(500));
    CHECK(identity.ok && names(identity, "Tester") && identity.account.names.size() >= 3);
    const HANDLE held = CreateFileW((directory / L"settings.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    identity = CollectIdentity(directory.wstring(), {}, std::chrono::milliseconds(200));
    CHECK(!identity.ok);
    // Released within the wait: read on the next try.
    std::thread release([held] { Sleep(150); CloseHandle(held); });
    identity = CollectIdentity(directory.wstring(), {}, std::chrono::milliseconds(3000));
    release.join();
    CHECK(identity.ok && names(identity, "Tester"));
    // The default name names no one; a name that is not text is unreadable.
    CHECK(sf4e::netplay::SettingsStore(directory.wstring()).SaveLauncher({{"displayName", "Player"}}, error));
    identity = CollectIdentity(directory.wstring(), {}, std::chrono::milliseconds(500));
    CHECK(identity.ok && !names(identity, "Player"));
    CHECK(sf4e::netplay::SettingsStore(directory.wstring()).SaveLauncher({{"displayName", 5}}, error));
    CHECK(!CollectIdentity(directory.wstring(), {}, std::chrono::milliseconds(200)).ok);
    CHECK(!CollectIdentity(L"", {}, std::chrono::milliseconds(50)).ok);
}

inline void Run(const std::filesystem::path& root) {
    Previews(root); Records(root); Automatic(root); Consent(root); HistoryStore(root); Identities(root);
}
}
