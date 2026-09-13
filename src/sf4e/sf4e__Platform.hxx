#pragma once

#include <vector>
#include <windows.h>
#include "../Dimps/Dimps__Platform.hxx"
#include "sf4e.hxx"

namespace sf4e {
	namespace Platform {
		void Install();

		struct D3D : Dimps::Platform::D3D
		{
			static void Install();

			void Destroy();
			DWORD Reset();
			void RunScene_Render(void* sceneCommandList);
		};

		struct GFxApp : Dimps::Platform::GFxApp
		{
			struct AdditionalMemento {
				std::pair<bool, sf4e::Eva::IEmSpriteAction::AdditionalMemento> actions[NUM_GFX_ACTIONS];
			};

			static void RecordToAdditionalMemento(Dimps::Platform::GFxApp* a, AdditionalMemento& m);
			static void RestoreFromAdditionalMemento(Dimps::Platform::GFxApp* a, const AdditionalMemento& m);
		};

		struct Main : Dimps::Platform::Main
		{
			static void Install();

			int Initialize(void*, void*, void*);
			void Destroy();
			static void WINAPI RunWindowFunc(Dimps::Platform::Main* lpMain, HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
		};

		struct Sound : Dimps::Platform::Sound
		{
			static void Install();
			static bool bAllowNewPlayers;

			static uint32_t GetNewPlayerHandle();
		};

		template <int N>
		struct SoundObjectPoolEntry : Dimps::Platform::SoundObjectPoolEntry<N> {
			struct SaveState {
				uint32_t handle;
				DWORD field_0x4;
				uint8_t data[N];
			};
		};

		template <int N>
		struct SoundObjectPool : Dimps::Platform::SoundObjectPool<N> {
			struct SaveState {
				std::vector<typename SoundObjectPoolEntry<N>::SaveState> inactive;
				std::vector<typename SoundObjectPoolEntry<N>::SaveState> active;
			};

			static void Load(Dimps::Platform::SoundObjectPool<N>* pool, SaveState* src) {
				Dimps::Platform::SoundObjectPoolEntry<N>* cursor;
				// Move all active entries to inactive so that we can
				// then pull them off as are needed.
				for (cursor = pool->activeHead; pool->activeHead != NULL; cursor = pool->activeHead) {
					pool->activeHead = cursor->next;
					cursor->prev = pool->inactiveTail;
					cursor->next = NULL;
					if (pool->inactiveHead == NULL) {
						pool->inactiveHead = cursor;
					}
					if (pool->inactiveTail != NULL) {
						pool->inactiveTail->next = cursor;
					}
					pool->inactiveTail = cursor;
				}
				pool->activeTail = NULL;

				for (auto activeIter = src->active.begin(); activeIter != src->active.end(); activeIter++) {
					Dimps::Platform::SoundObjectPoolEntry<N>* newActiveEntry = pool->inactiveHead;
					pool->inactiveHead = newActiveEntry->next;
					if (pool->inactiveHead == NULL) {
						pool->inactiveTail = NULL;
					}
					newActiveEntry->prev = pool->activeTail;
					newActiveEntry->next = NULL;
					newActiveEntry->handle = activeIter->handle;
					newActiveEntry->field_0x4 = activeIter->field_0x4;
					memcpy(newActiveEntry->data, activeIter->data, N);

					// Update the pool's active list pointers. This newly active
					// item will always be the last, but it may also require
					// additional reconciliation.
					if (pool->activeHead == NULL) {
						pool->activeHead = newActiveEntry;
					}
					if (pool->activeTail != NULL) {
						pool->activeTail->next = newActiveEntry;
					}
					pool->activeTail = newActiveEntry;
				}

				cursor = pool->inactiveHead;
				for (auto inactiveIter = src->inactive.begin(); inactiveIter != src->inactive.end(); inactiveIter++) {
					cursor->handle = inactiveIter->handle;
					cursor->field_0x4 = inactiveIter->field_0x4;
					memcpy(cursor->data, inactiveIter->data, N);
					cursor = cursor->next;
				}
			}

			static void Save(Dimps::Platform::SoundObjectPool<N>* pool, SaveState* dst) {
				Dimps::Platform::SoundObjectPoolEntry<N>* cursor;
				for (cursor = pool->inactiveHead; cursor != NULL; cursor = cursor->next) {
					SoundObjectPoolEntry<N>::SaveState entryState;
					entryState.handle = cursor->handle;
					entryState.field_0x4 = cursor->field_0x4;
					memcpy(entryState.data, cursor->data, N);
					dst->inactive.push_back(entryState);
				}
				for (cursor = pool->activeHead; cursor != NULL; cursor = cursor->next) {
					SoundObjectPoolEntry<N>::SaveState entryState;
					entryState.handle = cursor->handle;
					entryState.field_0x4 = cursor->field_0x4;
					memcpy(entryState.data, cursor->data, N);
					dst->active.push_back(entryState);
				}
			}
		};
	}
}