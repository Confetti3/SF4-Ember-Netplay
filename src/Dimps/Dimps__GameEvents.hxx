#pragma once

#include <string>
#include <windows.h>

#include <nlohmann/json.hpp>
#include "../common/SelectionValue.hxx"

#include "Dimps__Event.hxx"
#include "Dimps__Game.hxx"
#include "Dimps__Platform.hxx"

namespace Dimps {
	namespace GameEvents {
		void Locate(HMODULE peRoot);

		namespace StageSelect {
			void Locate(HMODULE peRoot);

			struct Control {
				static void Locate(HMODULE peRoot);

				enum Phase {
					SSP_INACTIVE = 0,
					SSP_ACTIVE = 1,
					SSP_SELECTED = 2,
					SSP_COMPLETE = 3
				};

				typedef struct __publicMethods {
					int (Control::* GetPhase)();
					void (Control::* SelectStage)(char* stageCode);
					void (Control::* SetStageCursor)(char* stageCode);
				} __publicMethods;

				static __publicMethods publicMethods;
			};
		}

		struct RootEvent : Dimps::Event::EventBaseWithEC {
			static char** eventFlowDefinition;
			static Game::ProgressData** GetProgressData(RootEvent* e);
			static void Locate(HMODULE peRoot);
		};

		struct MainMenu : Dimps::Event::EventBase {
			static void Locate(HMODULE peRoot);

			enum MainMenuItemObserverState {
				// While a background transition is active, MainMenuItem doesn't respond
				// to any inputs. When idle, it does.
				MMIOS_TRANSITION = 1,
				MMIOS_IDLE = 2
			};

			enum MainMenuItemID {
				MMI_ARCADE = 0,
				MMI_VERSUS = 1,
				MMI_NETWORK = 2,
				MMI_CHALLENGE = 3,
				MMI_TRAINING = 4,
				MMI_PLAYERDATA = 5,
				MMI_OPTIONS = 6,
				MMI_STORE = 7,
				MMI_BENCHMARK = 8,
				MMI_QUIT = 10,
				MMI_ROOT = 11,
				MMI_TOTITLE = 12,
				MMI_MANUAL = 13,
			};

			typedef struct __publicMethods {
				void* (MainMenu::* Destroy)(DWORD arg1);
			} __publicMethods;

			typedef struct __itemObserverMethods {
				void (MainMenu::* OnModeSelected)(int mode);
				void* (MainMenu::* GoToVersusMode)();
				int (MainMenu::* GetItemObserverState)();
			} __itemObserverMethods;

			typedef struct __staticMethods {
				MainMenu* (*Factory)(DWORD arg1, DWORD arg2, DWORD arg3);
			} __staticMethods;

			static __itemObserverMethods itemObserverMethods;
			static __publicMethods publicMethods;
			static __staticMethods staticMethods;

			// When item-observer methods are called, the base of the `this`
			// pointer needs to point at the item observer vtable contained
			// in the MainMenu object, which is _not_ the base of the object-
			// it's at offset 0x40. These helpers can make things a little
			// more clear when invoking item observer methods.
			static MainMenu* ToItemObserver(MainMenu* s);
			static MainMenu* FromItemObserver(MainMenu* s);
		};

		struct VsCharaSelect : Dimps::Event::EventBase {
			enum CharaSelectFlags {
				CHARASEL_P1_SIDEENABLED = 0x1,
				CHARASEL_P2_SIDEENABLED = 0x2,
				CHARASEL_SINGLE_SELECTOR = 0x4,
				CHARASEL_P1_REMOTE_BLINDPICK = 0x8,
				CHARASEL_P2_REMOTE_BLINDPICK = 0x10,
				CHARASEL_TIMER_ENABLED = 0x20,
				CHARASEL_ALLOW_CUSTOM_SELECT = 0x800,
				CHARASEL_ALLOW_UC_SELECT = 0x1000,
				CHARASEL_ALLOW_HANDICAP_SELECT = 0x2000,
				CHARASEL_ALLOW_CANCEL = 0x8000
			};

			struct PlayerConditions {
				BYTE pad[492];

				static Platform::dString* GetSelectedCharaAbbrev(PlayerConditions* c);
				static Platform::dString* GetHoveredCharaAbbrev(PlayerConditions* c);
				static BYTE* GetColor(PlayerConditions* c);
				static BYTE* GetCostume(PlayerConditions* c);
				static BYTE* GetPersonalAction(PlayerConditions* c);
				static BYTE* GetUltraCombo(PlayerConditions* c);
				static BYTE* GetWinQuote(PlayerConditions* c);
				static BYTE* GetHandicap(PlayerConditions* c);
				static BYTE* GetEdition(PlayerConditions* c);
			};

			struct CharaSelectState {
				DWORD flags;
				PlayerConditions playerConditions[2];
			};

			static void Locate(HMODULE peRoot);
			static CharaSelectState* GetState(VsCharaSelect* event);

			typedef struct __publicMethods {
				void* (VsCharaSelect::* Destroy)(DWORD arg1);
			} __publicMethods;

