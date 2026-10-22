// Ember's UI lab: the real shell (ApplicationShell, GameMenu, the V2 kit) in a
// window of its own, with made-up data and no game or network, so a layout can
// be tried at every screen size in seconds. Keyboard, mouse and an XInput pad
// work as in the game.
//
// F1 lab bar · F2 next size · F3 Look (Classic/V2) · F4 text size · F5 in a room · F6 picture · F12 screenshot
// BluMint room (--blumint): F7 next moment · F8 fighter or watcher
// --public: an Ember ID, a relay in Europe and five public rooms (Online play, Public rooms).
// --playback: a replay's strip and input lanes over the art, paused at half speed
// --rankings: an Ember ID with Rankings' answers (--screen rankings or rankings-board)
// UiLab.exe [--size WxH] [--scale 1.25] [--v2] [--room] [--public] [--rankings] [--playback] [--pip] [--hud 0|1] [--training] [--update] [--screen id] [--keys down,down,enter] [--shot file.bmp]
//   With --shot it plays --keys (one per 6 frames), saves the picture and exits.
#include "../ApplicationShell.hxx"
#include "../ControllerNavigation.hxx"
#include "../FighterSelector.hxx"
#include "../ReplayPlaybackHud.hxx"
#include "../SelectionArt.hxx"
#include "../TrainingPanel.hxx"
#include "../Theme.hxx"
#include "../V2.hxx"
#include "../../common/Localization.hxx"
#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>
#include <windows.h>
#include <shellapi.h>
#include <xinput.h>
#include <d3d9.h>
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
using namespace sf4e;
struct Size { int w, h; const char* name; };
// The V2 design's screen sizes.
const Size Sizes[] = {{1280,800,"Deck"},{1280,720,"720p"},{1366,768,"Laptop"},{1920,1080,"1080p"},{1920,1200,"16:10"},{2560,1440,"1440p"},
    {2560,1080,"21:9"},{3440,1440,"21:9 QHD"},{3840,2160,"4K"},{5120,1440,"32:9"},{1024,768,"4:3"},{800,600,"Small"},{640,720,"Narrow"}};
