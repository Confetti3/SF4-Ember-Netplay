#pragma once

#include <map>
#include <vector>
#include <windows.h>

#include "../Dimps/Dimps__Eva.hxx"
#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../Dimps/Dimps__Game__Battle__Camera.hxx"
#include "../Dimps/Dimps__Game__Battle__Chara.hxx"
#include "../Dimps/Dimps__Game__Battle__Command.hxx"
#include "../Dimps/Dimps__Game__Battle__Training.hxx"
#include "../Dimps/Dimps__Math.hxx"

namespace sf4e {
	namespace Game {
		namespace Battle {
			using Dimps::Eva::Task;

			void Install();

			struct IUnit : Dimps::Game::Battle::IUnit {
				// In order for the compiler to construct this method
				// with __thiscall__, the method needs to be declared
				// in the class declaration, despite the fact that this
				// method isn't actually part of the IUnit interface.
				//
				// This is intentionally not installed as part of IUnit.
				void SharedHudUpdate(Task** task);

				static bool bAllowHudUpdate;
			};

			namespace Sound {
				using Dimps::Game::Battle::Sound::SoundFlags;
				using Dimps::Game::Battle::Sound::SoundType;
				using Dimps::Game::Battle::Sound::SoundHandle;
				using rSoundPlayerManager = Dimps::Game::Battle::Sound::SoundPlayerManager;
				using Dimps::Platform::SoundObjectPoolEntry;

				struct SoundPlayerManager : rSoundPlayerManager {
					struct DeferredSoundRequest {
						// Metadata for tracking.
						bool bLive;
						int nFrame;
						SoundHandle currentAdapterHandle;

						// Arguments.
						SoundHandle cueSheetHandle;
						uint32_t cueIdx;
						SoundType type;
						SoundFlags flags;
						Dimps::Math::Vec4F* position;

						static bool IsEqual(DeferredSoundRequest* lhs, DeferredSoundRequest* rhs);
					};

					struct CriPlayerAdapter : rSoundPlayerManager::CriPlayerAdapter {
						static void Install();
						BOOL IsStillPlaying();
					};

					static bool bTrackRequests;
					static bool bUsePureSounds;
					static bool bWarnOnOverflow;
					static std::map<rSoundPlayerManager*, rSoundPlayerManager*> shadowManagerMap;
					static std::map<
						rSoundPlayerManager::CriPlayerAdapter*,
						DeferredSoundRequest
					> adapterToCurrentSound;
					static std::map<rSoundPlayerManager*, std::vector<DeferredSoundRequest>> queuedStops;

					
					static void Install();

					static Dimps::Game::Battle::Sound::SoundPlayerManager* Factory(DWORD param_1, DWORD param_2, int* numAdapters);
					void destructor(BOOL param_1);
					SoundHandle PlaySound(
						SoundHandle cueSheetHandle,
						uint32_t cueIdx,
						SoundType type,
						SoundFlags flags,
						Dimps::Math::Vec4F* position
					);
					void StopSound(SoundHandle adapterHandle, BOOL criParam);
					void StopAll(BOOL criParam);
					static void SyncState();
				};

				struct Unit : Dimps::Game::Battle::Sound::Unit {
					BOOL IsStillPlaying(
						uint32_t managerIdx,
						uint32_t adapterHandle
					);

					static void Install();
				};
			}
		}
	}
}