			typedef struct __staticMethods {
				VsCharaSelect* (*Factory)(DWORD arg1, DWORD arg2, DWORD arg3);
			} __staticMethods;

			static __publicMethods publicMethods;
			static __staticMethods staticMethods;
		};


		struct VsBattle : Dimps::Event::EventBase {
			static void Locate(HMODULE peRoot);

			typedef struct __privateMethods {
				int (VsBattle::* CheckAndMaybeExitBasedOnExitType)();
				void(VsBattle::* PrepareBattleRequest)();
			} __privateMethods;

			typedef struct __publicMethods {
				int (VsBattle::* HasInitialized)();
				void(VsBattle::* RegisterTasks)();
				void(VsBattle::* ExitForeground)();
			} __publicMethods;

			static __privateMethods privateMethods;
			static __publicMethods publicMethods;

			// The VsBattle class's IsTerminationComplete method is shared
			// with 16 other events and is deduplicated, but we want to
			// inject unique behavior into the VsBattle's implementation.
			// We could detour the shared implementation and check the
			// vtable address to determine if the `this` pointer is
			// referring to a VsBattle, but it's easier to just inject
			// a new method directly into the VsBattle vtable.
			static BOOL (VsBattle::** vt_IsTerminationComplete)();

			static Game::Request** GetRequest(VsBattle* battleEvent);
		};

		struct VsMode : Dimps::Event::EventBaseWithEC {
			struct ConfirmedCharaConditions {
				BYTE charaID;
				BYTE costume;
				BYTE color;
				BYTE _unused;
				BYTE personalAction;
				BYTE winQuote;
				BYTE ultraCombo;
				BYTE handicap;
				BYTE unc_edition;
			};

			struct ConfirmedPlayerConditions {
				BYTE pad[316];

				static int* GetCharaID(ConfirmedPlayerConditions* c);
				static int* GetSideActive(ConfirmedPlayerConditions* c);
				static ConfirmedCharaConditions* GetCharaConditions(ConfirmedPlayerConditions* c);
			};

			static void Locate(HMODULE peRoot);
			static ConfirmedPlayerConditions* GetConfirmedPlayerConditions(VsMode* e);
			static Platform::dString* GetStageName(VsMode* e);
			static int* GetStageCode(VsMode* e);

			typedef struct __publicMethods {
				void* (VsMode::* Destroy)(DWORD arg1);
			} __publicMethods;

			typedef struct __staticMethods {
				VsMode* (*Factory)(DWORD arg1, DWORD arg2, DWORD arg3);
			} __staticMethods;

			static __publicMethods publicMethods;
			static __staticMethods staticMethods;
		};

		struct VsPreBattle : Dimps::Event::EventBaseWithEC {
			static void Locate(HMODULE peRoot);

			typedef struct __publicMethods {
				void (VsPreBattle::* RegisterTasks)();
			} __publicMethods;

			static __publicMethods publicMethods;
		};
	
		struct VsStageSelect : Dimps::Event::EventBase {
			struct StageSelectState {
				enum Flags {
					SSSF_TIMER_ENABLED = 0x1
				};
				
				DWORD flags;
				Platform::dString stageCode1;
				Platform::dString stageCode2;
			};

			static void Locate(HMODULE peRoot);
			static StageSelectState* GetState(VsStageSelect* event);
			static StageSelect::Control* GetControl(VsStageSelect* event);

			typedef struct __publicMethods {
				void* (VsStageSelect::* Destroy)(DWORD arg1);
				void* (VsStageSelect::* GetPhase)();
			} __publicMethods;

			typedef struct __staticMethods {
				VsStageSelect* (*Factory)(DWORD arg1, DWORD arg2, DWORD arg3);
			} __staticMethods;

			static __publicMethods publicMethods;
			static __staticMethods staticMethods;
		};

		inline void to_json(nlohmann::json& j, const VsMode::ConfirmedCharaConditions& c) {
			j = {{"charaID", c.charaID}, {"costume", c.costume}, {"color", c.color},
				{"_unused", c._unused}, {"personalAction", c.personalAction}, {"winQuote", c.winQuote},
				{"ultraCombo", c.ultraCombo}, {"handicap", c.handicap}, {"unc_edition", c.unc_edition}};
		}

		inline void from_json(const nlohmann::json& j, VsMode::ConfirmedCharaConditions& c) {
			using sf4e::selection::ReadByte;
			// Decode into a temporary so a malformed field never partially changes a pick.
			VsMode::ConfirmedCharaConditions decoded{};
			decoded.charaID = ReadByte(j.at("charaID"));
			decoded.costume = ReadByte(j.at("costume"));
			decoded.color = ReadByte(j.at("color"));
			decoded._unused = ReadByte(j.at("_unused"));
			decoded.personalAction = ReadByte(j.at("personalAction"));
			decoded.winQuote = ReadByte(j.at("winQuote"));
			decoded.ultraCombo = ReadByte(j.at("ultraCombo"));
			decoded.handicap = ReadByte(j.at("handicap"));
			decoded.unc_edition = ReadByte(j.at("unc_edition"));
			c = decoded;
		}
	}
}
