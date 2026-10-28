#pragma once

#include <windows.h>

#include "Dimps__Eva.hxx"
#include "Dimps__Game.hxx"
#include "Dimps__Game__Battle.hxx"
#include "Dimps__Platform.hxx"

namespace Dimps {
	namespace Game {
		namespace Battle {
			namespace Hud {
				using Dimps::Eva::Task;
				using Dimps::Game::Battle::IUnit;
				using Dimps::Game::GameMementoKey;
				using Dimps::Game::Sprite::Control;
				using Dimps::Game::Sprite::SingleNodeControl;
				using Dimps::Platform::dDeque_0x10;
				using Dimps::Platform::WithReleaser;

				void Locate(HMODULE peRoot);

				// Dimps::Game::Battle::Hud::Continue::Unit and
				// Dimps::Game::Battle::Hud::Notice::Unit have identical
				// implementations of their update tasks, despite the fact
				// that their only shared ancestors (IMessageReceivable and
				// IUnit) don't define the method or the View object that
				// the method delegates to. Since the implementation ends up
				// being compiled to the same machine code, the compiler
				// deduplicated the implementation, which makes it impossible
				// to safely represent the original source implementation with
				// two separate pointers in each Unit class. Instead, _one_
				// pointer to the deduplicated implementation is stored here,
				// in the nearest containing namespace.
				//
				// Strictly speaking, the polymorphism here is wrong- there's no
				// actual way to express the deduplication performed by the
				// compiler- but it is at least functional.
				extern void (IUnit::* SharedHudUpdate)(Task** task);

				namespace Announce {
					struct Announcement {
						int data_0x0;
						int gamestart_related_0x4;
						int edition;
						int gameResult;
					};

					struct Round : Control {
						static WORD* GetCurrentRound(Round* c) {
							return (WORD*)((unsigned int)c + 0x2c);
						}
						static IEmSpriteNode** GetRoundActionNode(Round* c) {
							return (IEmSpriteNode**)((unsigned int)c + 0x1c);
						}
						static IEmSpriteNode** GetRoundNumberNode(Round* c) {
							return (IEmSpriteNode**)((unsigned int)c + 0x24);
						}

						typedef struct __publicMethods {
							void (Round::* Enable)();
							void (Round::* Disable)();
						} __publicMethods;

						static void Locate(HMODULE peRoot);
						static __publicMethods publicMethods;
					};

					struct View {
						static dDeque_0x10* GetQueuedAnnouncements(View* v) {
							return (dDeque_0x10*)((unsigned int)v + 0xb8);
						}
						static Control** GetActiveControl(View* v) {
							return (Control**)((unsigned int)v + 0xd0);
						}
						static Round** GetRoundControl(View* v) {
							return (Round**)((unsigned int)v + 0x20);
						}
						static SingleNodeControl** GetFinalRoundControl(View* v) {
							return (SingleNodeControl**)((unsigned int)v + 0x28);
						}
						static SingleNodeControl** GetExtraRoundControl(View* v) {
							return (SingleNodeControl**)((unsigned int)v + 0x30);
						}
						static SingleNodeControl** GetFightControl(View* v) {
							return (SingleNodeControl**)((unsigned int)v + 0x38);
						}
						static SingleNodeControl** GetKoControl(View* v) {
							return (SingleNodeControl**)((unsigned int)v + 0x40);
						}
						static SingleNodeControl** GetDoubleKoControl(View* v) {
							return (SingleNodeControl**)((unsigned int)v + 0x48);
						}
						static SingleNodeControl** GetTimeControl(View* v) {
							return (SingleNodeControl**)((unsigned int)v + 0x50);
						}
						static SingleNodeControl** GetDrawControl(View* v) {
							return (SingleNodeControl**)((unsigned int)v + 0x58);
						}
						static SingleNodeControl** GetPerfectControl(View* v) {
							return (SingleNodeControl**)((unsigned int)v + 0x60);
						}
						static SingleNodeControl** GetWinControl(View* v) {
							return (SingleNodeControl**)((unsigned int)v + 0x68);
						}
						static SingleNodeControl** GetLoseControl(View* v) {
							return (SingleNodeControl**)((unsigned int)v + 0x70);
						}
						static SingleNodeControl** GetWins1Control(View* v) {
							return (SingleNodeControl**)((unsigned int)v + 0x78);
						}
						static SingleNodeControl** GetWins2Control(View* v) {
							return (SingleNodeControl**)((unsigned int)v + 0x80);
						}
						static SingleNodeControl** GetSuccessControl(View* v) {
							return (SingleNodeControl**)((unsigned int)v + 0x88);
						}
						static SingleNodeControl** GetClearControl(View* v) {
							return (SingleNodeControl**)((unsigned int)v + 0x90);
						}
						static SingleNodeControl** GetReadyGoControl(View* v) {
							return (SingleNodeControl**)((unsigned int)v + 0x98);
						}
						static SingleNodeControl** GetEditionControl(View* v) {
							return (SingleNodeControl**)((unsigned int)v + 0xa0);
						}
					};

