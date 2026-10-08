#pragma once

#include <cstddef>
#include <cstdint>

#include <windows.h>

#include "Dimps__Eva.hxx"

namespace Dimps {
	namespace Game {
		using Dimps::Eva::IEmSpriteNode;

		void Locate(HMODULE peRoot);

		struct GameMementoKey {
			struct MementoID {
				DWORD lo;
				DWORD hi;
			};

			struct Metadata {
				MementoID id;
				void* memento;
			};

			typedef struct __publicMethods {
				void (GameMementoKey::* Initialize)(void* mementoable, int numMementos);
				void (GameMementoKey::* ClearKey)();
			} __publicMethods;

			static void Locate(HMODULE peRoot);
			static __publicMethods publicMethods;

			static int* totalMementoSize;

			void* mementoableObject;
			Metadata* metadata;
			int numMementos;
			void* mementos;
			int sizeAllocated;
			int nextMementoIndex;
		};

		struct ProgressData {
			enum NextBattleType {
				NBT_PVP = 0,
				NBT_PVC = 1,
				NBT_CVC = 2,
			};

			struct BattleTypeSettings {
				int diffculty;
				int rounds;
				Math::FixedPoint timeLimit;
				BOOL recordReplay;
				BOOL editionSelect;
			};

			static NextBattleType* GetNextBattleType(ProgressData* data);
			static BattleTypeSettings* GetBattleTypeSettings(ProgressData* data);
		};

		// Dimps::Game::ReplayInfoList, the game's 310 replay slots (vector at
		// +8). common/ReplaySlots.hxx has the record layout; Read (vtable
		// slot 2, 0x676EF0) fills the slots from the LIST file's stream.
		// The slots form four lists, laid out by a table of first slots
		// (0xA6A014) and a table of sizes (0x962684: size, largest file).
		struct ReplayInfoList {
			typedef struct __publicMethods {
				BOOL (ReplayInfoList::* Read)(void* stream);
			} __publicMethods;

			// The vector's begin and end on the list, and an entry: 0x108
			// bytes, its vtable (whose third function, 0x676820, fills it
			// from a stream holding a 125-byte record), the slot it names at
			// +4 (-1 when never filled, 0x676B70) and the slot's two bytes
			// at +0x106.
			static constexpr std::size_t Entries = 0x8, EntriesEnd = 0xC, EntryBytes = 0x108, EntrySlot = 0x4, EntrySlotBytes = 0x106, EntryDeserialize = 2;
			// The same, read: the vector's two ends, the n-th entry of a
			// table that starts at `entries`, the slot an entry names and
			// where its two bytes are.
			static std::uint8_t* GetEntries(ReplayInfoList* list) { return *reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uint8_t*>(list) + Entries); }
			static std::uint8_t* GetEntriesEnd(ReplayInfoList* list) { return *reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uint8_t*>(list) + EntriesEnd); }
			static std::uint8_t* GetEntry(std::uint8_t* entries, int at) { return entries + at * EntryBytes; }
			static std::uint32_t GetEntrySlot(const std::uint8_t* entry) { return *reinterpret_cast<const std::uint32_t*>(entry + EntrySlot); }
			static std::uint8_t* GetEntrySlotBytes(std::uint8_t* entry) { return entry + EntrySlotBytes; }
			// An entry's own function that fills it from a stream.
			static PVOID GetEntryDeserialize(std::uint8_t* entry) { return (*reinterpret_cast<PVOID**>(entry))[EntryDeserialize]; }

