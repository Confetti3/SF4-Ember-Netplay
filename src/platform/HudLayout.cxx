#include "HudLayout.hxx"
#include <zlib.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <regex>
#include <set>

namespace sf4e { namespace platform { namespace hud_layout {
namespace {
using Keys = std::vector<std::array<float, 8>>;

// A #EMS file: the offsets the game's own tables hold. Edits keep every
// offset valid; new key tables go on the end.
class Ems {
public:
    explicit Ems(std::string data) : d(std::move(data)) {
        if (d.size() < 0x20 || d.compare(0, 4, "#EMS") != 0) return;
        const uint32_t count = U32(0xC), table = U32(0x14);
        if (table + 4ull * count > d.size()) return;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t at = U32(table + 4 * i);
            if (at + 0x40 > d.size()) return;
            order.push_back(Name(at));
            nodes[order.back()] = at;
        }
        ok = true;
    }
    bool ok = false;
    std::string d;
    std::vector<std::string> order;
    std::map<std::string, uint32_t> nodes;

    uint32_t U32(size_t o) const { uint32_t v = 0; if (o + 4 <= d.size()) std::memcpy(&v, d.data() + o, 4); return v; }
    int S16(size_t o) const { int16_t v = 0; if (o + 2 <= d.size()) std::memcpy(&v, d.data() + o, 2); return v; }
    float F(size_t o) const { float v = 0; if (o + 4 <= d.size()) std::memcpy(&v, d.data() + o, 4); return v; }
    void PutF(size_t o, float v) { if (o + 4 <= d.size()) std::memcpy(&d[o], &v, 4); }
    void PutS16(size_t o, int v) { const int16_t s = (int16_t)v; if (o + 2 <= d.size()) std::memcpy(&d[o], &s, 2); }
    void PutU32(size_t o, uint32_t v) { if (o + 4 <= d.size()) std::memcpy(&d[o], &v, 4); }
    std::string Name(uint32_t at) const { return std::string(d.c_str() + at + 0x20, strnlen(d.c_str() + at + 0x20, 0x20)); }
    uint32_t Alloc(size_t n) { while (d.size() % 16) d.push_back(0); const uint32_t o = (uint32_t)d.size(); d.append(n, '\0'); return o; }

