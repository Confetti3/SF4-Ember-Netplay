#pragma once

#include <stdint.h>
#include <windows.h>

#include "Dimps__Eva.hxx"
#include "Dimps__Math.hxx"
#include "Dimps__Platform.hxx"

#define NUM_VALID_EDITIONS 7

namespace Dimps {
	namespace Game {
		namespace Battle {
			using Dimps::Eva::Task;
			using Dimps::Math::FixedPoint;

			void Locate(HMODULE peRoot);

			enum Edition {
				ED_SSF4 = 1,
				ED_AE2011 = 2,
				ED_AE2012 = 4,
				ED_USF4 = 14,
				ED_SF4 = 13,
				ED_OMEGA = 16
			};

			extern int orderedEditions[NUM_VALID_EDITIONS];

			enum GameMode {
				GAMEMODE_ARCADE = 0x0,
				GAMEMODE_VERSUS = 0x1,
				GAMEMODE_BENCHMARK_DEMO = 0x2,
				GAMEMODE_TRAINING = 0x3,
				GAMEMODE_TRIAL = 0x5,
				GAMEMODE_BONUS = 0x6,

				// These appear to be dead, at least in consumer builds.
				// Notably, "RivalBattleMode" and "BossBattleMode" are referenced
				// by the root event's flow, but no corresponding event class
				// exists in the code. Likely, these were for debugging purposes.
				GAMEMODE_DEAD_4 = 0x4,
				GAMEMODE_DEAD_7 = 0x7,

				GAMEMODE_UNDEFINED = 0xffffffff
			};

			struct ValidEditions {
				uint8_t valid[NUM_VALID_EDITIONS];
			};
			extern ValidEditions* validEditionsPerChara;

			struct GameManager {
				char pad[0x49c];

				typedef struct __publicMethods {
					void (GameManager::* GetAgglutinateTime)(FixedPoint* out);
					void (GameManager::* GetRoundTime)(FixedPoint* out);
					// This no-argument reader is native result state, not
					// presentation data.  In the shipped USF4 binary it is the
					// match-result selector/index used by the native flow.
					int (GameManager::* GetNativeResultIndex)();
				} __publicMethods;

				static void Locate(HMODULE peRoot);
				static __publicMethods publicMethods;
			};

			struct IUnit {
				typedef struct __publicMethods {
					void (IUnit::* SharedHudUpdate)(Task** task);
				} __publicMethods;

				static void Locate(HMODULE peRoot);
				static __publicMethods publicMethods;
			};

			// Runs the battle's per-frame job lists (per-fighter updates,
			// collision). Start is called once per battle; with workers > 0
			// every list runs on that many worker threads, otherwise in
			// queue order on the calling thread.
			struct JobManager {
				typedef struct __publicMethods {
					BOOL (JobManager::* Start)(int workers, int jobs, int jobSize);
				} __publicMethods;

				static void Locate(HMODULE peRoot);
				static __publicMethods publicMethods;
			};

			namespace Network {
				struct Unit {
					char pad[0x64f4];
				};
			}

			namespace Pause {
				struct Unit {
					char pad[0x18];

					static Eva::Task** GetPauseTask(Unit* u);
				};
			}

			namespace Sound {
				// Sound methods often return handles or take them as arguments-
				// they seem intended to be opaque, and the fact that most
				// functions return these values in `eax` and receive them
				// without allocating an additional pointer on the stack
				// suggests that the sound subsystem is using basic types to
				// exchange the data. However, they can be decoded to
				// references.
				typedef uint32_t SoundHandle;
				struct SoundReference {
					unsigned short index;
					unsigned short useCount;
					inline static SoundReference FromHandle(SoundHandle h) {
						return SoundReference{ 
							(unsigned short)((h & 0xffff0000) >> 0x10),
							(unsigned short)(h & 0xffff)
						};
					}
					inline SoundHandle ToHandle() { return this->index << 0x10 & this->useCount; }
				};

				typedef uint32_t SoundFlags;
				enum ESoundFlags {
					SOUNDFLAG_STEREOPAN = 0x4,
				};

				typedef uint32_t SoundType;

				struct SoundPlayerManager {
					struct CriPlayerAdapter {
						SoundHandle playerHandle;
						int flags;
						Dimps::Math::Vec4F* position;
						float volume;
						float fadeScale;
						DWORD playState;
						DWORD field6_0x18;

						typedef struct __publicMethods {
							BOOL (CriPlayerAdapter::* IsStillPlaying)();
						} __publicMethods;

						static void Locate(HMODULE peRoot);
						static __publicMethods publicMethods;
					};

					typedef struct __publicMethods {
						void (SoundPlayerManager::* destructor)(BOOL param_1);
						SoundHandle (SoundPlayerManager::* PlaySound)(
							SoundHandle cueSheetHandle,
							uint32_t cueIdx,
							SoundType type,
							SoundFlags flags,
							Dimps::Math::Vec4F* position
						);
						void(SoundPlayerManager::* StopSound)(
							SoundHandle adapterHandle,
							BOOL criParam
							);
						void(SoundPlayerManager::* StopAll)(BOOL criParam);
						void(SoundPlayerManager::* Update)();
					} __publicMethods;

					typedef struct __staticMethods {
						SoundPlayerManager* (*Factory)(DWORD param_1, DWORD param_2, int* numAdapters);
					} __staticMethods;

					typedef Platform::SoundObjectPool<sizeof(CriPlayerAdapter*)> AdapterPool;

					static void Locate(HMODULE peRoot);
					static __publicMethods publicMethods;
					static __staticMethods staticMethods;

					static AdapterPool* GetAdapterPool(SoundPlayerManager* m);
					static CriPlayerAdapter** GetAdapters(SoundPlayerManager* m);
					static int* GetNumAdapters(SoundPlayerManager* m);
				};

				struct Unit {
					typedef struct __publicMethods {
						BOOL (Unit::* IsStillPlaying)(
							uint32_t managerIdx,
							uint32_t adapterHandle
						);
					} __publicMethods;

					static void Locate(HMODULE peRoot);
					static __publicMethods publicMethods;

					static SoundPlayerManager** GetManagerArray(Unit* u);
				};
			}
		}
	}
}
