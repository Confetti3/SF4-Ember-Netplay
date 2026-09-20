// Render the real UI into a hidden DX9 surface; no game or network is started.
#include "../ui/ApplicationShell.hxx"
#include "../ui/Theme.hxx"
#include "../ui/TrainingPanel.hxx"
#include "../ui/FighterSelector.hxx"
#include "../ui/RecoveryMenu.hxx"
#include "../common/Localization.hxx"
#include "../common/StageCatalog.hxx"
#include <imgui_internal.h>
#include <imgui_impl_dx9.h>
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>
#include <chrono>
#include <memory>
#include <thread>

namespace {
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Renderer {
    HWND window = nullptr;
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* device = nullptr;
    Renderer() {
        window = CreateWindowA("STATIC", "SF4 UI rendering check", WS_OVERLAPPEDWINDOW,
            0, 0, 2560, 1440, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
        Require(window != nullptr, "Hidden window creation failed");
        d3d = Direct3DCreate9(D3D_SDK_VERSION);
        Require(d3d != nullptr, "Direct3D9 unavailable");
        D3DPRESENT_PARAMETERS p{};
        p.Windowed = TRUE; p.SwapEffect = D3DSWAPEFFECT_DISCARD;
        p.BackBufferFormat = D3DFMT_A8R8G8B8; p.BackBufferWidth = 2560; p.BackBufferHeight = 1440;
        p.hDeviceWindow = window;
        Require(SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &p, &device)), "DX9 device creation failed");
    }
    ~Renderer() { if (device) device->Release(); if (d3d) d3d->Release(); if (window) DestroyWindow(window); }
    void Resize(int width, int height) {
        D3DPRESENT_PARAMETERS params{};
        params.Windowed = TRUE; params.SwapEffect = D3DSWAPEFFECT_DISCARD;
        params.BackBufferFormat = D3DFMT_A8R8G8B8;
        params.BackBufferWidth = width; params.BackBufferHeight = height; params.hDeviceWindow = window;
        Require(SUCCEEDED(device->Reset(&params)), "DX9 device reset failed");
    }
    void Draw() {
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(45, 47, 49), 1.f, 0);
        Require(SUCCEEDED(device->BeginScene()), "BeginScene failed");
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        device->EndScene();
    }
    void Capture(const std::string& name, int width, int height) {
        IDirect3DSurface9 *target = nullptr, *copy = nullptr;
        Require(SUCCEEDED(device->GetRenderTarget(0, &target)), "GetRenderTarget failed");
        HRESULT result = device->CreateOffscreenPlainSurface(width, height, D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM, &copy, nullptr);
        if (SUCCEEDED(result)) result = device->GetRenderTargetData(target, copy);
        target->Release();
        Require(SUCCEEDED(result), "Readback failed");
        D3DLOCKED_RECT pixels{};
        Require(SUCCEEDED(copy->LockRect(&pixels, nullptr, D3DLOCK_READONLY)), "Surface lock failed");
        if(name.find("training-")!=std::string::npos) {
            const auto corner=*reinterpret_cast<const unsigned*>(static_cast<const char*>(pixels.pBits)+5*pixels.Pitch+5*4);
            Require((corner&0xffffff)==0x2d2f31,"Training rendering altered the game outside its panel");
        }
        BITMAPFILEHEADER file{}; BITMAPINFOHEADER info{};
        file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(info);
        file.bfSize = file.bfOffBits + width * height * 4;
        info.biSize = sizeof(info); info.biWidth = width; info.biHeight = -height;
        info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
        std::ofstream out(name, std::ios::binary);
        out.write(reinterpret_cast<const char*>(&file), sizeof(file));
        out.write(reinterpret_cast<const char*>(&info), sizeof(info));
        for (int y = 0; y < height; ++y)
            out.write(static_cast<const char*>(pixels.pBits) + y * pixels.Pitch, width * 4);
        copy->UnlockRect(); copy->Release();
        Require(out.good(), "Screenshot write failed");
    }
};
ImGuiWindow* FindWindow(const char* fragment) {
    for (auto* window : GImGui->Windows)
        if (window->Active && std::string(window->Name).find(fragment) != std::string::npos) return window;
    throw std::runtime_error(std::string("Missing UI window: ") + fragment);
}
void Activate(const char* windowFragment, const char* label) {
    ImGui::ActivateItemByID(FindWindow(windowFragment)->GetID(label));
}
void CheckStacks() {
    Require(GImGui->CurrentWindowStack.Size == 1, "Unbalanced window stack");
    Require(GImGui->ColorStack.Size == 0 && GImGui->StyleVarStack.Size == 0 &&
            GImGui->FontStack.Size == 0 && GImGui->DisabledStackSize == 0, "Unbalanced style stack");
    Require(GImGui->CurrentTable == nullptr, "Unbalanced table");
    Require(GImGui->ErrorCountCurrentFrame == 0, "ImGui reported a rendering error");
}
}