    uint32_t Action(const std::string& n) const { return U32(nodes.at(n) + 0x10); }
    // G1 own tracks, G2 sprite and colour, G3 child tracks, G4 user events.
    uint32_t Group(uint32_t action, int i) const { return U32(action + 4 + 4 * i); }
    uint32_t G1(const std::string& n) const { const uint32_t a = Action(n); return a ? Group(a, 0) : 0; }
    std::array<float, 4> Rect(const std::string& n) const { const uint32_t o = Action(n) + 0x20; return { F(o), F(o + 4), F(o + 8), F(o + 12) }; }
    void SetRect(const std::string& n, const std::array<float, 4>& r) { const uint32_t o = Action(n) + 0x20; for (int i = 0; i < 4; ++i) PutF(o + 4 * i, r[i]); }
    // G1 +4/+6: size-rotation key count/length, +8: position key count; +0x14 size-rotation keys, +0x1c position keys (32 bytes each).
    bool HasPos(const std::string& n) const { const uint32_t g = G1(n); return g && U32(g + 0x1c); }
    std::array<float, 3> Pos(const std::string& n) const { const uint32_t p = U32(G1(n) + 0x1c); return { F(p), F(p + 4), F(p + 8) }; }
    Keys SizeRot(const std::string& n) const {
        Keys keys; const uint32_t g = G1(n), p = U32(g + 0x14), count = (uint32_t)S16(g + 4);
        if (!p) return keys;
        for (uint32_t k = 0; k < count; ++k) { std::array<float, 8> v; for (int i = 0; i < 8; ++i) v[i] = F(p + 32 * k + 4 * i); keys.push_back(v); }
        return keys;
    }
    void SetSizeRot(const std::string& n, const Keys& keys) {
        const uint32_t g = G1(n), count = (uint32_t)keys.size(), old = (uint32_t)S16(g + 4);
        if (U32(g + 0x14) && old == count) {      // same shape: edit in place
            for (uint32_t k = 0; k < count; ++k) for (int i = 0; i < 8; ++i) PutF(U32(g + 0x14) + 32 * k + 4 * i, keys[k][i]);
            return;
        }
        const uint32_t header = Alloc(4 * count), values = Alloc(32 * count);
        const int length = S16(g + 6) ? S16(g + 6) : 1;
        for (uint32_t k = 0; k < count; ++k) {
            PutS16(header + 4 * k, (int)(length * (int)k / std::max<int>((int)count - 1, 1)));
            PutS16(header + 4 * k + 2, k + 1 < count ? 1 : 0);
            for (int i = 0; i < 8; ++i) PutF(values + 32 * k + 4 * i, keys[k][i]);
        }
        PutS16(g + 4, (int)count); PutS16(g + 6, length);
        PutU32(g + 0x10, header); PutU32(g + 0x14, values);
    }
};

struct Rot { double x, y; };
Rot Rotate(double x, double y, double deg) {
    const double c = std::cos(deg * 3.14159265358979323846 / 180), s = std::sin(deg * 3.14159265358979323846 / 180);
    return { x * c - y * s, x * s + y * c };
}

const std::regex& BoxSkip() {
    static const std::regex skip("blink|thunder|fire|on2|_ef\\d|efc|num(?![123]$)|text\\d|_color|time_loop|_on_|_on\\d|_max$|uc_max|revenge_uc|revenge_max|uc_on|_time");
    return skip;
}

// Sprite nodes of an element that are on screen: leaves with a position track, not relative effects.
std::vector<std::string> NodesOf(const Ems& e, const std::string& pattern) {
    std::vector<std::string> out;
    const std::regex rx(pattern);
    for (const std::string& n : e.order) {
        if (!std::regex_search(n, rx) || (n.size() >= 4 && n.compare(n.size() - 4, 4, "_efc") == 0)) continue;
        if (e.HasPos(n) && e.Pos(n)[0] < 2000) out.push_back(n);
    }
    return out;
}

// The size of a sprite with one size-rotation key (names are drawn 0.87x in the game); fills (ramps over several keys) are not scaled.
std::array<float, 2> StaticSize(const Ems& e, const std::string& n) {
    const Keys k = e.SizeRot(n);
    return k.size() == 1 && k[0][2] > 0 && k[0][3] > 0 ? std::array<float, 2>{ k[0][2], k[0][3] } : std::array<float, 2>{ 1, 1 };
}

std::vector<Rot> NodeBox(const Ems& e, const std::string& n) {
    const auto pos = e.Pos(n); const auto size = StaticSize(e, n); const auto raw = e.Rect(n);
    const double r[4] = { raw[0] * size[0], raw[1] * size[1], raw[2] * size[0], raw[3] * size[1] };
    const Keys keys = e.SizeRot(n); const double rot = keys.empty() ? 0 : keys[0][4];
    std::vector<Rot> out;
    for (const auto& c : { Rot{ r[0], r[1] }, Rot{ r[2], r[1] }, Rot{ r[2], r[3] }, Rot{ r[0], r[3] } }) {
        const Rot p = Rotate(c.x, c.y, rot); out.push_back({ pos[0] + p.x, pos[1] + p.y });
    }
    return out;
}

struct Box { double x0, y0, x1, y1; };
bool BoxOf(const Element& el, const std::map<std::string, Ems>& files, Box& box) {
    const auto it = files.find(el.file);
    if (it == files.end()) return false;
    std::vector<Rot> pts;
    for (const std::string& n : NodesOf(it->second, el.pattern)) {
        const auto r = it->second.Rect(n);
        if (std::regex_search(n, BoxSkip()) || (!r[0] && !r[1] && !r[2] && !r[3])) continue;
        const auto b = NodeBox(it->second, n); pts.insert(pts.end(), b.begin(), b.end());
    }
    if (pts.empty()) return false;
    box = { pts[0].x, pts[0].y, pts[0].x, pts[0].y };
    for (const Rot& p : pts) { box.x0 = std::min(box.x0, p.x); box.y0 = std::min(box.y0, p.y); box.x1 = std::max(box.x1, p.x); box.y1 = std::max(box.y1, p.y); }
    return true;
}

bool Load(const std::map<std::string, std::string>& emz, std::map<std::string, Ems>& files) {
    for (const auto& f : emz) {
        std::string ems;
        if (!Unpack(f.second, ems)) return false;
        Ems e(std::move(ems));
        if (!e.ok) return false;
        files.emplace(f.first, std::move(e));
    }
    return true;
}

std::vector<Info> Infos(const std::map<std::string, Ems>& files) {
    std::map<std::string, Info> info;
    auto put = [&](const Element& el, const Box& b) {
        info[el.id] = { el.id, el.label, el.parent, (b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2, b.x1 - b.x0, b.y1 - b.y0 };
    };
    for (const Element& el : Elements()) { Box b; if (!el.file.empty() && BoxOf(el, files, b)) put(el, b); }
    for (const Element& el : Elements()) {
        if (!el.file.empty()) continue;
        bool any = false; Box b{ 0, 0, 0, 0 };
        for (const Element& kid : Elements()) {
            const auto it = info.find(kid.id);
            if (kid.parent != el.id || it == info.end()) continue;
            const Info& k = it->second;
            const Box kb{ k.cx - k.w / 2, k.cy - k.h / 2, k.cx + k.w / 2, k.cy + k.h / 2 };
            b = any ? Box{ std::min(b.x0, kb.x0), std::min(b.y0, kb.y0), std::max(b.x1, kb.x1), std::max(b.y1, kb.y1) } : kb; any = true;
        }
        if (any) put(el, b);
    }
    std::vector<Info> out;
    for (const Element& el : Elements()) { const auto it = info.find(el.id); if (it != info.end()) out.push_back(it->second); }
    return out;
}

struct Link { double tx, ty, rot, scale, cx, cy; bool hide; };

// An element's edit, then each group above it, innermost first.
std::vector<Link> Chain(const std::string& id, const Layout& layout, const std::map<std::string, Info>& info) {
    std::vector<Link> chain;
    for (std::string at = id; !at.empty();) {
        const auto edit = layout.find(at);
        const auto where = info.find(at);
        if (edit != layout.end() && where != info.end())
            chain.push_back({ edit->second.x, edit->second.y, edit->second.rot, edit->second.scale, where->second.cx, where->second.cy, edit->second.hide });
        const auto el = std::find_if(Elements().begin(), Elements().end(), [&](const Element& x) { return x.id == at; });
        at = el == Elements().end() ? std::string() : el->parent;
    }
    return chain;
}

void ApplyNode(Ems& e, const std::string& n, const std::vector<Link>& chain, std::set<uint32_t>& seen) {
    const uint32_t g = e.G1(n), positions = e.U32(g + 0x1c);
    const int count = e.S16(g + 8);
    double rotation = 0, scale = 1; bool hide = false;
    for (const Link& c : chain) { rotation += c.rot; scale *= c.scale; hide = hide || c.hide; }
    if (seen.insert(positions).second) {
        for (int j = 0; j < count; ++j) {
            double x = e.F(positions + 32 * j), y = e.F(positions + 32 * j + 4);
            for (const Link& c : chain) { const Rot r = Rotate(c.scale * (x - c.cx), c.scale * (y - c.cy), c.rot); x = c.tx + r.x; y = c.ty + r.y; }
            e.PutF(positions + 32 * j, (float)(x + (hide ? 4000 : 0))); e.PutF(positions + 32 * j + 4, (float)y);
        }
    }
    if (scale != 1) { auto r = e.Rect(n); for (float& v : r) v = (float)(v * scale); e.SetRect(n, r); }
    if (rotation != 0) {
        const uint32_t q = e.U32(g + 0x14);
        if (q && seen.count(q)) return;
        if (q) seen.insert(q);
        Keys keys = e.SizeRot(n);
        if (keys.empty()) keys.push_back({ 1, 1, 1, 1, 0, 1, 1, 1 });
        for (auto& k : keys) k[4] = (float)std::fmod(std::fmod(k[4] + rotation, 360) + 360, 360);
        e.SetSizeRot(n, keys);
    }
}
}

const std::vector<std::string>& Files() {
    static const std::vector<std::string> files = { "cockpit", "chara", "cockpit_text", "uc" };
    return files;
}

const std::vector<Element>& Elements() {
    static const std::vector<Element> elements = [] {
        std::vector<Element> v;
        for (int p = 1; p <= 2; ++p) {
            const std::string s = std::to_string(p), g = "p" + s + "_all", pre = "p" + s + "_";
            v.push_back({ g, "P" + s + " cluster (all of it)", "", "", "" });
            v.push_back({ pre + "plate", "Plate / avatar plate", "cockpit", "^" + s + "_vital_base_2$", g });
            v.push_back({ pre + "life", "Life bar", "cockpit", "^" + s + "_(vital|recov|vital_max|vital_base_1|vital_frame_1|vital_frame_act)$", g });
            v.push_back({ pre + "super", "Super bar", "cockpit", "^" + s + "_(sc(_|$)|combo$|ex_on$)", g });
            v.push_back({ pre + "ultra", "Ultra meter", "cockpit", "^" + s + "_(revenge(?!_num|_frame)|uc_)", g });
            v.push_back({ pre + "ultradiv", "Ultra divider (3 + 3 bars)", "cockpit", "^" + s + "_revenge_frame$", g });
            v.push_back({ pre + "ultranum", "Ultra indicator (I / II / W)", "cockpit", "^" + s + "_revenge_num[123](_on|_max)?$", g });
            v.push_back({ pre + "wins", "Win marks", "cockpit", "^" + s + "_winmark", g });
            v.push_back({ pre + "edition", "Edition mark", "cockpit", "^" + s + "_edition_", g });
            v.push_back({ pre + "face", "Portrait", "chara", "^" + s + "p_face$", g });
            v.push_back({ pre + "name", "Fighter name", "chara", "^" + s + "_name$", g });
            v.push_back({ pre + "label", "PLAYER / CPU label", "cockpit_text", "^" + s + "_(1p|2p)$", g });
            v.push_back({ pre + "ucname", "Ultra move name", "uc", "^" + s + "_uc$", g });
        }
        v.push_back({ "ko", "K.O. frame", "cockpit", "^ko_frame$", "" });
        v.push_back({ "clock", "Clock digits", "cockpit", "^time_(inf|[123])$", "" });
        v.push_back({ "kotext", "K.O. text", "cockpit_text", "^ko_text$", "" });
        return v;
    }();
    return elements;
}

std::vector<Sprite> Sprites(const std::map<std::string, std::string>& emz) {
    std::map<std::string, Ems> files;
    std::vector<Sprite> out;
    if (!Load(emz, files)) return out;
    std::map<std::string, std::set<std::string>> seen;
    for (const Element& el : Elements()) {
        const auto file = files.find(el.file);
        if (el.file.empty() || file == files.end()) continue;
        const Ems& e = file->second;
        for (const std::string& n : NodesOf(e, el.pattern)) {
            if (!seen[el.file].insert(n).second) continue;
            const uint32_t action = e.Action(n), g2 = e.Group(action, 1), key = g2 ? e.U32(g2 + 0x14) : 0;
            const auto size = StaticSize(e, n);
            const auto raw = e.Rect(n);
            const float r[4] = { raw[0] * size[0], raw[1] * size[1], raw[2] * size[0], raw[3] * size[1] };
            if (!key || (!r[0] && !r[1] && !r[2] && !r[3])) continue;
            Sprite s;
            s.node = n; s.element = el.id; s.file = el.file;
            s.tex = (int)e.U32(key);
            for (int i = 0; i < 4; ++i) { s.uv[i] = (int)std::lround(e.F(key + 16 + 4 * i)); s.r[i] = r[i]; }
            if (s.uv[2] <= s.uv[0] || s.uv[3] <= s.uv[1] || s.tex > 40) continue;
            const auto pos = e.Pos(n); const Keys keys = e.SizeRot(n);
            for (int i = 0; i < 3; ++i) s.pos[i] = pos[i];
            s.rot = keys.empty() ? 0 : keys[0][4];
            s.additive = (e.U32(action) & 0x200) != 0;
            out.push_back(std::move(s));
        }
    }
    return out;
}

Layout LayoutFromJson(const nlohmann::json& json) {
    Layout layout;
    if (!json.is_object()) return layout;
    for (const Element& el : Elements()) {
        const auto found = json.find(el.id);
        if (found == json.end() || !found->is_object()) continue;
        const auto number = [&](const char* key, double& to) {
            const auto v = found->find(key);
            if (v == found->end() || !v->is_number()) return false;
            to = v->get<double>();
            return std::isfinite(to) && std::fabs(to) < 1e5;
        };
        Edit edit;
        if (!number("x", edit.x) || !number("y", edit.y) || !number("rot", edit.rot) || !number("s", edit.scale) || edit.scale <= 0) continue;
        const auto hide = found->find("hide");
        edit.hide = hide != found->end() && hide->is_boolean() && hide->get<bool>();
        layout[el.id] = edit;
    }
    return layout;
}

nlohmann::json LayoutToJson(const Layout& layout) {
    nlohmann::json json = nlohmann::json::object();
    for (const Element& el : Elements()) {
        const auto found = layout.find(el.id);
        if (found != layout.end()) json[el.id] = { { "x", found->second.x }, { "y", found->second.y }, { "rot", found->second.rot }, { "s", found->second.scale }, { "hide", found->second.hide } };
    }
    return json;
}

bool DesktopAgent(const std::string& userAgent) {
    std::string lower = userAgent;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.empty()) return false;
    for (const char* mobile : { "mobile", "android", "iphone", "ipad", "ipod", "tablet", "silk", "kindle", "playbook", "nintendo", "playstation", "steamdeck", "touch" })
        if (lower.find(mobile) != std::string::npos) return false;
    return true;
}

bool Unpack(const std::string& emz, std::string& ems) {
    if (emz.size() < 16) return false;
    if (emz.compare(0, 4, "#EMS") == 0) { ems = emz; return true; }      // a plain file under an .emz name
    if (emz.compare(0, 4, "#EMZ") != 0) return false;
    uint32_t size = 0, offset = 0;
    std::memcpy(&size, emz.data() + 8, 4); std::memcpy(&offset, emz.data() + 12, 4);
    if (offset < 16 || offset > emz.size() || size > (64u << 20)) return false;
    ems.assign(size, '\0');
    z_stream z{};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return false;
    z.next_in = (Bytef*)emz.data() + offset; z.avail_in = (uInt)(emz.size() - offset);
    z.next_out = (Bytef*)&ems[0]; z.avail_out = size;
    const int result = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    return result == Z_STREAM_END && z.avail_out == 0;
}

std::string Pack(const std::string& ems) {
    std::string out(16, '\0');
    out.replace(0, 4, "#EMZ");
    const uint32_t crc = (uint32_t)crc32(0, (const Bytef*)ems.data(), (uInt)ems.size()), size = (uint32_t)ems.size(), offset = 16;
    std::memcpy(&out[4], &crc, 4); std::memcpy(&out[8], &size, 4); std::memcpy(&out[12], &offset, 4);
    z_stream z{};
    deflateInit2(&z, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    std::string body(deflateBound(&z, (uLong)ems.size()), '\0');
    z.next_in = (Bytef*)ems.data(); z.avail_in = (uInt)ems.size();
    z.next_out = (Bytef*)&body[0]; z.avail_out = (uInt)body.size();
    deflate(&z, Z_FINISH);
    body.resize(z.total_out);
    deflateEnd(&z);
    return out + body;
}

std::vector<Info> Describe(const std::map<std::string, std::string>& emz) {
    std::map<std::string, Ems> files;
    return Load(emz, files) ? Infos(files) : std::vector<Info>();
}

bool Apply(const std::map<std::string, std::string>& emz, const Layout& layout, std::map<std::string, std::string>& out) {
    std::map<std::string, Ems> files;
    if (!Load(emz, files)) return false;
    std::map<std::string, Info> info;
    for (const Info& i : Infos(files)) info[i.id] = i;     // positions before any edit: an edit is relative to the stock centre
    out.clear();
    for (auto& f : files) {
        std::set<uint32_t> seen; bool touched = false;
        for (const Element& el : Elements()) {
            if (el.file != f.first || !info.count(el.id)) continue;
            const std::vector<Link> chain = Chain(el.id, layout, info);
            if (chain.empty()) continue;
            for (const std::string& n : NodesOf(f.second, el.pattern)) { ApplyNode(f.second, n, chain, seen); touched = true; }
        }
        if (touched) out[f.first] = Pack(f.second.d);
    }
    return true;
}
} } }