IDirect3DDevice9* device = nullptr;
D3DPRESENT_PARAMETERS present{};
int resizeW = 0, resizeH = 0;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, w, l)) return 1;
    if (msg == WM_SIZE && w != SIZE_MINIMIZED) { resizeW = LOWORD(l); resizeH = HIWORD(l); return 0; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, w, l);
}
void ResizeWindow(HWND hwnd, int w, int h) {
    RECT r{0, 0, w, h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}
void Reset(int w, int h) {
    ImGui_ImplDX9_InvalidateDeviceObjects();
    present.BackBufferWidth = w; present.BackBufferHeight = h;
    device->Reset(&present);
    ImGui_ImplDX9_CreateDeviceObjects();
}
// The back buffer as a 32-bit BMP.
bool SaveShot(const std::string& path) {
    IDirect3DSurface9 *target = nullptr, *copy = nullptr;
    if (FAILED(device->GetRenderTarget(0, &target))) return false;
    D3DSURFACE_DESC desc{}; target->GetDesc(&desc);
    bool ok = SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &copy, nullptr)) &&
        SUCCEEDED(device->GetRenderTargetData(target, copy));
    target->Release();
    D3DLOCKED_RECT pixels{};
    if (ok) ok = SUCCEEDED(copy->LockRect(&pixels, nullptr, D3DLOCK_READONLY));
    if (ok) {
        BITMAPFILEHEADER file{}; BITMAPINFOHEADER info{};
        file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(info); file.bfSize = file.bfOffBits + desc.Width * desc.Height * 4;
        info.biSize = sizeof(info); info.biWidth = desc.Width; info.biHeight = -static_cast<LONG>(desc.Height); info.biPlanes = 1; info.biBitCount = 32;
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(&file), sizeof(file)); out.write(reinterpret_cast<const char*>(&info), sizeof(info));
        for (UINT y = 0; y < desc.Height; ++y) out.write(static_cast<const char*>(pixels.pBits) + y * pixels.Pitch, desc.Width * 4);
        copy->UnlockRect();
    }
    if (copy) copy->Release();
    return ok;
}
// The first connected XInput pad's menu buttons, as the overlay hands them to the menu.
unsigned PadButtons() {
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE state{};
        if (XInputGetState(i, &state) != ERROR_SUCCESS) continue;
        const auto b = state.Gamepad.wButtons;
        unsigned held = (b & XINPUT_GAMEPAD_DPAD_UP ? 1 : 0) | (b & XINPUT_GAMEPAD_DPAD_DOWN ? 2 : 0) |
            (b & XINPUT_GAMEPAD_DPAD_LEFT ? 4 : 0) | (b & XINPUT_GAMEPAD_DPAD_RIGHT ? 8 : 0);
        std::uint32_t physical = 0;
        // The XInput masks of common/PadKind.hxx sit 16 bits up for the face buttons.
        if (b & XINPUT_GAMEPAD_A) physical |= input::xinput::A;
        if (b & XINPUT_GAMEPAD_B) physical |= input::xinput::B;
        if (b & XINPUT_GAMEPAD_X) physical |= input::xinput::X;
        if (b & XINPUT_GAMEPAD_Y) physical |= input::xinput::Y;
        if (b & XINPUT_GAMEPAD_BACK) physical |= input::xinput::View;
        if (b & XINPUT_GAMEPAD_LEFT_SHOULDER) physical |= input::xinput::LB;
        if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) physical |= input::xinput::RB;
        return ui::ControllerButtons(held, input::PadXInput, physical, input::MenuButtons{});
    }
    return 0;
}
unsigned KeyBit(const std::string& key) {
    using B = ui::MenuInput;
    const std::pair<const char*, unsigned> keys[] = {{"up",B::Up},{"down",B::Down},{"left",B::Left},{"right",B::Right},{"enter",B::Select},
        {"back",B::Back},{"x",B::Fighter},{"y",B::Options},{"view",B::Chat},{"lb",B::PrevTab},{"rb",B::NextTab},{"start",B::Start}};
    for (const auto& k : keys) if (key == k.first) return k.second;
    return 0;
}
// A ShellView with a player, a controller and nothing running.
ui::ShellView FakeView(bool v2) {
    ui::ShellView view;
    view.controllerReady = view.canChangeController = view.canEditPreferences = view.canEditSelection = view.canOpenRoom = view.helperReady = true;
    view.controller = "Xbox pad";
    view.preferences.displayName = "FRaccie";
    view.automaticCountry = CountryCode::Parsed("BE");
    view.preferences.lookV2 = v2;
    view.selectionSummary = "Ryu / Original / Color 01 / Ultra I";
    // The card's title and icon, and Home's news, as the backend and the release check give them.
    view.playerTitle = "TOURNAMENT ORGANIZER"; view.playerTier = ui::v2::Tier::Gold; view.playerIcon = 7;
    view.services.update.news = {{"v1.3.0", "Ember 1.3.0", "2026-10-21", "New menus: categories, panels and choices.\n- Spectators keep watching through Alt+Enter.", ""},
        {"v1.2.1", "Ember 1.2.1", "2026-10-17", "Fixes for rollback saves.", ""}, {"v1.2.0", "Ember 1.2.0", "2026-10-14", "Borderless window setting.", ""}};
    view.services.update.news.insert(view.services.update.news.begin() + 1, {"v1.3.0-nightly.12", "Nightly 1.3.0.12", "2026-10-19", "Room board fits Steam Deck.\n- Speech for menus.", ""});
    for (auto& n : view.services.update.news) { n.stable = n.tag.find('-') == std::string::npos; n.newer = n.tag > "v1.2.0"; }
    // Replays: matches played and watched, the first three a set (newest first).
    auto archive = std::make_shared<std::vector<platform::replays::ArchivedReplay>>();
    const auto replay = [&](const char* file, const char* date, std::uint64_t time, const char* a, const char* b, int fa, int fb, int sa, int sb, bool spectated, bool seen) {
        platform::replays::ArchivedReplay r; r.path = file; r.label = date; r.time = time; r.names[0] = a; r.names[1] = b; r.fighters[0] = fa; r.fighters[1] = fb;
        r.spectated = spectated; r.watched = seen; r.summary = replayinputs::Summary{}; r.summary->scored = true; r.summary->score[0] = sa; r.summary->score[1] = sb;
        r.summary->rounds = static_cast<std::uint32_t>(sa + sb); r.summary->frames = 60 * 75 * r.summary->rounds; archive->push_back(r);
    };
    replay("C:/replays/4.replay", "2026-10-21 21:14", 5000, "FRaccie", "Katie", 0, 16, 2, 1, false, false);
    replay("C:/replays/3.replay", "2026-10-21 21:02", 4300, "FRaccie", "Katie", 0, 16, 0, 2, false, true);
    replay("C:/replays/2.replay", "2026-10-21 20:51", 3600, "Katie", "FRaccie", 16, 0, 1, 2, false, false);
    replay("C:/replays/1.replay", "2026-10-21 20:30", 1000, "Katie", "Jimbo", 16, 13, 2, 0, true, false);
    replay("C:/replays/0.replay", "2026-10-20 22:10", 100, "Mo", "Ash", 6, 1, 1, 2, true, true);
    view.replays.archive = archive; view.replays.ready = true;
    return view;
}
// A joined room with four tables, sixteen members and some chat (the render test's sample room).
void JoinRoom(ui::ShellView& view) {
    view.session.room = netplay::RoomState::Joined; view.session.control = netplay::Health::Healthy; view.session.generation.room = 1;
    view.room.roomEpoch = 42; view.room.name = "Friday Night Fights"; view.room.capacity = 16; view.room.host = view.room.localMember = 1;
    view.canEditPreferences = false; view.canReady = true; view.localSlot = 0; view.invitation = "sf4://lab-only";
    for (int i = 0; i < 4; ++i) {
        auto& t = view.room.tables[i]; t.id = i; t.revision = 1;
        t.phase = i == 0 ? room::TablePhase::Waiting : i == 1 ? room::TablePhase::Playing : room::TablePhase::Idle;
        if (i < 2) { t.p1 = i * 2 + 1; t.p2 = i * 2 + 2; t.score[0] = i * 2; t.score[1] = i; }
    }
    const char* names[] = {"FRaccie","Akira","Jamie","Alex","Morgan","Riley","Sam","Jordan","Casey","Taylor","Robin","Ash","Sky","Reese","Avery","Drew"};
    const char* countries[] = {"BE","JP","GB","US","FR","DE","NL","BR","KR","ES","IT","PL","CA","MX","SE","AU"};
    for (int i = 1; i <= 16; ++i) {
        room::Member m; m.id = i; m.name = names[i - 1]; m.host = i == 1; m.fighter = (i - 1) * 2; m.mainFighter = (i + 7) % 44;
        m.country = CountryCode::Parsed(countries[i - 1]);
        if (i <= 4) { m.table = (i - 1) / 2; m.seat = (i - 1) % 2; m.status = i < 3 ? room::MemberStatus::Seated : room::MemberStatus::Playing; }
        m.link = static_cast<NetworkLink>(i % 3);
        // Class marks and pings, as a public room shows them.
        m.badge = i == 2 ? SkillBadge::Developer() : i == 3 ? SkillBadge::Placing() : i < 9 ? SkillBadge::OfClass(static_cast<std::uint8_t>(i), i % 2 == 0) : SkillBadge{};
        if (i > 1) { session::MemberLatency l; l.incarnation = m.incarnation; l.state = session::MemberLatency::State::Value; l.rttMs = 20 + i * 13 % 190; view.memberLatency[m.id] = l; }
        view.room.members.push_back(m);
    }
    const char* chat[] = {"Welcome! Grab a table or join a queue.", "Good games. I'll watch the next one.", "Ready for another set?", "Let's run it back!"};
    for (int i = 0; i < 12; ++i) view.room.chat.push_back({static_cast<std::uint64_t>(i + 1), static_cast<room::MemberId>(i % 16 + 1), chat[i % 4]});
}
// A trusted Ember ID and a relay in Europe; PublicRooms lists rooms once asked.
void PublicLab(ui::ShellView& view) {
    auto& id = view.identity;
    id.known = true; id.state = "ready"; id.emberId = "emb1_lab"; id.bridges = {{"brg_1", "https://bridge.example", "Example"}};
    view.netReport.reported = view.netReport.relayConnected = view.netReport.udp = true; view.netReport.relay = "euc1"; view.netReport.nat = NatClass::Open;
}
void PublicRooms(ui::ShellView& view) {
    const auto room = [](const char* id, const char* name, const char* host, const char* region, unsigned members, unsigned capacity, unsigned playing) {
        netplay::publicrooms::Room r; r.id = id; r.name = name; r.hostName = host; r.region = region; r.members = members; r.capacity = capacity; r.playing = playing;
        r.hasDetails = true; r.setFormat = 2; r.rotation = 0; r.createdAt = 100;
        for (unsigned i = 0; i < members; ++i) r.fighters.push_back(static_cast<int>(i * 5 % 44));
        return r;
    };
    auto& list = view.publicRooms;
    list.bridge = "brg_1"; list.loading = false; list.listedAt = 1300; ++list.listed;
    list.rooms = {room("a", "EU Monday Night Fights", "Katie", "euc1", 6, 8, 2), room("b", "Ultra only, AE2012 welcome", "Daigo-fan", "euc1", 3, 4, 1),
        room("c", "Beginners, ask anything", "Sven", "euc1", 2, 12, 0), room("d", "East coast casuals", "Marn", "use1", 8, 8, 3), room("e", "Tokyo late night", "Kaz", "aps1", 5, 10, 2)};
}
// A BluMint match's room: table 0 bound to the match, FRaccie (this PC) against
// Akira, first to 2. A watcher sits at no seat (member 5).
void BluMintRoom(ui::ShellView& view, bool watcher) {
    JoinRoom(view);
    auto& t = view.tournament;
    t.phase = netplay::tournament::Phase::InRoom; t.bridgeId = "brg_lab"; t.matchId = "emt_lab";
    netplay::tournament::Assignment a;
    a.matchId = "emt_lab"; a.provider = "blumint"; a.state = "ready"; a.gamesToWin = 2; a.profile = "ember-room-v1";
    t.list.items.push_back(a);
    view.room.name = "Good Fighters USF4 · Ember";
    view.room.tournament.matchId = "emt_lab"; view.room.tournament.gamesToWin = 2;
    view.room.tables[0].phase = room::TablePhase::Playing;
    if (watcher) view.room.localMember = 5;
}
// What happens next in it, from frame 40 (lab --moment): a game, the set won or
// lost, a game cut off, the host's close, the match's time run out, the match
// stopped, the next match called.
void BluMintMoment(ui::ShellView& view, const std::string& moment) {
    auto& table = view.room.tables[0];
    auto& a = view.tournament.list.items[0];
    if (moment == "game") table.score[0] = 1;
    else if (moment == "set" || moment == "lost") {
        const unsigned you = moment == "set" ? 2 : 1, them = moment == "set" ? 1 : 2;
        table.score[0] = you; table.score[1] = them; a.wins = {you, them}; a.state = "completed";
        table.phase = room::TablePhase::Waiting;
    }
    else if (moment == "restart") { table.score[0] = 1; view.tournament.reason = netplay::tournament::ConnectionLostReason; }
    else if (moment == "host") a.state = "cancelled";
    else if (moment == "expired") a.state = "expired";
    else if (moment == "failed") { view.tournament.phase = netplay::tournament::Phase::Failed; view.tournament.reason = "report_not_saved"; }
    // The next BluMint match called while the player sits in another room (king of the hill's queue).
    else if (moment == "called") {
        auto next = a; next.matchId = "emt_lab2";
        view.tournament = {}; view.room.tournament = {};
        view.tournament.list.items.push_back(next);
    }
}
// An Ember ID on a trusted service (lab --rankings), and Rankings' answers from
// it: FRaccie at B+ and listed, and a board of sixteen by class, as the design board.
void Rankings(ui::ShellView& view) {
    auto& id = view.identity;
    id.known = true; id.state = "ready"; id.emberId = "emb1_lab"; id.fingerprint = "lab";
    id.bridges = {{"brg_1", "https://bridge.example", "Example"}};
    auto& r = view.rankings;
    r.bridge = "brg_1"; r.emberId = id.emberId;
    auto& s = r.standing.value;
    s.kind = netplay::rankings::Standing::Kind::Ranked; s.classesShown = true; s.badge = SkillBadge::OfClass(8, true);
    s.listed = true; s.listing = {"FRaccie", CountryCode::Parsed("BE")};
    r.standing.loaded = true; r.standing.revision = r.standing.finished = 1;
    const std::tuple<const char*, const char*, int> players[] = {{"Shoryu_Ken","JP",10},{"Vulpecula","FR",10},{"MrPalm","US",9},{"Kaido","KR",9},
        {"TigerKnee","GB",9},{"NightOwl","MX",8},{"Rekka","BR",8},{"Pudding","NL",8},{"Gief4Life","PL",7},{"Tandem","DE",7},{"Ochre","CA",6},
        {"FRaccie","BE",6},{"Sonic Hurricane","ES",6},{"Aegis","IT",6},{"Bunbun","SE",5},{"Ninety9","US",5}};
    auto board = std::make_shared<std::vector<netplay::rankings::BoardEntry>>();
    for (const auto& p : players)
        board->push_back({{std::get<0>(p), CountryCode::Parsed(std::get<1>(p))}, static_cast<std::uint8_t>(std::get<2>(p)), std::string(std::get<0>(p)) == "FRaccie"});
    r.board.value = board; r.board.loaded = true; r.board.revision = r.board.finished = 1;
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) { std::wstring a = wide[i]; args.emplace_back(a.begin(), a.end()); }
    int width = 1280, height = 800;
    float textSize = 1;
    bool v2 = false, tv = false;  // --tv: TV mode
    bool welcome = false, pad = false;  // --welcome: a fresh install's first run; --pad: the pad was used last (the on-screen keyboard)
    std::string shot, keys, screen;
    bool inRoom = false, pip = false;
    int hud = -1;
    bool trainingPanel = false, blumint = false, watcher = false, update = false, publicLab = false, rankings = false, playback = false, toasts = false;  // --toasts: three notices, one a frame  // --update: Home offers a Nightly update
    std::string moment;  // --moment game|set|lost|restart|host|expired|failed|called, in a --blumint room  // --training: the training controls over the art  // --hud <layout>: the match HUD (0 Ember strip, 1 split) over the art instead of the menus
    for (std::size_t i = 0; i < args.size(); ++i) {
        const bool more = i + 1 < args.size();
        if (args[i] == "--size" && more) std::sscanf(args[++i].c_str(), "%dx%d", &width, &height);
        else if (args[i] == "--scale" && more) textSize = static_cast<float>(std::atof(args[++i].c_str()));
        else if (args[i] == "--v2") v2 = true;
        else if (args[i] == "--tv") tv = true;
        else if (args[i] == "--keys" && more) keys = args[++i];
        else if (args[i] == "--shot" && more) shot = args[++i];
        else if (args[i] == "--room") inRoom = true;
        else if (args[i] == "--pip") pip = true;
        else if (args[i] == "--hud" && more) hud = std::atoi(args[++i].c_str());
        else if (args[i] == "--training") trainingPanel = true;
        else if (args[i] == "--blumint") blumint = true;
        else if (args[i] == "--public") publicLab = true;
        else if (args[i] == "--watcher") watcher = true;
        else if (args[i] == "--rankings") rankings = true;
        else if (args[i] == "--playback") playback = true;
        else if (args[i] == "--moment" && more) moment = args[++i];
        else if (args[i] == "--screen" && more) screen = args[++i];
        else if (args[i] == "--update") update = true;
        else if (args[i] == "--toasts") toasts = true;
        else if (args[i] == "--welcome") welcome = true;
        else if (args[i] == "--pad") pad = true;
    }
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WindowProc, 0, 0, instance, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"EmberUiLab", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Ember UI lab", WS_OVERLAPPEDWINDOW, 40, 40, width, height, nullptr, nullptr, instance, nullptr);
    ResizeWindow(hwnd, width, height);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    present.Windowed = TRUE; present.SwapEffect = D3DSWAPEFFECT_DISCARD; present.BackBufferFormat = D3DFMT_A8R8G8B8;
    present.BackBufferWidth = width; present.BackBufferHeight = height; present.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &device))) return 1;
    ShowWindow(hwnd, shot.empty() ? SW_SHOWDEFAULT : SW_SHOWNOACTIVATE);

    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(device);
    // The repository's art: menu backdrop, emblem and prompts. Portraits need the game's files (SF4E_GAME_DIR).
    const char* game = std::getenv("SF4E_GAME_DIR");
    const std::string root = game ? game : "";
    ui::SelectionArt art(device, std::wstring(root.begin(), root.end()), L"" SF4E_LAB_ASSET_ROOT);
    ui::SetMenuArt(&art);

    ui::ApplicationShell shell;
    ui::ShellView view = FakeView(v2);
    view.preferences.tvMode = tv;
    view.preferences.welcomed = !welcome;
    // A BluMint room joins its own (BluMintRoom).
    if (inRoom && !blumint) JoinRoom(view);
    if (blumint) BluMintRoom(view, watcher);
    if (update) { view.services.launchOffer = launcher::InstallOffer{updates::UpdateChannel::Nightly, "v1.3.0", "lab"}; view.services.installedVersion = "v1.2.0"; }
    if (publicLab) PublicLab(view);
    // The helper's side for --public: each Ember ID request answered, the room list on asking.
    std::uint64_t identityAsked = 0;
    bool listAsked = false;
    if (rankings) Rankings(view);
    // Fighter select (--screen selection) with every fighter, costume and colour there.
    ui::FighterSelector selector;
    selection::Pick pick;
    selection::Availability available;
    available.ready = true; available.costumes = 5; available.colors[0] = available.colors[1] = 10; available.personalActions = 1;
    bool open = true, bar = shot.empty();
    std::size_t size = 0;
    std::vector<unsigned> script;
    { std::stringstream list(keys); std::string key; while (std::getline(list, key, ',')) script.push_back(KeyBit(key)); }
    const auto submit = [&](ui::ShellAction action) {
        // Saving is instant here: the shell reads what it saved back from the view.
        if (action.command.kind == netplay::CommandKind::SavePreferences) view.preferences = action.preferences;
        if (publicLab && action.identity.op != netplay::IdentityOp::None) identityAsked = action.identity.ticket;
        if (publicLab && action.tournament.op == netplay::tournament::Command::Op::RoomList) listAsked = true;
        // The helper answers each Ember ID request at once.
        if (action.identity.op != netplay::IdentityOp::None) { view.identityTicket = action.identity.ticket; view.identityRequest = view.identity.requestId = view.identityTicket + 100; view.identity.ok = true; }
        return true;
    };
    int frame = 0, momentAt = 40;
    for (MSG msg{}; msg.message != WM_QUIT;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (msg.message == WM_QUIT) break;
        if (resizeW && resizeH) { Reset(resizeW, resizeH); width = resizeW; height = resizeH; resizeW = resizeH = 0; }
        // The V2 scale rule, max(DPI, height / 1080), which the picture-scale branch brings to the game.
        const bool tvScale = ui::v2::On() && view.preferences.tvMode;
        if (ui::ApplyTheme((std::max)(ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd), height / 1080.f), textSize * (tvScale ? 1.25f : 1.f), ui::v2::On())) ImGui_ImplDX9_InvalidateDeviceObjects();
        art.Pump();
        ImGui_ImplDX9_NewFrame(); ImGui_ImplWin32_NewFrame();
        // Scripted shots keep the real mouse out of it: hovering a card focuses it.
        if (!shot.empty()) ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        ImGui::NewFrame();
        // A scripted key is held for 3 frames and released for 3, from frame 30 (a new screen ignores its first frames).
        unsigned held = PadButtons();
        const int scripted = frame - 30;
        const std::size_t step = scripted < 0 ? script.size() : static_cast<std::size_t>(scripted / 6);
        if (step < script.size() && scripted % 6 < 3) held |= script[step];
        ui::SetMenuInput({held, ImGui::GetTime()});
        ui::SetMenuGlyphs(pad || PadButtons() ? input::PadXInput : input::PadKeyboard, input::MenuButtons{});
        if (ImGui::IsKeyPressed(ImGuiKey_F1)) bar = !bar;
        if (ImGui::IsKeyPressed(ImGuiKey_F2)) { size = (size + 1) % std::size(Sizes); ResizeWindow(hwnd, Sizes[size].w, Sizes[size].h); }
        if (ImGui::IsKeyPressed(ImGuiKey_F3)) view.preferences.lookV2 = !view.preferences.lookV2;
        if (ImGui::IsKeyPressed(ImGuiKey_F4)) textSize = textSize >= 1.5f ? 1 : textSize + .25f;
        if (ImGui::IsKeyPressed(ImGuiKey_F5)) { const bool look = view.preferences.lookV2; inRoom = !inRoom; view = FakeView(look); if (inRoom) JoinRoom(view); }
        if (frame == 2 && !screen.empty()) shell.Navigation().Push(screen);
        static const std::pair<const char*, ui::Tone> said[] = {{"room.short_invitation_copied", ui::Tone::Success},
            {"tournament.link_waiting", ui::Tone::Pending}, {"notice.left_room", ui::Tone::Neutral}};
        if (toasts && frame >= 3 && frame < 6) shell.Say(loc::T(said[frame - 3].first), said[frame - 3].second, 600);
        if (identityAsked && identityAsked != view.identityTicket) {
            view.identityTicket = identityAsked; view.identityRequest = view.identity.requestId = identityAsked + 100; view.identity.ok = true;
        }
        if (listAsked) { listAsked = false; PublicRooms(view); }
        // F7 and F8 set the room up again, and the moment follows a few frames later so the shell sees it happen.
        static const char* const moments[] = {"", "game", "set", "lost", "restart", "host", "expired", "failed", "called"};
        const bool nextMoment = blumint && ImGui::IsKeyPressed(ImGuiKey_F7), swapRole = blumint && ImGui::IsKeyPressed(ImGuiKey_F8);
        if (nextMoment || swapRole) {
            if (nextMoment) { const auto at = std::find(std::begin(moments), std::end(moments), moment); moment = moments[at == std::end(moments) || at + 1 == std::end(moments) ? 0 : at - std::begin(moments) + 1]; }
            if (swapRole) watcher = !watcher;
            const bool look = view.preferences.lookV2; view = FakeView(look); JoinRoom(view); BluMintRoom(view, watcher); momentAt = frame + 10;
        }
        if (frame == momentAt && blumint && !moment.empty()) BluMintMoment(view, moment);
        const bool snap = ImGui::IsKeyPressed(ImGuiKey_F12);
        // The menu art stands in for a watched game's picture (F6).
        if (ImGui::IsKeyPressed(ImGuiKey_F6)) pip = !pip;
        view.preferences.watchedPicture = pip;
        view.watchedPicture = pip ? art.MenuBackdrop().texture : ImTextureID{};
        if (trainingPanel) {
            const auto* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(vp->Pos); ImGui::SetNextWindowSize(vp->Size);
            ImGui::Begin("Lab game", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBringToFrontOnFocus);
            ui::DrawMenuBackdrop(false);
            ImGui::End();
            ui::v2::SetOn(view.preferences.lookV2);
            // A Training battle with two recordings, as the render test's.
            training::View lab;
            lab.available = lab.ready = lab.checkpoint = true; lab.generation = 1;
            lab.lengths[0] = 120; lab.lengths[2] = 45; lab.on[0] = true; lab.playable = lab.replyable = 1u;
            // The dummy crouching and blocking everything, as the game reports it.
            lab.dummy.action = 1; lab.dummy.guard = 2; lab.dummy.counterHit = 0; lab.dummy.quickStand = 1; lab.dummy.super = 0; lab.dummy.revenge = 0;
            ui::DrawTrainingFlyout(lab, [](const training::Command&) { return true; });
        } else if (playback) {
            ui::v2::SetOn(view.preferences.lookV2);
            // The board's replay: paused at half speed in round 2, the lanes on.
            namespace in = replayinputs;
            ui::ReplayHudView replay;
            replay.shown = replay.armed = replay.paused = replay.inputsKnown = replay.lanes = true;
            replay.divisor = 2; replay.round = 1; replay.cursor = 1482; replay.stripAlpha = 1; replay.pad = pad;
            const replaylane::Row rows[] = {{in::Right | in::MK, in::MK, 3}, {in::Down | in::Right, 0, 2}, {in::Down | in::HP, in::HP, 1},
                {in::Left, 0, 14}, {0, 0, 6}, {in::Right | in::LP, in::LP, 2}, {in::Down, 0, 1}, {in::Down | in::Left | in::HK | in::LK, in::HK, 4}};
            for (int side = 0; side < 2; side++) { std::copy(std::begin(rows), std::end(rows), replay.rows[side]); replay.rowCount[side] = 8; }
            ui::DrawReplayStrip(replay);
            ui::DrawReplayLanes(replay);
            // After the HUD: a window that never comes to the front is put behind those made before it.
            const auto* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(vp->Pos); ImGui::SetNextWindowSize(vp->Size);
            ImGui::Begin("Lab game", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBringToFrontOnFocus);
            ui::DrawMenuBackdrop(view.preferences.lookV2);
            ImGui::End();
        } else if (hud >= 0) {
            // A fight's HUD: the art stands in for the game behind it.
            const auto* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(vp->Pos); ImGui::SetNextWindowSize(vp->Size);
            ImGui::Begin("Lab game", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBringToFrontOnFocus);
            ui::DrawMenuBackdrop(view.preferences.lookV2);
            ImGui::End();
            ui::v2::SetOn(view.preferences.lookV2);
            ui::MatchStripView strip;
            strip.names[0] = "FRaccie"; strip.names[1] = "Akira";
            strip.links[0] = NetworkLink::Wired; strip.links[1] = NetworkLink::Wireless;
            strip.pingMs = 96; strip.rollbackFrames = 2; strip.appliedDelay = 2; strip.layout = hud; strip.spectators = 3; strip.score = "2 - 1";
            ui::DrawMatchStrip(strip);
            if (toasts) shell.Background(view, view.room, submit);
        } else shell.Draw(view, &open, submit, [&] { selector.Draw(pick, false, &art, [&](int) { return available; }); });
        open = true;
        if (bar) {
            ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always, ImVec2(0, 0));
            ImGui::SetNextWindowBgAlpha(.85f);
            ImGui::Begin("Lab", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
            ImGui::Text("%dx%d  text %.2f  %s", width, height, textSize, view.preferences.lookV2 ? "V2" : "Classic");
            ImGui::TextDisabled("F1 bar  F2 size  F3 look  F4 text  F5 room  F6 picture  F12 shot");
            ImGui::End();
        }
        ImGui::EndFrame();
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(20, 19, 18), 1.f, 0);
        device->BeginScene(); ImGui::Render(); ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData()); device->EndScene();
        ++frame;
        if (snap) { char name[64]; std::snprintf(name, sizeof(name), "uilab-%d.bmp", frame); SaveShot(name); }
        // Scripted runs give the art time to load and the last key time to settle.
        if (!shot.empty() && frame > static_cast<int>(script.size()) * 6 + 60 && art.MenuBackdrop().texture) { SaveShot(shot); break; }
        device->Present(nullptr, nullptr, nullptr, nullptr);
    }
    ImGui_ImplDX9_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    device->Release(); d3d->Release();
    DestroyWindow(hwnd);
    return 0;
}