					struct Unit : IUnit {
						typedef struct __publicMethods {
							void (Unit::* HudAnnounce_Update)(Task** task);
						} __publicMethods;

						static Eva::Task** GetHudAnnounceUpdateTask(Unit* u) {
							return (Eva::Task**)((unsigned int)u + 0x8);
						}
						static View** GetView(Unit* u);
						static void Locate(HMODULE peRoot);
						static __publicMethods publicMethods;
					};
				}

				namespace Cockpit {
					struct View {
						typedef struct __publicMethods {
							void (View::* Update)();
							void (View::* RestoreFromInternalMementoKey)(GameMementoKey::MementoID* id);
						} __publicMethods;

						static void Locate(HMODULE peRoot);
						static __publicMethods publicMethods;
					};

					struct Unit : IUnit {
						typedef struct __publicMethods {
							void (Unit::* HudCockpit_Update)(Task** task);
						} __publicMethods;

						static Eva::Task** GetHudCockpitUpdateTask(Unit* u) {
							return (Eva::Task**)((unsigned int)u + 0x8);
						}
						static void Locate(HMODULE peRoot);
						static __publicMethods publicMethods;
					};
				}

				namespace Continue {
					struct Unit : IUnit {
						static Eva::Task** GetHudContinueUpdateTask(Unit* u) {
							return (Eva::Task**)((unsigned int)u + 0x8);
						}
					};
				}

				namespace Cursor {
					struct Unit : IUnit {
						typedef struct __publicMethods {
							void (Unit::* HudCursor_Update)(Task** task);
						} __publicMethods;

						static Eva::Task** GetHudCursorUpdateTask(Unit* u) {
							return (Eva::Task**)((unsigned int)u + 0x8);
						}
						static void Locate(HMODULE peRoot);
						static __publicMethods publicMethods;
					};
				}

				namespace Notice {
					enum NoticeType {
						NT_COMBO = 0,
						NT_FIRSTATTACK = 1,
						NT_REVERSAL = 2,
						NT_TECHNICAL = 3,
						NT_COUNTERHIT = 4,
						NT_SUPERCANCEL = 5,
						NT_STUN = 6,
						NT_SCFINISH = 7,
						NT_UCFINISH = 8,
						NT_NULL = 9
					};

					struct NoticeData {
						int type;
						int score;
						int comboLen;
						int bShouldShowAdjective;
					};

					struct Bonus : Dimps::Game::Sprite::Control {
						typedef struct __publicMethods {
							void (Bonus::* Enable)();
							void (Bonus::* Disable)();
						} __publicMethods;
						
						static void Locate(HMODULE peRoot);
						static __publicMethods publicMethods;
						static Eva::IEmSpriteNode** GetActiveSprite(Bonus* b) {
							return (Eva::IEmSpriteNode**)((unsigned int)b + 0x64);
						}
						static DWORD* GetCurrentBonus(Bonus* b) {
							return (DWORD*)((unsigned int)b + 0x74);
						}
						static int32_t* GetScore(Bonus* b);
						static DWORD* GetIsActive(Bonus* b);
					};

					struct Combo : Dimps::Game::Sprite::Control {	
						typedef struct __publicMethods {
							void (Combo::* Enable)();
							// Disabling is 0057bd80
						} __publicMethods;
						
						static void Locate(HMODULE peRoot);
						static __publicMethods publicMethods;
						static uint32_t* GetComboLength(Combo* c) {
							return (uint32_t*)((unsigned int)c + 0x60);
						}
						static int32_t* GetScore(Combo* c);
						static DWORD* GetIsActive(Combo* c);
						static DWORD* GetShouldShowAdjective(Combo* c) {
							return (DWORD*)((unsigned int)c + 0x6c);
						}
					};

