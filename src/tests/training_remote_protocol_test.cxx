#include "../training/RemoteProtocol.hxx"
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
        // Saved combos: the list the phone shows, the commands that play one, the names it may send.
        const auto combos = CombosJson({{"1-1", "Ken combo", 3, 4, 60, 100}}, [](int id) { return id == 3 ? std::string("Ken") : std::string(); });
        Require(combos["combos"].size() == 1 && combos["combos"][0]["fighter"] == "Ken" && combos["combos"][0]["length"] == 60 && combos["combos"][0]["id"] == "1-1", "The combo list was wrong");
        SavedCombo saved; saved.x[0] = -1; saved.x[1] = 2; saved.frames = {{0, 0, 0, 0}, {1, 1, 0, 0}};
        const auto play = ComboPlayCommands(saved, 9);
        Require(play.size() == 2 && play[0].action == Action::Place && play[0].place[0] == -1 && play[0].place[1] == 2 && play[1].action == Action::ComboPlay &&
            play[1].frames.size() == 2 && play[0].generation == 9 && play[1].generation == 9, "A saved combo did not become Place then ComboPlay");
        std::string name;
        Require(ParseComboName("  My combo ", name, error) && name == "My combo", "A name was not trimmed");
        Require(!ParseComboName("   ", name, error) && !ParseComboName("a\nb", name, error) && !ParseComboName(std::string(ComboNameBytes + 1, 'a'), name, error) && !ParseComboName(5, name, error), "A bad name was taken");
        // Frame data: the fighters with their counts, one fighter's rows, nothing for an unknown one.
        const json frames = {{"cols", {"move"}}, {"fighters", {{"Ryu", {{"5LP"}, {"5MP"}}}, {"Ken", {{"5LP"}}}}}};
        Require(FramesIndex(frames)["fighters"].size() == 2 && FramesIndex(frames)["fighters"][1] == json({{"name", "Ryu"}, {"moves", 2}}), "The fighter list was wrong");
        Require(FramesOf(frames, "Ryu")["rows"].size() == 2 && FramesOf(frames, "Ryu")["cols"][0] == "move" && FramesOf(frames, "Nobody").is_null() && FramesIndex(json()).at("fighters").empty(), "A fighter's rows were wrong");
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
    return 0;
}