			static void Locate(HMODULE peRoot);
			static __publicMethods publicMethods;
			static DWORD* listFirstSlot;
			static DWORD* listSizes;
		};

		// What the game does to play a replay it has loaded into the replay
		// system (the replay channel's ReplayPlayer, 0x482CE0): a battle
		// Request (two 0xA0-byte player blocks) is built from the replay's
		// header and its slot record (a ReplayInfoList entry, 0x108 bytes),
		// and handed to the battle flow, whose Versus state then loads it.
		// The local battle log's list (BattleLog::SelectEvent's +0x44, created
		// in 0x470820): rows of 0x38 bytes between +0x74 and +0x78 with the
		// slot first, the selected row at +0x84. The DECIDE handler (0x4797C0)
		// stores the row and calls PlayRow (0x4796D0): it reads the slot through
		// the save controller into the mode event's buffer and sets the list's
		// step (+0x6C, run by the Select event's task) to the game's own chain:
		// 0x479020 checks the read, 0x478D40 loads the replay system, builds the
		// request into the mode event and ends Select with 0, which moves the
		// mode to Versus and then Battle.
		struct ReplayBattle {
			static constexpr std::size_t SelectList = 0x44, ListRowsBegin = 0x74, ListRowsEnd = 0x78, ListRowBytes = 0x38, ListSelected = 0x84;
			// The Versus splash (the Versus state's +0x48) advances when its Flash
			// movies finish and the announcer voice ends, or when the player
			// presses Start (0x605240 at 0x605371): the voice at +0x150 is faded
			// (0x686B50), its phase +0x18C and state +0x11C are set to 3, which
			// the state's check waits for, and the two movies at +0x2C8 (8 bytes
			// each) get the "Close" signal (0x78DAF0) when valid (0x78ECD0).
			static constexpr std::size_t VersusSplash = 0x48, SplashState = 0x11C, SplashPhase = 0x18C, SplashVoice = 0x150, SplashMovies = 0x2C8;
			// When the replay's match is over the Battle state opens its end
			// menu (the object at +0x464, 0x484609) and its step 0x484310 waits
			// for the choice at +0x480, which the menu's handler writes
			// (0x42DFEF) and is -1 until then: 0 plays the replay again
			// (0x4846C0), anything else ends the state, which puts the log back
			// on its list. Read from the code; which number the menu's own
			// "leave" writes was not seen, so 1 stands for it.
			static constexpr std::size_t BattleEndChoice = 0x480;
			static constexpr int EndChoiceLeave = 1;
			static int* GetEndChoice(void* battle) { return reinterpret_cast<int*>(reinterpret_cast<std::uint8_t*>(battle) + BattleEndChoice); }
			struct List;
			struct Voice;
			struct Movie;
			struct Splash;
			// The same, read. The battle log's list on its Select state, its
			// rows (each begins with its slot) and the row it has selected.
			static List* GetList(void* select) { return *reinterpret_cast<List**>(reinterpret_cast<std::uint8_t*>(select) + SelectList); }
			static const int* GetRowsBegin(List* list) { return *reinterpret_cast<const int**>(reinterpret_cast<std::uint8_t*>(list) + ListRowsBegin); }
			static const int* GetRowsEnd(List* list) { return *reinterpret_cast<const int**>(reinterpret_cast<std::uint8_t*>(list) + ListRowsEnd); }
			static const int* NextRow(const int* row) { return reinterpret_cast<const int*>(reinterpret_cast<const std::uint8_t*>(row) + ListRowBytes); }
			static int RowIndex(const int* begin, const int* row) { return static_cast<int>((reinterpret_cast<const std::uint8_t*>(row) - reinterpret_cast<const std::uint8_t*>(begin)) / ListRowBytes); }
			static int* GetSelectedRow(List* list) { return reinterpret_cast<int*>(reinterpret_cast<std::uint8_t*>(list) + ListSelected); }
			// The Versus state's splash, its state and phase, its announcer
			// voice and its two movies.
			static Splash* GetSplash(void* versus) { return *reinterpret_cast<Splash**>(reinterpret_cast<std::uint8_t*>(versus) + VersusSplash); }
			static int* GetSplashState(Splash* splash) { return reinterpret_cast<int*>(reinterpret_cast<std::uint8_t*>(splash) + SplashState); }
			static int* GetSplashPhase(Splash* splash) { return reinterpret_cast<int*>(reinterpret_cast<std::uint8_t*>(splash) + SplashPhase); }
			static Voice* GetSplashVoice(Splash* splash) { return *reinterpret_cast<Voice**>(reinterpret_cast<std::uint8_t*>(splash) + SplashVoice); }
			static Movie* GetSplashMovie(Splash* splash, int movie) { return reinterpret_cast<Movie*>(reinterpret_cast<std::uint8_t*>(splash) + SplashMovies + movie * 8); }
			typedef struct __staticMethods {
				void (__thiscall* PlayRow)(List* list);
				void (__thiscall* FadeVoice)(Voice* voice, int frames);
				bool (__thiscall* MovieValid)(Movie* movie);
				void (__thiscall* MovieSignal)(Movie* movie, const char* name, int);
			} __staticMethods;

			static void Locate(HMODULE peRoot);
			static __staticMethods staticMethods;
		};

		// Dimps::Game::SaveDataController (singleton from 0x67C880), which reads
		// and writes the save files through Steam. The replay channel reads a
		// slot into a buffer with ReadSlot (0x67C450) and Start (0x67C3F0),
		// then waits for State (0x67C410, the implementation's +0x240) to
		// reach 2; Busy (0x67C430) refuses a new request while one runs.
		struct SaveDataController {
			typedef struct __publicMethods {
				void (SaveDataController::* ReadSlot)(int slot, void* buffer, int bytes, int a, int b);
				void (SaveDataController::* Start)(int);
				int (SaveDataController::* State)();
				BOOL (SaveDataController::* Busy)();
			} __publicMethods;
			typedef struct __staticMethods {
				SaveDataController* (*GetSingleton)();
			} __staticMethods;

			static void Locate(HMODULE peRoot);
			static __publicMethods publicMethods;
			static __staticMethods staticMethods;
		};

		struct Request {
			typedef struct __publicMethods {
				DWORD (Request::* GetRandomSeed)();
				void (Request::* SetIsOnlineBattle)(BOOL isOnlineBattle);
				void (Request::* SetRandomSeed)(DWORD seed);
				// One of a player's battle parameters. The request keeps a
				// table of them a player (0x2B of them, at +0xF0, 0x2B8 bytes
				// a player) and refuses a value outside the parameter's own
				// range. The battle reads them back through
				// Battle::System's vtable +0x158.
				void (Request::* SetPlayerParam)(int player, int param, int value);
			} __publicMethods;

			// The parameters the fighter's own update reads every frame for
			// its gauges, and what their values do there (actor code at
			// 0x5529F0, 0x552B20, 0x557E20, 0x557F60, 0x552E50):
			// PM_NORMAL the gauge behaves as in a match, PM_EMPTY holds it
			// at nothing, PM_FULL holds it full, PM_REFILL fills it again
			// while the fighter is left alone. Training mode sets the same
			// values from its pause menu. PP_VITALITY at PM_REFILL also
			// skips the knockout (0x55C89A).
			enum PlayerParam {
				PP_VITALITY = 0x1A,
				PP_RECOVERABLE_VITALITY = 0x1C,
				PP_SUPER_COMBO = 0x1E,
				PP_REVENGE = 0x21,
				PP_STUN = 0x24,
			};
			enum ParamMode { PM_NORMAL = 0, PM_EMPTY = 6, PM_FULL = 7, PM_REFILL = 8 };

			static void Locate(HMODULE peRoot);
			static __publicMethods publicMethods;
		};

		namespace Sprite {
			struct Control {
				typedef struct __publicMethods {
					// The polymorphism here is technically wrong- these methods
					// are deduplicated by the compiler for subclasses of Control
					// with identical implementations. There's no way to safely
					// express this duplication with Detours, because multiple
					// pointers to the same function could hypothetically
					// be independently detoured, which would result in a last-writer-wins
					// situation. Instead, just put it in the parent class and be careful.
					void(Control::* Disable_0x57bd80)();
					void(Control::* Enable_0x577910)();
					void(Control::* Enable_0x588450)();
				} __publicMethods;

				static DWORD* GetEnabled(Control* c);
				static void Locate(HMODULE peRoot);
				static __publicMethods publicMethods;
			};

			struct SingleNodeControl : Control {
				static int* GetCurrentFrame(SingleNodeControl* c);
				static IEmSpriteNode** GetSpriteNode(SingleNodeControl* c);
			};
		}
	}
}