					struct Player {

						static WithReleaser<Bonus>* GetBonuses(Player* p) {
							return (WithReleaser<Hud::Notice::Bonus>*)((unsigned int)p + 0x28);
						}
						static WithReleaser<Combo>* GetCombo(Player* p) {
							return (WithReleaser<Hud::Notice::Combo>*)((unsigned int)p + 0x20);
						}
						static dDeque_0x10* GetQueuedComboNotices(Player* p) {
							return (dDeque_0x10*)((unsigned int)p + 0x38);

						}
						static dDeque_0x10* GetQueuedBonusNotices(Player* p) {
							return (dDeque_0x10*)((unsigned int)p + 0x50);
						}
					};

					struct View {
						static WithReleaser<Player>* GetPlayers(View* v) {
							return (WithReleaser<Hud::Notice::Player>*)((unsigned int)v + 0x20);
						}
					};

					struct Unit : IUnit {
						typedef struct __publicMethods {
							void (Unit::* RestoreFromInternalMementoKey)();
						} __publicMethods;

						static void Locate(HMODULE peRoot);
						static __publicMethods publicMethods;
						static Eva::Task** GetHudNoticeUpdateTask(Unit* u) {
							return (Eva::Task**)((unsigned int)u + 0x8);
						}
						static View** GetView(Unit* u);
					};
				}

				namespace Result {
					struct Unit : IUnit {
						typedef struct __publicMethods {
							void (Unit::* HudResult_Update)(Task** task);
						} __publicMethods;

						static Eva::Task** GetHudResultUpdateTask(Unit* u) {
							return (Eva::Task**)((unsigned int)u + 0x8);
						}
						static void Locate(HMODULE peRoot);
						static __publicMethods publicMethods;
					};
				}

				namespace Subtitle {
					struct Unit : IUnit {
						typedef struct __publicMethods {
							void (Unit::* HudSubtitle_Update)(Task** task);
						} __publicMethods;

						static Eva::Task** GetHudSubtitleUpdateTask(Unit* u) {
							return (Eva::Task**)((unsigned int)u + 0x8);
						}
						static void Locate(HMODULE peRoot);
						static __publicMethods publicMethods;
					};
				}

				namespace Training {
					struct Unit : IUnit {
						typedef struct __publicMethods {
							void (Unit::* HudTraining_Update)(Task** task);
						} __publicMethods;

						static Eva::Task** GetHudTrainingUpdateTask(Unit* u) {
							return (Eva::Task**)((unsigned int)u + 0x8);
						}
						static void Locate(HMODULE peRoot);
						static __publicMethods publicMethods;
					};
				}

				struct Unit : IUnit
				{
					typedef struct __publicMethods {
						void (Unit::* RecordToInternalMementoKey)(GameMementoKey::MementoID* id);
						void (Unit::* RestoreFromInternalMementoKey)(GameMementoKey::MementoID* id);
					} __publicMethods;

					typedef struct __staticMethods {
						Unit* (*Factory)(DWORD arg1, DWORD arg2);
					} __staticMethods;

					static Announce::Unit** GetAnnounce(Unit* u) {
						return (Announce::Unit**)((unsigned int)u + 0x20);
					}
					static Cockpit::Unit** GetCockpit(Unit* u) {
						return (Cockpit::Unit**)((unsigned int)u + 0x18);
					}
					static Continue::Unit** GetContinue(Unit* u) {
						return (Continue::Unit**)((unsigned int)u + 0x2c);
					}
					static Cursor::Unit** GetCursor(Unit* u) {
						return (Cursor::Unit**)((unsigned int)u + 0x24);
					}
					static Notice::Unit** GetNotice(Unit* u) {
						return (Notice::Unit**)((unsigned int)u + 0x1c);
					}
					static Result::Unit** GetResult(Unit* u) {
						return (Result::Unit**)((unsigned int)u + 0x28);
					}
					static Subtitle::Unit** GetSubtitle(Unit* u) {
						return (Subtitle::Unit**)((unsigned int)u + 0x30);
					}
					static Training::Unit** GetTraining(Unit* u) {
						return (Training::Unit**)((unsigned int)u + 0x34);
					}
					static void Locate(HMODULE peRoot);
					static void ResetUnits(Unit* u);

					static __publicMethods publicMethods;
					static __staticMethods staticMethods;

				};
			}
		}
	}
}