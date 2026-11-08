#pragma once
#include "ComboCheck.hxx"
#include "PracticeSettings.hxx"
#include "TrainingSession.hxx"
#include <algorithm>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// Every Training option the phone can change, in one table: the dummy, the
// recording slots, the reply, the HUD and Combo check. The phone page draws
// its tiles from this table, so a new option appears there without page work.
// Pure: no sockets and no game. Options kept in the overlay's saved settings
// (Lab) are changed on the overlay thread; the game's own (Game) go as Commands.
namespace sf4e { namespace training { namespace remote {
enum class Tab { Dummy, Reply, Hud, Tools };
// The overlay's saved settings an option may change (TrainingPanel.cxx: Lab).
struct Settings { MeterOptions meter; DummyPlan plan; combo_check::Settings check; };
// value: what is stored and sent. key: the label's catalog id; text is shown when there is none.
struct Choice { int value; const char* key; std::string text; };
struct Option {
    std::string id;
    const char* label; int arg = -1;      // arg: the label's {0}, a slot's number
    Tab tab; const char* section;         // section: a catalog id; the page shows one section at a time
    std::vector<Choice> choices;          // two choices Off, On make a switch
    // Lab: reads the value, or sets it first when given one.
    std::function<int(Settings&, const int*)> lab;
    // Game: the value now, and the Command that sets it.
    std::function<int(const View&)> read;
    std::function<Command(int, const View&)> command;
    bool Switch() const { return choices.size() == 2 && choices[0].key && std::string(choices[0].key) == "common.off"; }
    bool Has(int value) const { return std::any_of(choices.begin(), choices.end(), [&](const Choice& c) { return c.value == value; }); }
};
inline std::vector<Choice> OffOn() { return {{0, "common.off", {}}, {1, "common.on", {}}}; }
inline std::vector<Choice> Numbers(std::initializer_list<int> values, const char* unit) {
    std::vector<Choice> choices;
    for (const int v : values) choices.push_back({v, nullptr, std::to_string(v) + unit});
    return choices;
}
// The dummy's settings as the game's Training menu numbers them (DummyState).
struct DummyKind { const char* id; const char* label; int DummyState::* field; std::vector<Choice> choices; };
inline const std::vector<DummyKind>& DummyKinds() {
    static const std::vector<Choice> gauge{{0, "training.dummy.gauge_normal", {}}, {5, "training.dummy.gauge_max", {}},
        {7, "training.dummy.gauge_infinite", {}}, {8, "training.dummy.gauge_refill", {}}};
    static const std::vector<DummyKind> kinds{
        {"action", "training.dummy.action", &DummyState::action, {{0, "training.dummy.stand", {}}, {1, "training.dummy.crouch", {}}, {2, "training.dummy.jump", {}}, {3, "training.dummy.cpu", {}}}},
        {"guard", "training.dummy.guard", &DummyState::guard, {{0, "training.dummy.guard_none", {}}, {1, "training.dummy.guard_first", {}}, {2, "training.dummy.guard_all", {}}, {3, "training.dummy.guard_random", {}}}},
        {"counterHit", "training.dummy.counter", &DummyState::counterHit, {{0, "training.dummy.counter_off", {}}, {1, "training.dummy.counter_on", {}}, {2, "training.dummy.counter_random", {}}}},
        {"quickStand", "training.dummy.quick", &DummyState::quickStand, {{0, "training.dummy.quick_quick", {}}, {1, "training.dummy.quick_normal", {}}, {2, "training.dummy.quick_delayed", {}}, {3, "training.dummy.quick_random", {}}}},
        {"stun", "training.dummy.stun", &DummyState::stun, {{0, "training.dummy.stun_normal", {}}, {1, "training.dummy.stun_constant", {}}, {2, "training.dummy.stun_none", {}}}},
        {"super", "training.dummy.super", &DummyState::super, gauge},
        {"revenge", "training.dummy.revenge", &DummyState::revenge, gauge}};
    return kinds;
}
inline const DummyKind& DummyKindOf(const char* id) {
    const auto& kinds = DummyKinds();
    return *std::find_if(kinds.begin(), kinds.end(), [&](const DummyKind& k) { return std::string(k.id) == id; });
}
namespace detail {
// A member of Settings, reached through another, as one accessor.
template <class Outer, class Inner>
std::function<int(Settings&, const int*)> Member(Outer Settings::* outer, Inner Outer::* inner) {
    return [=](Settings& s, const int* set) {
        auto& field = s.*outer.*inner;
        if (set) field = static_cast<std::decay_t<decltype(field)>>(*set);
        return static_cast<int>(field);
    };
}
inline Option Lab(std::string id, const char* label, Tab tab, const char* section, std::vector<Choice> choices, std::function<int(Settings&, const int*)> access) {
    Option option; option.id = std::move(id); option.label = label; option.tab = tab; option.section = section;
    option.choices = std::move(choices); option.lab = std::move(access);
    return option;
}
inline std::vector<Choice> Sizes() { return {{0, "size.small", {}}, {1, "size.standard", {}}, {2, "size.large", {}}}; }
}
inline const std::vector<Option>& Options() {
    static const std::vector<Option> options = [] {
        using detail::Lab; using detail::Member;
        std::vector<Option> all;
        const auto meter = [&](const char* id, const char* label, const char* section, std::vector<Choice> choices, auto field) {
            all.push_back(Lab(id, label, Tab::Hud, section, std::move(choices), Member(&Settings::meter, field)));
        };
        const char* const dummySection = "training.dummy"; const char* const slotsSection = "training.dummy_recording";
        for (const auto& kind : DummyKinds()) {
            Option option; option.id = kind.id; option.label = kind.label; option.tab = Tab::Dummy; option.section = dummySection; option.choices = kind.choices;
            const auto field = kind.field;
            option.read = [field](const View& v) { return v.dummy.*field; };
            option.command = [field](int value, const View& v) { Command c; c.action = Action::DummyState; c.generation = v.generation; c.dummy.*field = value; return c; };
            all.push_back(std::move(option));
        }
        const auto game = [&](const char* id, const char* label, int arg, std::vector<Choice> choices, std::function<int(const View&)> read, std::function<Command(int, const View&)> command) {
            Option option; option.id = id; option.label = label; option.arg = arg; option.tab = Tab::Dummy; option.section = slotsSection;
            option.choices = std::move(choices); option.read = std::move(read); option.command = std::move(command);
            all.push_back(std::move(option));
        };
        game("loop", "training.loop", -1, OffOn(), [](const View& v) { return static_cast<int>(v.loop); },
            [](int value, const View& v) { Command c; c.action = Action::Loop; c.generation = v.generation; c.loop = value != 0; return c; });
        game("order", "training.order", -1, {{0, "training.order.random", {}}, {1, "training.order.in_order", {}}}, [](const View& v) { return static_cast<int>(v.order); },
            [](int value, const View& v) { Command c; c.action = Action::PlayOrder; c.generation = v.generation; c.order = value ? Order::InOrder : Order::Random; return c; });
        for (int slot = 0; slot < SlotCount; ++slot)
            game(("slot" + std::to_string(slot)).c_str(), "training.slot", slot + 1, OffOn(), [slot](const View& v) { return static_cast<int>(v.on[slot]); },
                [slot](int value, const View& v) { Command c; c.action = Action::SlotOn; c.generation = v.generation; c.slot = slot; c.on = value != 0; return c; });
        // The reply: when it plays, how likely, how early or late, and a different stance.
        const auto reply = [&](const char* id, const char* label, std::vector<Choice> choices, auto field) {
            all.push_back(Lab(id, label, Tab::Reply, "training.reply", std::move(choices), Member(&Settings::plan, field)));
        };
        reply("replyWhen", "training.reply", {{0, "common.off", {}}, {1, "training.reply.hit", {}}, {2, "training.reply.block", {}}, {3, "training.reply.rise", {}}, {4, "training.reply.any", {}}}, &DummyPlan::when);
        reply("replyChance", "training.reply.chance", Numbers({25, 50, 75, 100}, "%"), &DummyPlan::chance);
        std::vector<Choice> timing;
        for (int t = -MaxReplyTiming; t <= MaxReplyTiming; ++t) timing.push_back({t, nullptr, (t > 0 ? "+" : "") + std::to_string(t) + " f"});
        reply("replyTiming", "training.reply.timing", std::move(timing), &DummyPlan::timing);
        reply("replyStance", "training.reply.vary_stance", OffOn(), &DummyPlan::varyStance);
        // The frame meter, then the Data panel and the hitboxes, as the HUD pages group them.
        const char* const frames = "training.frame_data";
        meter("style", "training.meter.style", frames, {{0, "training.meter.style.angled", {}}, {1, "training.meter.style.flat", {}}, {2, "training.meter.style.classic", {}}}, &MeterOptions::style);
        meter("shown", "training.meter.frames_shown", frames, Numbers({60, 90, 120}, ""), &MeterOptions::shown);
        meter("size", "training.meter.hud_size", frames, detail::Sizes(), &MeterOptions::size);
        {
            std::vector<Choice> inputs;
            for (int i = 0; i < InputDisplays; ++i) inputs.push_back({i, InputDisplayChoices[i].label, {}});
            meter("inputs", "training.meter.hud_inputs", frames, std::move(inputs), &MeterOptions::inputs);
        }
        meter("recovery", "training.meter.hud_recovery", frames, OffOn(), &MeterOptions::recovery);
        for (const auto& part : HudParts) meter(part.key, part.label, frames, OffOn(), part.shown);
        for (const auto& part : ClassicParts) meter(part.key, part.label, frames, OffOn(), part.shown);
        meter("position", "training.meter.position", frames, Numbers({12, 25, 40, 55, 70, 82, 92}, "%"), &MeterOptions::position);
        meter("barHeight", "training.meter.bar_height", frames, {{12, "size.small", {}}, {16, "size.standard", {}}, {22, "size.large", {}}}, &MeterOptions::barHeight);
        meter("history", "training.meter.history", frames, {{240, nullptr, "4 s"}, {600, nullptr, "10 s"}, {1200, nullptr, "20 s"}, {3600, nullptr, "60 s"}}, &MeterOptions::history);
        meter("afterPause", "training.meter.after_pause", frames, {{0, "training.meter.after_pause.start_over", {}}, {1, "training.meter.after_pause.continue", {}}}, &MeterOptions::continuous);
        meter("scroll", "training.meter.scroll", frames, {{0, "training.meter.scroll.pages", {}}, {1, "training.meter.scroll.smooth", {}}}, &MeterOptions::smooth);
        const char* const data = "training.data";
        meter("data", "training.data", data, OffOn(), &MeterOptions::data);
        {
            std::vector<Choice> sides;
            for (int i = 0; i < DataFighters; ++i) sides.push_back({i, DataFighterChoices[i].label, {}});
            meter("dataFighters", "training.data.fighters", data, std::move(sides), &MeterOptions::dataFighters);
        }
        meter("dataOpacity", "training.data.opacity", data, Numbers({40, 60, 80, 100}, "%"), &MeterOptions::dataOpacity);
        for (const auto& part : DataParts) meter(part.key, part.label, data, OffOn(), part.shown);
        const char* const boxes = "training.hitboxes";
        meter("hitboxes", "training.hitboxes", boxes, OffOn(), &MeterOptions::hitboxes);
        for (const auto& part : HitboxParts) meter(part.key, part.label, boxes, OffOn(), part.shown);
        // Combo check's choices; the fighters it tries are picked on the PC.
        const char* const check = "training.combo_check";
        const auto checkOption = [&](const char* id, const char* label, std::vector<Choice> choices, auto field) {
            all.push_back(Lab(id, label, Tab::Tools, check, std::move(choices), Member(&Settings::check, field)));
        };
        checkOption("checkStanding", "training.combo_check.stance.standing", OffOn(), &combo_check::Settings::standing);
        checkOption("checkCrouching", "training.combo_check.stance.crouching", OffOn(), &combo_check::Settings::crouching);
        checkOption("checkStart", "training.combo_check.start_from", {{0, "training.combo_check.start_from.recorded", {}}, {1, "training.combo_check.start_from.middle", {}}, {2, "training.combo_check.start_from.corner", {}}}, &combo_check::Settings::start);
        checkOption("checkSpeed", "training.combo_check.speed", {{0, "training.combo_check.speed.normal", {}}, {1, "training.combo_check.speed.fast", {}}}, &combo_check::Settings::fast);
        return all;
    }();
    return options;
}
inline const Option* FindOption(const std::string& id) {
    const auto& all = Options();
    const auto found = std::find_if(all.begin(), all.end(), [&](const Option& o) { return o.id == id; });
    return found == all.end() ? nullptr : &*found;
}
// Sets a Lab option to a value its choices name; false when it is not one.
inline bool WriteLab(Settings& settings, const std::string& id, int value) {
    const auto* option = FindOption(id);
    if (!option || !option->lab || !option->Has(value)) return false;
    option->lab(settings, &value);
    return true;
}
// The Lab options' values, by id.
inline nlohmann::json ReadLab(Settings& settings) {
    nlohmann::json values = nlohmann::json::object();
    for (const auto& option : Options()) if (option.lab) values[option.id] = option.lab(settings, nullptr);
    return values;
}
// The overlay thread's side of the phone: it publishes the Lab values and takes
// what the phone changed since (an id and a value, or the id of an action).
// The server thread only reads the one and queues the other.
class Mailbox {
public:
    static constexpr std::size_t Capacity = 32;
    void Publish(nlohmann::json values) { std::lock_guard<std::mutex> hold(lock_); values_ = std::move(values); }
    nlohmann::json Values() const { std::lock_guard<std::mutex> hold(lock_); return values_; }
    bool Push(std::string id, int value) {
        std::lock_guard<std::mutex> hold(lock_);
        if (pending_.size() >= Capacity) return false;
        pending_.emplace_back(std::move(id), value); return true;
    }
    // A typed order (the reply's moves): an id and its text.
    bool PushText(std::string id, std::string text) {
        std::lock_guard<std::mutex> hold(lock_);
        if (texts_.size() >= Capacity) return false;
        texts_.emplace_back(std::move(id), std::move(text)); return true;
    }
    std::vector<std::pair<std::string, std::string>> TakeText() {
        std::lock_guard<std::mutex> hold(lock_);
        return std::exchange(texts_, {});
    }
    std::vector<std::pair<std::string, int>> Take() {
        std::lock_guard<std::mutex> hold(lock_);
        return std::exchange(pending_, {});
    }
private:
    mutable std::mutex lock_;
    nlohmann::json values_ = nlohmann::json::object();
    std::vector<std::pair<std::string, int>> pending_;
    std::vector<std::pair<std::string, std::string>> texts_;
};
inline Mailbox& Phone() { static Mailbox mailbox; return mailbox; }
} } }
