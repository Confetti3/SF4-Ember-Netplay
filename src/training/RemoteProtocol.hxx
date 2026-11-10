#pragma once
#include "ComboFile.hxx"
#include "FighterData.hxx"
#include "RemoteOptions.hxx"
#include <cstdint>
#include <functional>
#include <string>

// The phone remote's wire format (RemoteServer.cxx): the options and live data
// as JSON, and a phone's order as the Command the overlay would have sent.
// Pure; no sockets and no game.
namespace sf4e { namespace training { namespace remote {
// Resolves a catalog id (and a slot's number) to the player's language.
using Say = std::function<std::string(const char* key, int arg)>;
// The options: the page draws its tiles from this, and their values come in the stream.
inline nlohmann::json OptionsJson(const Say& say) {
    static const char* const tabs[] = {"dummy", "reply", "hud", "tools"};
    nlohmann::json list = nlohmann::json::array();
    for (const auto& option : Options()) {
        nlohmann::json choices = nlohmann::json::array();
        for (const auto& c : option.choices) choices.push_back({c.value, c.key ? say(c.key, -1) : c.text});
        list.push_back({{"id", option.id}, {"label", say(option.label, option.arg)}, {"tab", tabs[static_cast<int>(option.tab)]},
            {"section", say(option.section, -1)}, {"switch", option.Switch()}, {"choices", choices}});
    }
    nlohmann::json presets = nlohmann::json::array();
    for (const char* preset : ReplyPresets) presets.push_back(preset);
    return {{"options", list}, {"replyPresets", presets}};
}
// Every option's value now, by id: the game's from the view, the rest as the overlay thread
// last published them (ReadLab).
inline nlohmann::json ValuesJson(const View& view, const nlohmann::json& lab) {
    nlohmann::json values = lab.is_object() ? lab : nlohmann::json::object();
    for (const auto& option : Options()) if (option.read) values[option.id] = option.read(view);
    values["available"] = view.available && view.ready;
    values["mode"] = static_cast<int>(view.mode);
    values["selected"] = view.selected;
    return values;
}
// The fields of `now` that differ from `before`, which then becomes `now`.
inline nlohmann::json Changed(nlohmann::json& before, const nlohmann::json& now) {
    nlohmann::json changed = nlohmann::json::object();
    for (auto it = now.begin(); it != now.end(); ++it) if (!before.contains(it.key()) || before[it.key()] != it.value()) changed[it.key()] = it.value();
    before = now;
    return changed;
}
// One phone's view of the frame meter, as little as changes. The first message
// and a restarted exchange are marked reset and carry the newest frames; after
// that the frames added since, and the last few again, because a meaty mark
// reaches back over cells already sent. A cell is [kind, flags, pad] per side:
// flags 1 meaty, 2 after a stop, 4 a hit or block landed here.
class MeterFeed {
public:
    static constexpr std::size_t Tail = 16, Newest = 240;
    nlohmann::json Next(const MeterView& meter) {
        const std::size_t size = meter.frames.size(), length = (std::max)(meter.length, size);
        const int stamp = size ? meter.frames.back().frame : 0;
        const bool reset = !started_ || length < sent_ || stamp < stamp_;
        if (!reset && length == sent_) return nullptr;
        const std::size_t base = length - size;
        std::size_t from = reset ? (length > Newest ? length - Newest : 0) : (sent_ > Tail ? sent_ - Tail : 0);
        from = (std::max)(from, base);
        nlohmann::json cells = nlohmann::json::array(), hits = nlohmann::json::array();
        for (std::size_t i = from; i < length; ++i) {
            const auto& frame = meter.frames[i - base];
            nlohmann::json cell = nlohmann::json::array();
            for (int side = 0; side < 2; ++side) {
                const auto& c = frame.cells[side];
                cell.push_back(static_cast<int>(c.kind));
                cell.push_back((c.meaty ? 1 : 0) | (c.afterStop ? 2 : 0) | (c.hitAdvantage != NoHitAdvantage ? 4 : 0));
                cell.push_back(frame.fighters[side].input);
                if (c.hitAdvantage != NoHitAdvantage) hits.push_back({i, side, c.hitAdvantage});
            }
            cells.push_back(std::move(cell));
        }
        started_ = true; sent_ = length; stamp_ = stamp;
        return {{"reset", reset}, {"length", length}, {"from", from}, {"cells", cells}, {"hits", hits}};
    }
private:
    bool started_ = false;
    std::size_t sent_ = 0;
    int stamp_ = 0;
};
// The Data panel's readings, one object per fighter, in the game's units: everything the panel in the game shows.
// "script": id, name, frame, total, the frame it can be cancelled from, its meter runs [from, to, kind]; "advantage" is
// the frames the meter measured for the fighter (null before one); "range" the rule and its distance; "charges" each a
// counter: a name (empty when the file's is a code), the directions held, the frames held and the frames each of its levels asks;
// "stock" Juri's Fuhajin stock, a bit to each kick, -1 for everyone else.
inline nlohmann::json DataJson(const View& view) {
    const DataView& data = view.data;
    if (!data.valid) return nullptr;
    const auto& advantage = ShownAdvantage(view.meter);
    nlohmann::json sides = nlohmann::json::array();
    int side = 0;
    for (const auto& f : data.fighters) {
        // A counter with several levels is one entry: its frames held and the frames each level asks.
        nlohmann::json charges = nlohmann::json::array();
        for (const auto& group : ChargeGroups(f)) {
            const Charge& first = f.charges[group.front()];
            int held = 0;
            nlohmann::json needs = nlohmann::json::array();
            for (const std::size_t i : group) { held = (std::max)(held, f.charges[i].held); needs.push_back(f.charges[i].required); }
            const std::string name = ChargeGroupName(f, group);
            charges.push_back({{"n", ReadableChargeName(name) ? name : std::string()}, {"b", (first.directions & ChargeBack) != 0}, {"d", (first.directions & ChargeDown) != 0},
                {"h", held}, {"r", needs}});
        }
        const bac::Script* named = f.Script();
        nlohmann::json runs = nlohmann::json::array();
        if (named && f.scripts) for (const auto& run : ScriptRuns(*f.scripts, f.script)) runs.push_back({run.from, run.to, static_cast<int>(run.kind)});
        nlohmann::json script = {{"id", f.script}, {"name", named ? named->name : std::string()}, {"frame", f.scriptFrame}, {"total", named ? named->total : 0},
            {"interruptible", named ? named->interruptible : 0}, {"runs", runs}};
        sides.push_back({{"valid", f.valid}, {"life", {f.life, f.lifeMax, f.recoverable}}, {"stun", {f.stun, f.stunMax}}, {"super", {f.super, f.superMax}},
            {"revenge", {f.revenge, f.revengeMax}}, {"x", f.x}, {"y", f.y}, {"stance", static_cast<int>(f.stance)}, {"status", f.status}, {"juggled", f.juggled},
            {"juggle", {f.juggle.start, f.juggle.add, f.juggle.limit}}, {"script", script}, {"range", {static_cast<int>(f.rangeRule), f.range}},
            {"advantage", advantage.valid ? nlohmann::json(advantage.frames[side]) : nlohmann::json()}, {"charges", charges}, {"stock", f.stock}});
        ++side;
    }
    return {{"distance", data.distance}, {"frame", data.frame}, {"sides", sides}};
}
// The live part of a stream message besides the meter: input history and Combo check's results.
inline nlohmann::json LiveJson(const View& view) {
    nlohmann::json history = nlohmann::json::array();
    for (const auto& side : view.history) {
        nlohmann::json runs = nlohmann::json::array();
        const std::size_t from = side.size() > 8 ? side.size() - 8 : 0;
        for (std::size_t i = from; i < side.size(); ++i) runs.push_back({side[i].frames, side[i].buttons});
        history.push_back(std::move(runs));
    }
    nlohmann::json runs = nlohmann::json::array();
    for (const auto& run : view.check.runs) runs.push_back({run.fighter, run.crouching, run.done, run.hits});
    return {{"history", history}, {"check", {{"best", view.check.best}, {"hits", view.check.hits}, {"frames", view.check.frames},
        {"running", view.check.running}, {"recording", view.check.recording}, {"runs", runs}}}};
}
// The saved combos for the phone's list; fighterName gives the name for a fighter id.
inline nlohmann::json CombosJson(const std::vector<ComboSummary>& list, const std::function<std::string(int)>& fighterName) {
    nlohmann::json combos = nlohmann::json::array();
    for (const auto& c : list) combos.push_back({{"id", c.id}, {"name", c.name}, {"fighter", fighterName(c.fighter)}, {"hits", c.hits}, {"length", c.length}, {"created", c.created}});
    return {{"combos", combos}};
}
// A saved combo played on Player 1: both fighters put where it began, then its frames (Training plays them once).
inline std::vector<Command> ComboPlayCommands(const SavedCombo& combo, std::uint64_t generation) {
    Command place; place.action = Action::Place; place.generation = generation; place.place[0] = combo.x[0]; place.place[1] = combo.x[1];
    Command play; play.action = Action::ComboPlay; play.generation = generation; play.frames = combo.frames;
    return {place, play};
}
// A name from the phone: text with no control characters, 1 to ComboNameBytes bytes after trimming.
inline bool ParseComboName(const nlohmann::json& value, std::string& name, std::string& error) {
    name.clear();
    if (!value.is_string()) { error = "no name"; return false; }
    name = value.get<std::string>();
    name.erase(0, name.find_first_not_of(" \t")); name.erase(name.find_last_not_of(" \t") + 1);
    if (name.empty() || !CleanText(name, ComboNameBytes)) { error = "bad name"; return false; }
    return true;
}
// Ember's frame data (assets/framedata/usf4.json: {"cols": [...], "fighters": {name: [row, ...]}}): the fighters
// with their move counts, or one fighter's rows. Null for a fighter it has none of.
inline nlohmann::json FramesIndex(const nlohmann::json& data) {
    nlohmann::json list = nlohmann::json::array();
    if (data.is_object() && data.contains("fighters") && data["fighters"].is_object())
        for (auto it = data["fighters"].begin(); it != data["fighters"].end(); ++it) list.push_back({{"name", it.key()}, {"moves", it.value().size()}});
    return {{"fighters", list}};
}
inline nlohmann::json FramesOf(const nlohmann::json& data, const std::string& fighter) {
    if (!data.is_object() || !data.contains("fighters") || !data["fighters"].contains(fighter)) return nullptr;
    return {{"fighter", fighter}, {"cols", data["cols"]}, {"rows", data["fighters"][fighter]}};
}
// One Server-Sent Events message.
inline std::string EventFrame(const nlohmann::json& message) { return "data: " + message.dump() + "\n\n"; }

// Where a phone's order goes: the game as a Command, the overlay thread by its
// Mailbox (an option or an action only it can do), or nowhere.
enum class Route { Refused, Game, Overlay };
struct Order { Command command; std::string id; int value = 0; std::string text; };
// {"cmd": name} or {"set": id, "value": n}; generation is the battle the page last saw.
inline Route ParseOrder(const nlohmann::json& body, const View& view, Order& order, std::string& error) {
    order = Order{};
    if (!body.is_object()) { error = "no order"; return Route::Refused; }
    const auto set = body.value("set", nlohmann::json());
    if (set.is_string()) {
        const auto value = body.value("value", nlohmann::json());
        const auto* option = FindOption(set.get<std::string>());
        if (!option) { error = "unknown option"; return Route::Refused; }
        if (!value.is_number_integer() || !option->Has(value.get<int>())) { error = "bad value"; return Route::Refused; }
        order.id = option->id; order.value = value.get<int>();
        if (option->lab) return Route::Overlay;
        order.command = option->command(order.value, view);
        return Route::Game;
    }
    const auto name = body.value("cmd", nlohmann::json());
    if (!name.is_string()) { error = "no cmd"; return Route::Refused; }
    const auto text = name.get<std::string>();
    // Combo check's start needs the fighters the player picked, which only the overlay holds.
    if (text == "comboStart") { order.id = text; return Route::Overlay; }
    // The reply's moves, typed or picked: the overlay checks them as the menu's field does.
    if (text == "replyMoves") {
        const auto moves = body.value("text", nlohmann::json());
        if (!moves.is_string() || moves.get<std::string>().size() > 256) { error = "bad moves"; return Route::Refused; }
        order.id = text; order.text = moves.get<std::string>(); return Route::Overlay;
    }
    static const std::pair<const char*, Action> plain[] = {{"record", Action::AdvanceRecording}, {"play", Action::Play}, {"stop", Action::Interrupt},
        {"clear", Action::Clear}, {"mirror", Action::Mirror}, {"save", Action::Save}, {"restore", Action::Restore},
        {"comboRecord", Action::ComboRecord}, {"comboStop", Action::ComboStop}};
    order.command.generation = view.generation;
    for (const auto& entry : plain) if (text == entry.first) { order.command.action = entry.second; return Route::Game; }
    if (text == "select") {
        const auto slot = body.value("slot", nlohmann::json());
        if (!slot.is_number_integer() || slot < 0 || slot >= SlotCount) { error = "bad slot"; return Route::Refused; }
        order.command.action = Action::Select; order.command.slot = slot.get<int>();
        return Route::Game;
    }
    error = "unknown cmd"; return Route::Refused;
}
} } }
