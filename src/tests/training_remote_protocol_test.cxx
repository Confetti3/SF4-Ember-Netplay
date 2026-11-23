#include "../training/RemoteProtocol.hxx"
#include "../training/RemoteRequest.hxx"
#include <algorithm>
#include <iterator>
#include <cstdio>
#include <set>
#include <stdexcept>
// The phone remote: its option table, how an order becomes a Command or an
// overlay change, and what the stream sends of the meter.
using namespace sf4e::training;
using namespace sf4e::training::remote;
using json = nlohmann::json;
void Require(bool pass, const char* why) { if (!pass) throw std::runtime_error(why); }
MeterView Exchange(std::size_t length, int firstStamp = 0) {
    MeterView meter; meter.length = length;
    for (std::size_t i = 0; i < length; ++i) { MeterFrame frame; frame.frame = firstStamp + static_cast<int>(i); frame.cells[0].kind = MeterKind::Startup; meter.frames.push_back(frame); }
    return meter;
}
int main() {
    try {
        // Every option: a unique id, something to choose, and a way to read and set it.
        std::set<std::string> ids;
        for (const auto& option : Options()) {
            Require(ids.insert(option.id).second, "An option id was used twice");
            Require(!option.choices.empty() && option.label && option.section, "An option has nothing to choose");
            Require(static_cast<bool>(option.lab) != static_cast<bool>(option.command) && (option.lab || option.read), "An option is not Lab or Game");
            for (const auto& c : option.choices) Require(c.key || !c.text.empty(), "A choice has no name");
        }
        // The Lab settings take each choice and give it back, and only a choice.
        Settings settings;
        for (const auto& option : Options()) if (option.lab) for (const auto& c : option.choices) {
            Require(WriteLab(settings, option.id, c.value) && option.lab(settings, nullptr) == c.value, "A Lab option did not round-trip");
        }
        Require(!WriteLab(settings, "shown", 7) && !WriteLab(settings, "nothing", 0), "A value no choice names was taken");
        WriteLab(settings, "shown", 120); WriteLab(settings, "style", 2); WriteLab(settings, "replyChance", 50); WriteLab(settings, "replyTiming", -3); WriteLab(settings, "data_move", 0);
        Require(settings.meter.shown == 120 && settings.meter.style == MeterStyle::Classic && settings.plan.chance == 50 && settings.plan.timing == -3 && !settings.meter.dataMove, "Settings were not written to their fields");
        Require(ReadLab(settings)["shown"] == 120, "ReadLab missed a value");
        // Game options become the Command the menu would send, setting only their own field.
        View view; view.generation = 7; view.available = view.ready = true; view.dummy.guard = 3;
        remote::Order order; std::string error;
        Require(ParseOrder({{"set", "guard"}, {"value", 2}}, view, order, error) == Route::Game && order.command.action == Action::DummyState &&
            order.command.dummy.guard == 2 && order.command.dummy.action == -1 && order.command.generation == 7, "A dummy order was wrong");
        Require(ParseOrder({{"set", "super"}, {"value", 7}}, view, order, error) == Route::Game && order.command.dummy.super == 7, "The gauge was wrong");
        Require(ParseOrder({{"set", "slot3"}, {"value", 1}}, view, order, error) == Route::Game && order.command.action == Action::SlotOn && order.command.slot == 3 && order.command.on, "A slot order was wrong");
        Require(ParseOrder({{"set", "order"}, {"value", 1}}, view, order, error) == Route::Game && order.command.order == sf4e::training::Order::InOrder, "The play order was wrong");
        Require(ParseOrder({{"set", "style"}, {"value", 1}}, view, order, error) == Route::Overlay && order.id == "style" && order.value == 1, "A Lab order was not left to the overlay");
        Require(ParseOrder({{"cmd", "replyMoves"}, {"text", "623P"}}, view, order, error) == Route::Overlay && order.id == "replyMoves" && order.text == "623P", "The reply's moves were not left to the overlay");
        Require(ParseOrder({{"cmd", "replyMoves"}}, view, order, error) == Route::Refused, "Reply moves without text were accepted");
        Require(OptionsJson([](const char*, int) { return std::string("x"); })["replyPresets"].size() == std::size(UniversalMoves), "The reply presets were not offered");
        Require(ParseOrder({{"cmd", "comboStart"}}, view, order, error) == Route::Overlay && order.id == "comboStart", "Combo check's start was not left to the overlay");
        Require(ParseOrder({{"cmd", "play"}}, view, order, error) == Route::Game && order.command.action == Action::Play, "Play");
        Require(ParseOrder({{"cmd", "stop"}}, view, order, error) == Route::Game && order.command.action == Action::Interrupt, "Stop is the Interrupt");
        Require(ParseOrder({{"cmd", "select"}, {"slot", 1}}, view, order, error) == Route::Game && order.command.slot == 1, "Select");
        Require(ParseOrder({{"cmd", "select"}, {"slot", SlotCount}}, view, order, error) == Route::Refused && error == "bad slot", "A slot out of range");
        Require(ParseOrder({{"set", "guard"}, {"value", 4}}, view, order, error) == Route::Refused && error == "bad value", "A guard past the menu");
        Require(ParseOrder({{"set", "super"}, {"value", 3}}, view, order, error) == Route::Refused, "A gauge value the menu never stores");
        Require(ParseOrder({{"set", "format"}, {"value", 0}}, view, order, error) == Route::Refused && error == "unknown option", "An unknown option");
        Require(ParseOrder({{"cmd", "format"}}, view, order, error) == Route::Refused && error == "unknown cmd", "An unknown order");
        Require(ParseOrder(json("record"), view, order, error) == Route::Refused, "Not an object");
        Require(ParseOrder({{"cmd", "comboRename"}, {"id", "1-1"}}, view, order, error) == Route::Store && order.id == "comboRename", "A saved combo's order was not left to the store");
        Require(ParseOrder({{"cmd", "comboPlay"}}, view, order, error) == Route::Store && ParseOrder({{"cmd", "comboRecord"}}, view, order, error) == Route::Game, "Combo check's orders were mixed up with the store's");
        Require(ParseOrder({{"cmd", "comboAnything"}}, view, order, error) == Route::Refused && error == "unknown cmd", "A combo order nobody knows was taken");
        // Fields of the wrong type are refused, never thrown on, and a number too big for an int is not cut down to a valid one.
        Require(ParseOrder({{"set", "guard"}, {"value", "2"}}, view, order, error) == Route::Refused && error == "bad value", "A value that is text was taken");
        Require(ParseOrder({{"set", "guard"}, {"value", 4294967298LL}}, view, order, error) == Route::Refused && error == "bad value", "A value past an int was cut down");
        Require(ParseOrder({{"cmd", 5}}, view, order, error) == Route::Refused && error == "no cmd", "An order name that is a number was taken");
        Require(ParseOrder({{"cmd", "select"}, {"slot", "1"}}, view, order, error) == Route::Refused && ParseOrder({{"cmd", "replyMoves"}, {"text", 623}}, view, order, error) == Route::Refused,
            "Fields of the wrong type were taken");
        // Text that is not UTF-8 (a modded fighter's script names) is written with U+FFFD, never thrown on.
        {
            const json bad = {{"data", {{"sides", {{{"script", {{"name", std::string("Ryu\xff\xfe")}}}}}}}}};
            Require(EventFrame(bad).find("Ryu\xEF\xBF\xBD") != std::string::npos && JsonReply(bad).body.find("Ryu\xEF\xBF\xBD") != std::string::npos, "Bytes that are not UTF-8 must be written as U+FFFD");
        }
        // The page's data: the options, and every value with the game's from the view.
        const auto page = OptionsJson([](const char* key, int arg) { return std::string(key) + (arg < 0 ? "" : std::to_string(arg)); });
        Require(page["options"].size() == Options().size() && page["options"][0]["choices"][0].size() == 2, "The options JSON was wrong");
        const auto values = ValuesJson(view, json{{"shown", 90}});
        Require(values["guard"] == 3 && values["shown"] == 90 && values["available"] == true, "The values JSON was wrong");
        json before = json::object();
        Require(Changed(before, values).size() == values.size() && Changed(before, values).empty(), "Changed repeated what was sent");
        // The meter: everything at first, then the new frames and the tail, nothing when nothing is new, all again after a restart.
        MeterFeed feed;
        auto first = feed.Next(Exchange(30));
        Require(first["reset"] == true && first["from"] == 0 && first["cells"].size() == 30 && first["cells"][0].size() == 6 && first["cells"][0][0] == static_cast<int>(MeterKind::Startup), "The first message was not the whole exchange");
        Require(feed.Next(Exchange(30)).is_null(), "A message went out with nothing new");
        auto more = feed.Next(Exchange(34));
        Require(more["reset"] == false && more["from"] == 14 && more["cells"].size() == 20 && more["length"] == 34, "The new frames were not sent with the tail");
        Require(feed.Next(Exchange(5))["reset"] == true, "A restarted exchange was not marked");
        auto long_ = MeterFeed().Next(Exchange(500));
        Require(long_["from"] == 500 - MeterFeed::Newest && long_["cells"].size() == MeterFeed::Newest, "A long exchange was not cut to the newest frames");
        auto hit = Exchange(3); hit.frames[1].cells[1].hitAdvantage = 4;
        const auto marked = MeterFeed().Next(hit);
        Require(marked["hits"].size() == 1 && marked["hits"][0] == json::array({1, 1, 4}) && marked["cells"][1][4] == 4, "A hit's advantage was not sent");
        Require(EventFrame({{"a", 1}}) == "data: {\"a\":1}\n\n", "An event was not framed as Server-Sent Events");
        Require(DataJson(View{}).is_null(), "Data was sent with none read");
        {
            // El Fuerte's twelve charges go out as six counters, the three levels of a button's in one; Juri's as her stock.
            View view; view.data.valid = true;
            auto& ric = view.data.fighters[0]; ric.valid = true;
            const std::vector<std::string> names{"QUESA_L_tameL", "QUESA_L_tameM", "QUESA_L_tameH", "QUESA_M_tameL", "QUESA_M_tameM", "QUESA_M_tameH",
                "QUESA_H_tameL", "QUESA_H_tameM", "QUESA_H_tameH", "QUESA_EX_tameLM", "QUESA_EX_tameLH", "QUESA_EX_tameMH"};
            ric.chargeNames = std::make_shared<const std::vector<std::string>>(names);
            const int held[3] = {130, 37, 185}, need[3] = {60, 120, 180};
            for (std::size_t i = 0; i < names.size(); ++i) ric.charges[i] = {i, ChargeBack, i < 9 ? need[i / 3] : 210, i < 9 ? (std::min)(held[i % 3], need[i / 3]) : 0};
            ric.chargeCount = names.size();
            auto& juri = view.data.fighters[1]; juri.valid = true; juri.stock = 5; juri.oil = 240;
            const auto sent = DataJson(view)["sides"];
            Require(sent[0]["charges"].size() == 6 && sent[0]["charges"][0]["n"] == "QUESA_tameL" && sent[0]["charges"][0]["h"] == 130 && sent[0]["charges"][0]["r"] == json::array({60, 120, 180}) &&
                sent[0]["stock"] == -1 && sent[1]["stock"] == 5, "Charges were not sent as one counter with its levels");
            Require(sent[0]["oil"][0] == -1 && sent[1]["oil"] == nlohmann::json({240, sf4e::training::MaxOil}), "Hakan's oil was not sent with its most");
        }
        // What the phone queued reaches the overlay thread, and no more than fits.
        Mailbox box;
        for (std::size_t i = 0; i < Mailbox::Capacity; ++i) Require(box.Push("style", 1), "The mailbox refused early");
        Require(!box.Push("style", 2) && box.Take().size() == Mailbox::Capacity && box.Take().empty(), "The mailbox kept more than it should");
        // The dummy kinds the menu uses are the table's: its numbering stays what the game's menu has.
        Require(DummyKindOf("super").choices.size() == 4 && DummyKindOf("super").choices[1].value == 5 && DummyKindOf("action").choices.size() == 4, "A dummy kind changed");
        // Quick stand: the game's own 0 is a normal rise and 1 the quick one, so each label names the value it sets.
        const auto& quickStand = DummyKindOf("quickStand").choices;
        Require(quickStand.size() == 4 && quickStand[0].value == 0 && std::string(quickStand[0].key) == "training.dummy.quick_normal" &&
            quickStand[1].value == 1 && std::string(quickStand[1].key) == "training.dummy.quick_quick" &&
            std::string(quickStand[2].key) == "training.dummy.quick_delayed" && std::string(quickStand[3].key) == "training.dummy.quick_random", "Quick stand named the wrong game value");
        // Saved combos: the list the phone shows, the commands that play one, the names it may send.
        const auto combos = CombosJson({{"1-1", "Ken combo", 3, 4, 60, 100}}, [](int id) { return id == 3 ? std::string("Ken") : std::string(); });
        Require(combos["combos"].size() == 1 && combos["combos"][0]["fighter"] == "Ken" && combos["combos"][0]["length"] == 60 && combos["combos"][0]["id"] == "1-1", "The combo list was wrong");
        SavedCombo saved; saved.x[0] = -1; saved.x[1] = 2; saved.frames = {{0, 0, 0, 0}, {1, 1, 0, 0}};
        const auto play = ComboPlayCommands(saved, 9);
        Require(play.size() == 2 && play[0].action == Action::Place && play[0].place[0] == -1 && play[0].place[1] == 2 && play[1].action == Action::ComboPlay &&
            play[1].frames.size() == 2 && play[0].generation == 9 && play[1].generation == 9, "A saved combo did not become Place then ComboPlay");
        std::string name;
        Require(ParseComboName("  My combo ", name, error) && name == "My combo", "A name was not trimmed");
        Require(!ParseComboName("   ", name, error) && !ParseComboName("a\nb", name, error) && !ParseComboName(std::string(ComboNameBytes + 1, 'a'), name, error) && !ParseComboName(std::nullopt, name, error), "A bad name was taken");
        // Frame data: the fighters with their counts, one fighter's rows, nothing for an unknown one.
        const json frames = {{"cols", {"move"}}, {"fighters", {{"Ryu", {{"5LP"}, {"5MP"}}}, {"Ken", {{"5LP"}}}}}};
        Require(FramesIndex(frames)["fighters"].size() == 2 && FramesIndex(frames)["fighters"][1] == json({{"name", "Ryu"}, {"moves", 2}}), "The fighter list was wrong");
        Require(FramesOf(frames, "Ryu")["rows"].size() == 2 && FramesOf(frames, "Ryu")["cols"][0] == "move" && FramesOf(frames, "Nobody").is_null() && FramesIndex(json()).at("fighters").empty(), "A fighter's rows were wrong");
        // The route table: every request the pages make has its route, its body limit and where its PIN may be.
        {
            const struct { Method method; const char* path; } asked[] = {{Method::Get, "/"}, {Method::Get, "/api/options"}, {Method::Get, "/api/state"}, {Method::Get, "/api/stream"},
                {Method::Post, "/api/cmd"}, {Method::Get, "/api/camera"}, {Method::Post, "/api/camera"}, {Method::Get, "/api/combos"}, {Method::Post, "/api/combos"},
                {Method::Get, "/api/combos/1-1"}, {Method::Get, "/api/frames"}, {Method::Get, "/api/hud/themes"}, {Method::Get, "/api/hud/theme"}, {Method::Get, "/api/hud/scene"},
                {Method::Get, "/api/hud/sheet"}, {Method::Post, "/api/hud/image"}, {Method::Post, "/api/hud/image-remove"}, {Method::Post, "/api/hud/restart"},
                {Method::Post, "/api/hud/save-as"}, {Method::Post, "/api/hud/layout"}, {Method::Post, "/api/hud/apply"}};
            for (const auto& a : asked) Require(ResourceAt(a.method, a.path) != nullptr, "A request the page makes has no route");
            Require(!ResourceAt(Method::Get, "/api/cmd") && !ResourceAt(Method::Post, "/") && !ResourceAt(Method::Get, "/api/combos/") && !ResourceAt(Method::Post, "/api/hud/imagex") && !ResourceAt(Method::Get, "/api/nothing"),
                "A route was found that is not in the table");
            Require(ResourceAt(Method::Post, "/api/hud/image")->body == HudImageBytes && ResourceAt(Method::Post, "/api/hud/layout")->body == HudBytes &&
                ResourceAt(Method::Post, "/api/combos")->body == ComboFileBytes && ResourceAt(Method::Post, "/api/cmd")->body == CommandBytes && ResourceAt(Method::Get, "/api/state")->body == 0,
                "A route's body limit was wrong");
            Require(ResourceAt(Method::Get, "/")->pin == PinFrom::None && ResourceAt(Method::Get, "/api/stream")->pin == PinFrom::HeaderOrAddress &&
                ResourceAt(Method::Get, "/api/combos/1-1")->pin == PinFrom::HeaderOrAddress && ResourceAt(Method::Get, "/api/state")->pin == PinFrom::Header, "A route takes its PIN from the wrong place");
            Require(std::get<hud::Order>(ResourceAt(Method::Post, "/api/hud/apply")->handler) == hud::Order::Apply && std::get<Endpoint>(ResourceAt(Method::Get, "/api/combos/7")->handler) == Endpoint::ComboFile,
                "A route has the wrong handler");
            Require(ResourceAt(Method::Post, "/api/camera")->needs == Needs::Camera && ResourceAt(Method::Get, "/api/camera")->needs == Needs::Nothing, "The camera's orders must wait for the camera");
            Require(ResourceAt(Method::Post, "/api/hud/apply")->needs == Needs::NoMatch && ResourceAt(Method::Post, "/api/hud/restart")->needs == Needs::NoMatch &&
                ResourceAt(Method::Post, "/api/hud/layout")->needs == Needs::Nothing, "Applying a HUD and restarting the game must wait for no match");
        }
        // A request's head: the method, the path and query, the headers in any case, and a length that is one plain number.
        {
            Head head;
            Require(ParseHead("GET /api/frames?fighter=Ryu HTTP/1.1\r\nHost: x\r\nx-pin:  123456 \r\nUSER-AGENT: Phone", head) && head.method == Method::Get && head.path == "/api/frames" &&
                head.query == "fighter=Ryu" && head.pin == "123456" && head.userAgent == "Phone" && head.length == 0, "A head was read wrong");
            Require(ParseHead("POST /api/cmd HTTP/1.1\r\nContent-Length: 42", head) && head.method == Method::Post && head.length == 42, "A body's length was read wrong");
            Require(!ParseHead("PUT /api/cmd HTTP/1.1", head) && !ParseHead("GET /api/cmd", head) && !ParseHead("GET  HTTP/1.1", head), "A request line the remote does not take was read");
            Require(!ParseHead("POST /api/cmd HTTP/1.1\r\nContent-Length: 12a", head) && !ParseHead("POST /api/cmd HTTP/1.1\r\nContent-Length: -1", head) &&
                !ParseHead("POST /api/cmd HTTP/1.1\r\nContent-Length: 9999999999", head) && !ParseHead("POST /api/cmd HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2", head),
                "A length that is not one plain number was taken");
            Require(UrlDecode("a%20b+c%zz%4") == "a b c%zz%4" && QueryParam("x=1&pin=12%2034", "pin") == "12 34" && QueryParam("xpin=1", "pin").empty() && QueryParam("", "pin").empty(), "An address was decoded wrong");
        }
        // Decided on the head alone, in order: the route, the PIN, what the route needs, then the body's length.
        {
            PinGate pins; pins.Reset("123456");
            const auto now = Clock::now();
            bool cameraInUse = false, match = true;
            const auto holds = [&](Needs needs) { return needs == Needs::Camera ? cameraInUse : needs == Needs::NoMatch ? !match : true; };
            const auto admit = [&](const std::string& text, unsigned long peer = 1) { Head head; Require(ParseHead(text, head), "A test head was not read"); return Admit(head, pins, peer, now, holds); };
            Require(admit("GET /nothing HTTP/1.1").refused->status == 404, "An address the remote does not answer");
            Require(admit("GET / HTTP/1.1").resource != nullptr, "The page itself needs no PIN");
            Require(admit("POST /api/hud/image HTTP/1.1\r\nX-Pin: 000000\r\nContent-Length: 6000000", 2).refused->status == 403, "A wrong PIN must be refused before the length is looked at");
            const auto big = admit("POST /api/hud/image HTTP/1.1\r\nX-Pin: 123456\r\nContent-Length: 7000000");
            Require(big.refused && big.refused->status == 400 && big.refused->body == "too big", "A body over the route's limit must be refused");
            Require(admit("POST /api/hud/image HTTP/1.1\r\nX-Pin: 123456\r\nContent-Length: 6000000").resource != nullptr, "A picture within its limit must be taken");
            Require(admit("POST /api/cmd HTTP/1.1\r\nX-Pin: 123456\r\nContent-Length: 2000").refused->status == 400, "An order is small");
            Require(admit("GET /api/stream?pin=123456 HTTP/1.1").resource != nullptr && admit("GET /api/combos/1-1?pin=123456 HTTP/1.1").resource != nullptr, "A stream and a download take the PIN in the address");
            Require(admit("GET /api/state?pin=123456 HTTP/1.1", 3).refused->status == 403, "Other routes take the PIN only in the header");
            Require(admit("POST /api/camera HTTP/1.1\r\nX-Pin: 000000", 4).refused->status == 403, "The PIN comes before what the route needs");
            const auto notNow = admit("POST /api/camera HTTP/1.1\r\nX-Pin: 123456\r\nContent-Length: 10");
            Require(notNow.refused && notNow.refused->status == 409 && json::parse(notNow.refused->body)["ok"] == false, "Camera orders out of Training and replays must be refused, as JSON the page reads");
            cameraInUse = true;
            Require(admit("POST /api/camera HTTP/1.1\r\nX-Pin: 123456\r\nContent-Length: 10").resource != nullptr, "Camera orders in use must be taken");
            const auto restart = admit("POST /api/hud/restart HTTP/1.1\r\nX-Pin: 123456\r\nContent-Length: 2");
            Require(restart.refused && restart.refused->status == 409 && json::parse(restart.refused->body)["error"] == "not during a match", "A restart during a match must be refused");
            match = false;
            Require(admit("POST /api/hud/apply HTTP/1.1\r\nX-Pin: 123456\r\nContent-Length: 2").resource != nullptr, "Applying a HUD with no match must be taken");
        }
        // Wrong PINs: five lock one address out for 30 s; the addresses kept are bounded; past the budget only signed-in addresses get in.
        {
            using V = PinGate::Verdict;
            const auto t0 = Clock::now();
            PinGate pins; pins.Reset("123456");
            for (int i = 0; i < PinGate::FailuresAllowed; ++i) Require(pins.Check("1", 10, t0) == V::Wrong, "A wrong PIN must be wrong");
            Require(pins.Check("123456", 10, t0) == V::Locked && pins.Check("123456", 11, t0) == V::Right, "Five wrong tries must lock that address only");
            Require(pins.Check("123456", 10, t0 + PinGate::LockFor) == V::Right, "The lockout must end");
            pins.Reset("123456");
            for (unsigned long peer = 100; peer < 100 + PinGate::Budget; ++peer) Require(pins.Check("1", peer, t0) == V::Wrong, "A first wrong try");
            Require(pins.Check("123456", 500, t0) == V::Locked, "Past the budget a new address must wait");
            pins.Reset("123456"); pins.Check("123456", 99, t0);
            for (unsigned long peer = 100; peer < 100 + PinGate::Budget; ++peer) pins.Check("1", peer, t0);
            Require(pins.Check("123456", 99, t0) == V::Right, "An address that signed in must keep working past the budget");
            Require(pins.Check("123456", 500, t0 + PinGate::BudgetWindow) == V::Right, "The budget must come back after its minute");
            pins.Reset("123456");
            for (unsigned long peer = 0; peer < PinGate::MaxPeers; ++peer) Require(pins.Check("123456", 1000 + peer, t0) == V::Right, "Signing in");
            Require(pins.Check("123456", 5000, t0) == V::Locked, "More addresses than are kept must be refused");
            pins.Reset("");
            Require(pins.Check("", 1, t0) == V::Wrong, "A stopped remote's empty PIN must match nothing");
        }
        // What requests hold: the bodies' budget, given back as each request goes, and a wait cut into slices.
        {
            BodyBudget bodies(BodiesHeld);
            {
                const BodyBudget::Lease picture(bodies, HudImageBytes);
                Require(static_cast<bool>(picture) && !BodyBudget::Lease(bodies, HudImageBytes) && static_cast<bool>(BodyBudget::Lease(bodies, HudBytes)), "The budget must hold one picture and small bodies beside it");
            }
            Require(bodies.Held() == 0, "A request's share must come back when it goes");
            const auto t = Clock::now();
            Require(NextWait(t, t).count() == 0 && NextWait(t + std::chrono::seconds(1), t).count() == 0 && NextWait(t, t + RequestTime) == WaitSlice &&
                NextWait(t, t + std::chrono::milliseconds(50)).count() == 50, "A wait must be cut to the time left, in slices");
        }
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
    return 0;
}