int main(int argc, char** argv) {
    try {
        Renderer renderer;
        const std::string output = argc > 1 ? argv[1] : "";
        const bool trainingShotsOnly = argc > 2 && std::string(argv[2]) == "--training-shots-only";
        const bool recoveryShotsOnly = argc > 2 && std::string(argv[2]) == "--recovery-shots-only";
        const bool matchShotsOnly = argc > 2 && std::string(argv[2]) == "--match-shots-only";
        const bool uxShotsOnly = argc > 4 && std::string(argv[4]) == "--ux-shots-only";
        const bool readmeShots = argc > 4 && std::string(argv[4]) == "--readme-shots";
        std::unique_ptr<sf4e::ui::SelectionArt> art;
        if (argc > 2 && !trainingShotsOnly && !recoveryShotsOnly && !matchShotsOnly) {
            const std::string root = argv[2];
            const std::string assets = argc > 3 ? argv[3] : "assets/selection";
            art.reset(new sf4e::ui::SelectionArt(renderer.device, std::wstring(root.begin(), root.end()), std::wstring(assets.begin(), assets.end())));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            int loaded = 0;
            do {
                art->Pump(); loaded = 0;
                for (int id = 0; id < sf4e::selection::FighterCount; ++id) {
                    const auto portrait = art->Portrait(id);
                    if (portrait.missing) throw std::runtime_error(std::string("Native portrait decode failed: ") + sf4e::selection::FindFighter(id)->code);
                    if (portrait.texture) ++loaded;
                }
                if (loaded == sf4e::selection::FighterCount) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } while (std::chrono::steady_clock::now() < deadline);
            Require(loaded == sf4e::selection::FighterCount, "Native portrait loading timed out");
            std::printf("All 44 native portraits decoded and uploaded to DX9.\n");
            // Exercise the real file decoder/upload path for every costume photo.
            // Also settle these asynchronous loads before taking UI captures.
            const auto costumeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            int expected = 0;
            for (int id = 0; id < sf4e::selection::FighterCount; ++id)
                expected += sf4e::selection::CostumeCount(id) - 1;
            do {
                art->Pump(); loaded = 0;
                for (int id = 0; id < sf4e::selection::FighterCount; ++id)
                    for (int costume = 1; costume < sf4e::selection::CostumeCount(id); ++costume) {
                        const auto photo = art->Appearance(id, costume, 0);
                        if (photo.missing) throw std::runtime_error(std::string("Costume decode failed: ") +
                            sf4e::selection::FindFighter(id)->code + "/" + std::to_string(costume));
                        if (photo.texture) ++loaded;
                    }
                if (loaded == expected) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } while (std::chrono::steady_clock::now() < costumeDeadline);
            Require(loaded == expected, "Costume photo loading timed out");
            std::printf("All %d alternate costume photos decoded and uploaded to DX9.\n", expected);
            const auto stageDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            do {
                art->Pump(); loaded = 0;
                for (const auto& stage : sf4e::selection::StageList()) {
                    const auto photo = art->Stage(stage.id);
                    if (photo.missing) throw std::runtime_error(std::string("Stage photo decode failed: ") + stage.code);
                    if (photo.texture) {
                        Require(photo.width * 9 == photo.height * 16, "Stage photo aspect ratio changed");
                        ++loaded;
                    }
                }
                if (loaded == sf4e::selection::VersusStageCount) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } while (std::chrono::steady_clock::now() < stageDeadline);
            Require(loaded == sf4e::selection::VersusStageCount, "Stage photo loading timed out");
            std::printf("All 28 stage photos decoded and uploaded to DX9.\n");
            std::vector<std::pair<int, int>> ultraPhotos;
            for (int id = 0; id < sf4e::selection::FighterCount; ++id) for (int ultra = 0; ultra < 2; ++ultra) {
                const std::string path = assets + "/" + sf4e::selection::FindFighter(id)->code + "/ultra-" + std::to_string(ultra) + ".png";
                const bool present = std::ifstream(path, std::ios::binary).good();
                // The archived 2010 guides cover both Ultras for every SSFIV fighter.
                if (sf4e::selection::EditionAllowed(id, 1, true)) Require(present, "Missing archived SSFIV Ultra photo");
                if (present) ultraPhotos.emplace_back(id, ultra);
            }
            const auto ultraDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            do {
                art->Pump(); loaded = 0;
                for (const auto& option : ultraPhotos) {
                    const auto photo = art->Ultra(option.first, option.second);
                    Require(!photo.missing, "Ultra photo decode failed");
                    if (photo.texture) {
                        Require(photo.width == 256 && photo.height == 144, "Ultra photo framing changed");
                        ++loaded;
                    }
                }
                if (loaded == static_cast<int>(ultraPhotos.size())) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } while (std::chrono::steady_clock::now() < ultraDeadline);
            Require(loaded == static_cast<int>(ultraPhotos.size()), "Ultra photo loading timed out");
            std::printf("All %d supplied Ultra photos decoded and uploaded to DX9.\n", loaded);
            // Load numbered color assets in bounded groups, allowing the real
            // texture cache to evict previous groups as it does during browsing.
            struct ColorPhoto { int fighter, costume, color; };
            std::vector<ColorPhoto> colorPhotos;
            for (int id = 0; id < sf4e::selection::FighterCount; ++id)
                for (int costume = 0; costume < sf4e::selection::CostumeCount(id); ++costume)
                    for (int color = 0; color < sf4e::selection::ColorCount(id, costume); ++color) {
                        const std::string stem = assets + "/" + sf4e::selection::FindFighter(id)->code +
                            "/costume-" + std::to_string(costume) + "/color-" + std::to_string(color);
                        if (GetFileAttributesA((stem + ".png").c_str()) == INVALID_FILE_ATTRIBUTES) continue;
                        Require(GetFileAttributesA((stem + "-cutout.png").c_str()) != INVALID_FILE_ATTRIBUTES,
                                "Numbered color photograph has not been masked");
                        colorPhotos.push_back({id, costume, color});
                    }
            for (std::size_t first = 0; first < colorPhotos.size(); first += 32) {
                const std::size_t end = (std::min)(first + 32, colorPhotos.size());
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                do {
                    art->Pump(); loaded = 0;
                    for (std::size_t i = first; i < end; ++i) {
                        const auto& key = colorPhotos[i];
                        const auto photo = art->Appearance(key.fighter, key.costume, key.color);
                        Require(!photo.missing, "Numbered color decode failed");
                        if (photo.texture) ++loaded;
                    }
                    if (loaded == static_cast<int>(end - first)) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                } while (std::chrono::steady_clock::now() < deadline);
                Require(loaded == static_cast<int>(end - first), "Numbered color loading timed out");
            }
            std::printf("All %d supplied numbered color cutouts decoded and uploaded to DX9.\n", static_cast<int>(colorPhotos.size()));
            const auto originalDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            bool originalLoaded = false;
            do {
                art->Pump();
                originalLoaded = art->Appearance(0, 0, 0).texture != 0;
                if (originalLoaded) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } while (std::chrono::steady_clock::now() < originalDeadline);
            Require(originalLoaded, "Original outfit preview failed");
        }
        if(!art)art.reset(new sf4e::ui::SelectionArt(renderer.device,L"",L"" SF4E_TEST_ASSET_ROOT));
        sf4e::ui::SetMenuArt(art.get());
        const auto brandDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!art->MenuBackdrop().texture&&std::chrono::steady_clock::now()<brandDeadline){
            art->Pump();std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Require(art->MenuBackdrop().texture!=0,"Ember background failed to load");
        const char* promptAssets[]={"xbox_button_color_a","xbox_button_color_b","xbox_button_color_x","xbox_button_color_y",
            "xbox_lb","xbox_rb","xbox_lt","xbox_rt","xbox_dpad","xbox_dpad_horizontal","keyboard_enter","keyboard_escape",
            "keyboard_arrows_all","keyboard_arrows_horizontal","generic_button_circle_fill","xbox_button_back","xbox_button_start"};
        for(const auto* prompt:promptAssets){
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while(!art->InputPrompt(prompt).texture&&std::chrono::steady_clock::now()<deadline){art->Pump();std::this_thread::sleep_for(std::chrono::milliseconds(5));}
            Require(art->InputPrompt(prompt).texture!=0,"Packaged Kenney prompt failed to load");
        }
        struct Size { int w, h; float dpi; };
        const Size sizes[] = {{1280,720,1}, {1920,1080,1}, {1920,1080,1.25f}, {1920,1080,1.5f}, {2560,1440,1.5f}, {640,720,1.5f}, {3440,1440,1}, {3840,2160,1}, {3840,2160,1.5f}, {1280,720,2}};
        int frames = 0;
        sf4e::ui::SetMenuTextProbe([](const char* id,float text,float interior,float width,float available){
            if(text>interior+.5f)throw std::runtime_error(std::string("Menu text exceeds padded row: ")+id+" text="+std::to_string(text)+" interior="+std::to_string(interior));
            if(width>available+.5f)throw std::runtime_error(std::string("Menu label overflows horizontally: ")+id);
        });

        struct LocalePass { const char* name; int locale; };
        const LocalePass localePasses[]={{"en",0},{"pt-BR",1},{"es-419",2},{"pseudo",3}};
        for(const auto localePass:localePasses) {
          if(localePass.locale==0)sf4e::loc::SetActive(sf4e::loc::Locale::En);
          else if(localePass.locale==1)sf4e::loc::SetActive(sf4e::loc::Locale::PtBR);
          else if(localePass.locale==2)sf4e::loc::SetActive(sf4e::loc::Locale::Es419);
          else sf4e::loc::testing::SetPseudoActive();
        for(const auto size:sizes) {
            using namespace sf4e;using namespace ui;
            std::printf("UI viewport %dx%d at %.0f%% DPI\n",size.w,size.h,size.dpi*100);std::fflush(stdout);
            renderer.Resize(size.w,size.h);ImGui::CreateContext();
            auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize=ImVec2(static_cast<float>(size.w),static_cast<float>(size.h));io.DeltaTime=1.f/60;
            ApplyTheme(size.dpi);ImGui_ImplDX9_Init(renderer.device);
            ApplicationShell shell;ShellView view;bool open=true;
            view.controllerReady=view.canChangeController=view.canEditPreferences=view.canEditSelection=view.canOpenRoom=view.helperReady=true;
            view.controller="Assigned controller";view.preferences.displayName="Ember Player";view.selectionSummary="Ryu / Original / Color 01 / Ultra I";
            FighterSelector selector;selection::Pick pick;int stage=0;
            auto availability=[](int id){selection::Availability a;a.ready=true;a.personalActions=0x3ff;
                for(int c=0;c<selection::CostumeCount(id);++c){a.costumes|=1u<<c;a.colors[c]=(1u<<selection::ColorCount(id,c))-1;}return a;};
            training::View training;training.available=training.ready=training.checkpoint=true;training.generation=static_cast<unsigned>(size.w+size.dpi*100);
            training.lengths[0]=120;
            training::FrameMeter meter;std::array<training::FighterSample,2> fighters;
            for(int f=0;f<120;++f){
                for(int p=0;p<2;++p){auto& s=fighters[p];s.valid=true;s.timeScale=1;s.status=p?(f<40?0:f<65?22:0):(f<30?0:f<70?16:0);
                    s.action=s.status?100+p:0;s.actionFrame=static_cast<float>(f);s.firstActiveFrame=p?-1:34;s.health=p?920:1000;s.damage=p?80:0;s.comboDamage=p?160:0;}
                meter.Observe(f,fighters);
            }
            training.meter=meter.View();training.history[0]={{0x14,5},{1,3},{0,16}};training.history[1]={{0x40,2},{0,10}};
            int mode=0;
            MatchStripView matchStrip;matchStrip.names[0]="Player One";matchStrip.names[1]="Player Two";
            matchStrip.pingMs=68;matchStrip.rollbackFrames=2;matchStrip.appliedDelay=3;
            training::Command trainingCommand;
            bool acceptTraining=false;
            GameMenu recoveryMenu;recoveryMenu.navigation=MenuNavigation("close");
            platform::ServiceSnapshot recoveryState;bool recoveryUpdates=false;
            const auto draw=[&](const char* shot=nullptr,unsigned buttons=0,int settle=3){
                for(int i=0;i<settle;++i){
                    if(art)art->Pump();SetMenuInput({buttons,0});
                    if(mode==1){const ImGuiKey keys[]={ImGuiKey_UpArrow,ImGuiKey_DownArrow,ImGuiKey_LeftArrow,ImGuiKey_RightArrow,ImGuiKey_Enter,ImGuiKey_Escape};
                        for(unsigned key=0;key<6;++key)io.AddKeyEvent(keys[key],(buttons&(1u<<key))!=0);}
                    ImGui_ImplDX9_NewFrame();ImGui::NewFrame();
                    if(mode==0)shell.Draw(view,&open,[&](ShellAction a){
                        if(a.command.kind==netplay::CommandKind::SavePreferences&&view.settingsError.empty())view.preferences=a.preferences;
                        return true;},[&]{selector.Draw(pick,true,art.get(),availability,&stage,view.canEditSelection);});
                    else if(mode==1)DrawTrainingFlyout(training,[&](training::Command c){trainingCommand=c;return acceptTraining;});
                    else if(mode==2)DrawTrainingHud(training);
                    else if(mode==5)DrawControllerWarning("Match input blocked: reconnect your controller. If its slot changed, return to the room to reassign it.");
                    else if(mode==4)DrawRecoveryMenu(recoveryMenu,recoveryState,"The selected folder does not contain SSFIV.exe. Choose the installed game folder or close recovery without starting SF4.",recoveryUpdates);
                    else DrawMatchStrip(matchStrip);
                    CheckStacks();ImGui::Render();renderer.Draw();++frames;
                    if(mode==3){
                        Require(!io.WantCaptureKeyboard&&!io.WantCaptureMouse,"Match HUD captured gameplay input");
                        const auto* list=ImGui::GetForegroundDrawList();
                        for(const auto& vertex:list->VtxBuffer)Require(vertex.pos.x>=size.w*.1f-2&&vertex.pos.x<=size.w*.9f+2&&
                            vertex.pos.y>=0&&vertex.pos.y<=size.h,"Match HUD escaped safe viewport bounds");
                    }
                    if((mode==0||mode==4)&&i==settle-1&&settle>=3){
                        const auto* root=FindWindow(mode==0?"EmberShell":"###EmberRecovery");
                        if(root->ScrollMax.y>=1)throw std::runtime_error(std::string("Player menu footer escaped on ")+(mode==0?shell.Navigation().Screen():"recovery")+" by "+std::to_string(root->ScrollMax.y)+" pixels");
                    }
                    if(mode==1){
                        auto* flyout=FindWindow("###TrainingControls");
                        Require(flyout->Size.x<=size.w*.8f+1&&flyout->Size.y<=size.h*.8f+1,"Training flyout covers too much game");
                        Require(std::abs(flyout->Pos.x*2+flyout->Size.x-size.w)<=2&&std::abs(flyout->Pos.y*2+flyout->Size.y-size.h)<=2,"Training flyout is not centered");
                        Require(flyout->ScrollMax.y<1,"Training footer displaced by overflowing content");
                        Require(ImGui::GetTopMostPopupModal()==nullptr,"Training confirmation dims the game viewport");
                        for(auto* window:GImGui->Windows){
                            if(window->LastFrameActive!=ImGui::GetFrameCount()||window->RootWindow!=flyout)continue;
                            Require(window->Pos.x>=flyout->Pos.x-1&&window->Pos.y>=flyout->Pos.y-1&&
                                window->Pos.x+window->Size.x<=flyout->Pos.x+flyout->Size.x+1&&
                                window->Pos.y+window->Size.y<=flyout->Pos.y+flyout->Size.y+1,"Training child escapes the flyout");
                        }
                    }
                    const bool recoveryShot=shot && (std::string(shot).find("table-delay-")==0 ||
                        std::string(shot).find("room-transfer-host")==0 ||
                        std::string(shot).find("table-terminal-pending")==0 ||
                        std::string(shot).find("table-recover")==0 || std::string(shot).find("table-replacement")==0);
                    const bool matchShot=shot&&mode==3&&(
                        (std::string(shot)=="match-hud"&&((size.w==1280&&size.h==720&&size.dpi==1)||
                         (size.w==1920&&size.h==1080&&size.dpi==1)||(size.w==2560&&size.h==1440)||
                         (size.w==3440&&size.h==1440)||(size.w==3840&&size.h==2160&&size.dpi==1)))||
                        (size.w==1920&&size.h==1080&&size.dpi==1&&
                         (std::string(shot)=="match-hud-size-0"||std::string(shot)=="match-hud-size-2"||
                          std::string(shot)=="match-hud-raised"||std::string(shot)=="match-hud-long"||
                          std::string(shot)=="match-hud-unavailable"||std::string(shot)=="match-hud-spectator"||
                          std::string(shot)=="match-hud-reset")));
                    if(i==settle-1&&shot&&!output.empty()&&(!trainingShotsOnly||mode==1||mode==2)&&
                        (!recoveryShotsOnly||recoveryShot)&&(!matchShotsOnly||matchShot) && (!uxShotsOnly ||
                            ((std::string(shot)=="table-delay-checking" || std::string(shot)=="table-delay-retry" ||
                              std::string(shot)=="table-recover-updating" || std::string(shot)=="table-recover-leaving") &&
                             ((size.w==1280&&size.dpi==1) || (size.w==1920&&size.dpi==1.5f) || size.w==640))))
                        renderer.Capture(output+shot+"-"+localePass.name+"-"+std::to_string(size.w)+"-"+std::to_string(static_cast<int>(size.dpi*100))+".bmp",size.w,size.h);
                }
            };
            const auto page=[&](const char* screen){shell.Navigation().Home();if(std::string(screen)!="home")shell.Navigation().Push(screen);draw(screen);};
            SetMenuGlyphs(3,0x40000,0x20000);
            // The launch card opens once for mismatched game settings. Right
            // moves to "Don't show again" and Back declines it, so this covers
            // the two-button notice without writing the real preference.
            view.showGameSettingsCard=true;
            view.gameSettings.frameRate="SMOOTH";view.gameSettings.vsync="ON";view.gameSettings.msaa="4X";
            draw("game-settings-card");
            Require(ImGui::GetTopMostPopupModal()!=nullptr,"Game settings card did not open");
            draw(nullptr,MenuInput::Right,1);draw();
            Require(ImGui::GetTopMostPopupModal()!=nullptr,"Choosing the alternative closed the card early");
            draw(nullptr,MenuInput::Back,1);draw();
            Require(ImGui::GetTopMostPopupModal()==nullptr,"Game settings card did not close");
            view.gameSettings={};view.showGameSettingsCard=false;
            for(const char* screen:{"home","profile","main-character","online","create","join","settings","player","defaults","interface","discord","about"})page(screen);
            page("home");
            for(int i=0;i<8;++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
            draw("home-last-row");
            // Real editor activation and cancellation through the same input snapshot.
            page("player");draw(nullptr,MenuInput::Select,1);draw("text-edit");draw(nullptr,MenuInput::Back,1);draw();
            Require(!shell.Navigation().Editing()&&shell.Navigation().Screen()=="player","Text Back escaped the screen");
            view.inputCapture=input::Capture::ReleaseAll;draw("controller-assignment");view.inputCapture=input::Capture::Idle;
            view.settingsError="The settings directory is temporarily unavailable.";page("settings");draw("save-error");
            page("main-character");
            for(int i=0;i<50&&shell.Navigation().Focus()!="retry-save";++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
            // Down preserves the grid column; the short final row may require
            // moving right to reach its last action.
            for(int i=0;i<8&&shell.Navigation().Focus()!="retry-save";++i){draw(nullptr,MenuInput::Right,1);draw(nullptr,0,1);}
            Require(shell.Navigation().Focus()=="retry-save","Portrait retry is unreachable");draw("portrait-save-error");
            view.settingsError.clear();shell=ApplicationShell{};
            page("selection");
            for(const char* screen:{"roster","appearance","costumes","colors","ultra","stage","options"}){
                selector.Navigation().Home();selector.Navigation().Push(screen);
                if(argc>2&&!trainingShotsOnly&&(std::string(screen)=="costumes"||std::string(screen)=="colors")){
                    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
                    bool loaded=false;
                    do{
                        art->Pump();loaded=true;
                        const auto a=availability(pick.fighter);
                        const auto options=std::string(screen)=="costumes"?selection::AllowedCostumes(pick.fighter,a):selection::AllowedColors(pick.fighter,pick.costume,a);
                        for(int option:options){
                            const auto img=std::string(screen)=="costumes"?art->Appearance(pick.fighter,option,0):art->Appearance(pick.fighter,pick.costume,option);
                            Require(!img.missing,"Gallery preview asset is missing");loaded=loaded&&img.texture!=0;
                        }
                        if(!loaded)std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }while(!loaded&&std::chrono::steady_clock::now()<deadline);
                    Require(loaded,"Gallery preview loading timed out");
                }
                const bool gallery=std::string(screen)=="costumes"||std::string(screen)=="colors";
                if(gallery)SetMenuCardProbe([](const char*,ImVec2 min,ImVec2 max){
                    Require(max.y-min.y<=ImGui::GetWindowHeight()+.5f,"Gallery card taller than the scrolling pane");
                    Require(max.y-min.y>=100,"Gallery artwork collapsed to an unreadable thumbnail");});
                draw(screen);SetMenuCardProbe({});
                if(std::string(screen)=="ultra"){
                    const int savedUltra=pick.ultra;
                    for(int ultra:{0,1,2}){
                        pick.ultra=ultra;draw(("ultra-saved-"+std::to_string(ultra)).c_str());
                    }
                    pick.ultra=savedUltra;
                }
            }
            selector.Navigation().Home();selector.Navigation().Push("roster");draw();
            const int saved=pick.fighter;draw(nullptr,MenuInput::Right,1);draw("fighter-focus");
            Require(pick.fighter==saved,"Grid movement committed fighter");
            draw(nullptr,MenuInput::Select,1);draw();Require(pick.fighter!=saved,"Select did not commit focused fighter");
            const int lockedFighter=pick.fighter;
            view.canEditSelection=false;draw(nullptr,MenuInput::Right,1);draw(nullptr,0,1);draw(nullptr,MenuInput::Select,1);
            Require(pick.fighter==lockedFighter,"Locked fighter changed");draw("fighter-locked");view.canEditSelection=true;
            view.session.room=netplay::RoomState::Joined;view.session.control=netplay::Health::Healthy;view.session.generation.room=1;
            view.room.roomEpoch=42;view.room.name="Friday Night Fights";view.room.capacity=16;view.room.host=view.room.localMember=1;
            view.canEditPreferences=false;view.canReady=true;view.localSlot=0;view.invitation="sf4://test-only";
            for(int i=0;i<4;++i){auto& t=view.room.tables[i];t.id=i;t.revision=1;t.phase=i==0?room::TablePhase::Waiting:i==1?room::TablePhase::Playing:room::TablePhase::Idle;
                if(i<2){t.p1=i*2+1;t.p2=i*2+2;}}
            const char* sampleNames[]={"Ember Player","Akira","Jamie","Alex","Morgan","Riley","Sam","Jordan","Casey","Taylor","Robin","Ash","Sky","Reese","Avery","Drew"};
            for(int i=1;i<=16;++i){room::Member m;m.id=i;m.name=readmeShots?sampleNames[i-1]:(i==2?"Long player name for layout test":"Member "+std::to_string(i));m.host=i==1;m.fighter=(i-1)*2;m.mainFighter=(i+7)%44;
                if(i<=4){m.table=(i-1)/2;m.seat=(i-1)%2;m.status=i<3?room::MemberStatus::Seated:room::MemberStatus::Playing;}
                view.room.members.push_back(m);}
            const char* sampleChat[]={"Welcome! Grab a table or join a queue.","Good games. I'll watch the next one.","Ready for another set?","Let's run it back!"};
            for(int i=0;i<20;++i)view.room.chat.push_back({static_cast<std::uint64_t>(i+1),static_cast<room::MemberId>(i%16+1),readmeShots?sampleChat[i%4]:"Ready for the next set? This is a longer chat message for narrow-layout inspection."});
            draw();for(const char* screen:{"room","room-table","room-rules","room-members","room-chat","room-admin"})page(screen);
            page("room-members");
            for(int i=0;i<24&&shell.Navigation().Focus()!="member-2";++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
            Require(shell.Navigation().Focus()=="member-2","Host-transfer recipient is unreachable at this viewport/DPI");
            draw(nullptr,MenuInput::Select,1);draw();
            Require(shell.Navigation().Screen()=="room-member","Member activation did not open its actions");
            for(int i=0;i<12&&shell.Navigation().Focus()!="transfer-host";++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
            Require(shell.Navigation().Focus()=="transfer-host","Host transfer is unreachable at this viewport/DPI");
            draw("room-transfer-host");
            draw(nullptr,MenuInput::Select,1);draw("room-transfer-host-confirm");
            Require(shell.Navigation().Confirming()&&!shell.Navigation().ConfirmSelected(),"Host transfer confirmation did not default to Cancel");
            draw(nullptr,MenuInput::Back,1);draw();
            page("room-table");view.room.tables[0].p2=0;view.canReady=false;draw("table-waiting-opponent");
            view.room.tables[0].p2=2;view.canReady=true;draw("table-ready-up");
            // Delay advice is a setup aid: the recommendation is read-only,
            // manual adjustment is bounded and immediate, and insufficient
            // probe data never blocks a manual Ready.
            view.recommendedDelay=4;view.selectedDelay=2;view.canProbe=true;view.canApplyDelay=true;
            view.probeStatus="complete";view.probeSamples=96;view.probeLost=4;draw("table-delay-recommended");
            view.probeStatus="checking";draw("table-delay-checking");
            view.probeStatus="unavailable";view.recommendedDelay=-1;draw("table-delay-retry");
            view.probeStatus="complete";view.recommendedDelay=4;
            for(const char* control:{"selected-delay","check-connection","apply-recommendation"}) {
                for(int i=0;i<24&&shell.Navigation().Focus()!=control;++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
                Require(shell.Navigation().Focus()==control,"Delay control is unreachable at this viewport/DPI");
                draw((std::string("table-delay-focus-")+control).c_str());
            }
            view.recommendedDelay=-1;view.canApplyDelay=false;view.probeStatus="insufficient samples";view.probeSamples=12;view.probeLost=88;
            view.canReady=true;draw("table-delay-manual-ready");
            view.room.tables[0].ready[0]=true;view.canReady=false;view.canEditSelection=false;draw("table-unready");
            view.room.tables[0].ready[1]=true;view.room.tables[0].phase=room::TablePhase::Playing;
            view.session.match=netplay::MatchState::PostMatch;draw("table-postmatch-waiting");
            view.room.tables[0].resultPending=true;draw("table-postmatch-reporting");
            view.room.tables[0].phase=room::TablePhase::Paused;draw("table-postmatch-unresolved");
            view.room.tables[0].phase=room::TablePhase::Waiting;
            view.room.tables[0].ready[0]=view.room.tables[0].ready[1]=false;
            view.room.tables[0].resultPending=false;view.canReady=view.canEditSelection=true;draw("table-rematch");
            // Applied terminal receipts remain a committed eligibility fence
            // until native/socket/helper retirement and explicit ACK.  Render
            // the waiting reason at every viewport/DPI so it cannot disappear
            // when the room table is narrow or scaled.
            view.room.localTerminalPending=true;view.room.terminalPending[0]=true;
            view.canReady=true;view.canEditSelection=true;
            // Ready itself stays pressable (the runtime parks it); the fighter
            // change control carries the committed waiting reason.
            for(int i=0;i<24&&shell.Navigation().Focus()!="selection";++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
            Require(shell.Navigation().Focus()=="selection","Terminal waiting reason is unreachable at this viewport/DPI");
            draw("table-terminal-pending");
            view.room.localTerminalPending=false;view.room.terminalPending[0]=false;
            view.session.coordinated=true;view.session.authorityWritable=false;
            draw("table-recover-updating");
            view.session.authorityWritable=true;view.session.room=netplay::RoomState::Closing;
            draw("table-recover-leaving");
            view.session.room=netplay::RoomState::Joined;view.session.coordinated=false;
            view.session.recovery=netplay::Recovery::Recovering;
            view.session.error="Room control is recovering. Room actions are paused.";
            const auto recoveryFocus=shell.Navigation().Focus();
            std::string recoveryStatus;Tone recoveryTone=Tone::Neutral;
            SetMenuStatusProbe([&](const char* status,Tone tone){recoveryStatus=status;recoveryTone=tone;});
            draw();
            SetMenuStatusProbe({});
            Require(shell.Navigation().Focus()==recoveryFocus,"Recovery feedback displaced menu focus");
            Require(recoveryStatus==view.session.error,"Pinned recovery status lost its explanation");
            Require(recoveryTone==Tone::Error,"Room control failure is not rendered as an error");
            const auto* feedback=FindWindow("Command feedback");
            Require(feedback->Active&&feedback->DrawList->VtxBuffer.Size>0&&feedback->Size.y>0&&
                feedback->Pos.y>=0&&feedback->Pos.y+feedback->Size.y<=size.h,
                "Pinned recovery status is not visible at this viewport/DPI");
            draw("table-recovering");
            view.session.recovery=netplay::Recovery::ReplacementOffered;
            view.session.error="Room control unavailable. Replace the room when no match is active.";
            view.canReplaceRoom=true;
            draw();
            for(int i=0;i<24&&shell.Navigation().Focus()!="replace-room";++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
            Require(shell.Navigation().Focus()=="replace-room","Replacement action is unreachable at this viewport/DPI");
            draw("table-replacement");
            view.room.tables[0].phase=room::TablePhase::Playing;view.session.match=netplay::MatchState::Preparing;
            view.canReady=false;view.canReplaceRoom=false;
            draw("table-replacement-waiting");
            view.canReplaceRoom=true;draw("table-replacement-preparation");
            view.session.match=netplay::MatchState::PostMatch;view.canReplaceRoom=true;
            draw("table-replacement-local-retired");
            draw(nullptr,MenuInput::Select,1);draw("table-replacement-local-retired-confirmation");
            Require(shell.Navigation().Confirming()&&!shell.Navigation().ConfirmSelected(),
                "Locally retired replacement confirmation is unsafe at this viewport/DPI");
            draw(nullptr,MenuInput::Back,1);draw();
            view.room.tables[0].phase=room::TablePhase::Waiting;view.session.match=netplay::MatchState::None;
            view.canReplaceRoom=false;view.session.recovery=netplay::Recovery::None;view.session.error.clear();
            view.session.match=netplay::MatchState::None;view.canReady=false;
            view.room.tables[0].ready[0]=false;view.canEditSelection=true;view.controllerReady=false;draw("table-controller-needed");
            view.controllerReady=view.canReady=true;
            const auto roomTitle=view.room.name;view.room.name=std::string(64,'W');page("room");draw("room-long-title");view.room.name=roomTitle;
            page("room");
            if(size.w-40*size.dpi>=820*size.dpi&&size.h/size.dpi>=700) {
                Require(FindWindow("Battle slots")->ScrollMax.y<2,"Four room battle slots must fit at standard landscape scale");
                // The wide board sizes its member list to whole cards, so the
                // bottom one is never cut through its portrait.
                const auto* members=FindWindow("Member list");
                const float pitch=58*Scale()+ImGui::GetStyle().ItemSpacing.y;
                const float content=members->Size.y-12*Scale()+ImGui::GetStyle().ItemSpacing.y;
                Require(content>=pitch-.5f&&std::fabs(content/pitch-std::floor(content/pitch+.5f))<.02f,
                    "Member list height is not a whole number of member cards");
            }
            // Move to Leave by clamped navigation, then open the safe dialog.
            for(int i=0;i<64&&shell.Navigation().Focus()!="leave";++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
            draw(nullptr,MenuInput::Select,1);draw("leave-confirmation");
            Require(shell.Navigation().Confirming()&&!shell.Navigation().ConfirmSelected(),"Leave confirmation not safe");
            auto* confirmation=FindWindow("###ConfirmAction");
            Require(confirmation&&confirmation->Size.x>(std::min)(300*size.dpi,size.w*.5f)&&confirmation->Size.x<size.w&&confirmation->Size.y<size.h,"Confirmation geometry unusable");
            draw(nullptr,MenuInput::Back,1);draw();
            view.discordPending=view.discordConfirm=view.discordCanSwitch=true;view.discordRevision=3;draw("discord-invitation");
            view.discordPending=false;draw();
            mode=1;TrainingNavigation().Home();draw("training-home");
            Require(TrainingNavigation().Focus()=="recording","Removed practice position still occupies training root");
            for(const char* screen:{"recording","history"}){
                TrainingNavigation().Home();TrainingNavigation().Push(screen);draw((std::string("training-")+screen).c_str());
            }
            TrainingNavigation().Home();TrainingNavigation().Push("recording");draw();
            // Returning restores the prior selection, which may be below Record.
            for(int i=0;i<20;++i){draw(nullptr,MenuInput::Up,1);draw(nullptr,0,1);}
            for(int i=0;i<30&&TrainingNavigation().Focus()!="record";++i){draw(nullptr,MenuInput::Down,1);draw(nullptr,0,1);}
            Require(TrainingNavigation().Focus()=="record","Recording action unreachable");
            draw(nullptr,MenuInput::Select,1);draw("training-overwrite-confirmation");
            Require(TrainingNavigation().Confirming()&&!TrainingNavigation().ConfirmSelected(),"Training overwrite default is not Cancel");
            draw(nullptr,MenuInput::Back,1);draw();
            Require(TrainingNavigation().Screen()=="recording"&&!TrainingNavigation().Confirming(),"Training Back did more than cancel");
            acceptTraining=true;draw(nullptr,MenuInput::Down,1);draw();draw(nullptr,MenuInput::Select,1);draw("training-pending");
            training.commandId=trainingCommand.requestId;training.commandAccepted=false;
            training.commandError="Practice command rejected: the battle state changed while the command was pending. Wait until both fighters are ready and try again. This deliberately long explanation must not displace the controls or button legend.";
            draw("training-command-error");Require(!TakeMenuReturn(),"Failed training command closed flyout");
            training.ready=false;TrainingNavigation().Home();TrainingNavigation().Push("recording");draw("training-unavailable");
            training.ready=true;training.mode=training::Mode::Recording;draw("training-recording-suspended");training.mode=training::Mode::Idle;
            mode=2;draw("training-hud");
            Require(!io.WantCaptureKeyboard&&!io.WantCaptureMouse,"Passive training HUD captured input");
            {
                MatchStripView strip;strip.names[0]="P1";strip.names[1]="P2";
                // 'small' is a windows.h macro; never name a local that.
                strip.size=0;const float sizeSmall=MatchStripScale(strip);
                strip.size=1;const float sizeStandard=MatchStripScale(strip);
                strip.size=2;const float sizeLarge=MatchStripScale(strip);
                Require(sizeSmall<sizeStandard&&sizeStandard<sizeLarge,
                    "Match HUD size settings collapse at this viewport; two of the three choices do nothing");
            }
            auto* hud=FindWindow("Training frame meter");Require(hud->Size.x<=size.w*.76f&&hud->Size.y<size.h*.13f,"Passive HUD too large");
            SetMenuGlyphs(4,0,0);draw("training-hud-directinput");
            Require(!io.WantCaptureKeyboard&&!io.WantCaptureMouse,"DirectInput HUD captured input");
            SetMenuGlyphs(0,0,0);draw("training-hud-keyboard");SetMenuGlyphs(3,0x40000,0x20000);
            mode=3;draw("match-hud");
            {
                // The strip's link-state line: the most urgent condition wins,
                // a stall names itself, a warning carries the drop countdown.
                MatchStripView strip;strip.names[0]="P1";strip.names[1]="P2";
                Require(MatchStripStateLine(strip).empty(),"Match HUD shows a state line with nothing to say");
                strip.notice="Connection restored.";strip.noticeSeverity=0;
                Require(MatchStripStateLine(strip)=="Connection restored.","Info notice not shown on the match HUD");
                strip.connectionWarning=true;strip.disconnectCountdownMs=2100;
                Require(MatchStripStateLine(strip)==sf4e::loc::Tf("match.connection_countdown",3),"Connection warning countdown missing or wrong rounding");
                strip.disconnectCountdownMs=-1;
                Require(MatchStripStateLine(strip)==sf4e::loc::T("match.connection_unstable"),"Connection warning without a countdown");
                strip.predictionStalled=true;
                Require(MatchStripStateLine(strip)==sf4e::loc::T("match.waiting_opponent"),"A prediction stall must be named on the match HUD");
                strip.notice="Opponent disconnected. The match is over.";strip.noticeSeverity=2;
                Require(MatchStripStateLine(strip)=="Opponent disconnected. The match is over.","An error notice must outrank the stall and warning lines");
                strip.pingMs=68;strip.rollbackFrames=7;strip.appliedDelay=2;
                matchStrip=strip;mode=3;draw("match-hud-disconnected");
                matchStrip.notice.clear();matchStrip.noticeSeverity=0;matchStrip.predictionStalled=false;
                matchStrip.connectionWarning=true;matchStrip.disconnectCountdownMs=1400;draw("match-hud-warning");
                matchStrip.connectionWarning=false;matchStrip.disconnectCountdownMs=-1;matchStrip.predictionStalled=true;draw("match-hud-stalled");
                matchStrip.predictionStalled=false;
            }
            for(int hudSize=0;hudSize<3;++hudSize){
                matchStrip.size=hudSize;
                const auto shot="match-hud-size-"+std::to_string(hudSize);draw(shot.c_str());
            }
            matchStrip.raised=true;draw("match-hud-raised");
            matchStrip.names[0]="Long player name with UTF-8 \xc3\xa9\xc3\xa9\xc3\xa9";matchStrip.names[1]="Another very long player name";
            matchStrip.pingMs=9999;matchStrip.rollbackFrames=999;matchStrip.appliedDelay=10;draw("match-hud-long");
            matchStrip.pingMs=-1;matchStrip.appliedDelay=-1;draw("match-hud-unavailable");
            matchStrip.spectator=true;draw("match-hud-spectator");
            ImGui_ImplDX9_InvalidateDeviceObjects();draw("match-hud-reset");
            mode=5;draw("controller-warning");
            Require(!io.WantCaptureKeyboard&&!io.WantCaptureMouse,"Controller warning captured input");
            auto* warning=FindWindow("Controller warning");
            Require(warning->ScrollMax.y<1&&warning->Pos.y+warning->Size.y<size.h,"Controller warning escaped viewport");
            mode=4;draw("launch-recovery");recoveryUpdates=true;recoveryState.update.ok=recoveryState.update.updateAvailable=true;
            recoveryState.update.expectedSha256=std::string(64,'a');draw("update-available");
            draw(nullptr,MenuInput::Down,1);draw();draw(nullptr,MenuInput::Select,1);draw("update-confirmation");
            Require(recoveryMenu.navigation.Confirming()&&!recoveryMenu.navigation.ConfirmSelected(),"Recovery update confirmation is unsafe");
            draw(nullptr,MenuInput::Back,1);draw();recoveryState.pending=true;recoveryState.downloadedBytes=25*1024*1024;recoveryState.totalBytes=100*1024*1024;
            recoveryState.message="Downloading the verified update. You can cancel this operation.";draw("update-downloading");
            mode=0;shell.Navigation().Home();draw("home-restored");
            auto* main=FindWindow("EmberShell");
            Require(main->Pos.x==0&&main->Pos.y==0&&main->Size.x==size.w&&main->Size.y==size.h,"Shell geometry changed");
            const float padding=ImGui::GetStyle().WindowPadding.x;
            ApplyTheme(size.dpi+.25f);ApplyTheme(size.dpi);ImGui_ImplDX9_InvalidateDeviceObjects();
            Require(ImGui::GetStyle().WindowPadding.x==padding,"DPI scaling accumulated");draw();
            ImGui_ImplDX9_InvalidateDeviceObjects();renderer.Resize(size.w,size.h);draw();
            ImGui_ImplDX9_Shutdown();ImGui::DestroyContext();
        }
        }
        sf4e::loc::SetActive(sf4e::loc::Locale::En);
        std::printf("Localized controller-first UI render checks passed: %d DX9 frames across forty locale/viewport/DPI configurations.\n",frames);
        return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"UI render check failed: %s\n",error.what());return 1;}
}
