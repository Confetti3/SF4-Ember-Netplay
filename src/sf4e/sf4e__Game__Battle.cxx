#include <map>
#include <vector>
#include <windows.h>
#include <detours/detours.h>
#include "spdlog/spdlog.h"

#include "../Dimps/Dimps__Eva.hxx"
#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../Dimps/Dimps__Math.hxx"

#include "sf4e__Game__Battle.hxx"
#include "sf4e__Game__Battle__Effect.hxx"
#include "sf4e__Game__Battle__Hud.hxx"
#include "sf4e__Game__Battle__System.hxx"
#include "sf4e__Game__Battle__Vfx.hxx"
#include "sf4e__Platform.hxx"

namespace rBattle = Dimps::Game::Battle;
namespace fBattle = sf4e::Game::Battle;

using fIUnit = sf4e::Game::Battle::IUnit;
using rIUnit = Dimps::Game::Battle::IUnit;
using SoundHandle = Dimps::Game::Battle::Sound::SoundHandle;
using SoundReference = Dimps::Game::Battle::Sound::SoundReference;
using rSoundPlayerManager = Dimps::Game::Battle::Sound::SoundPlayerManager;
using rSoundUnit = Dimps::Game::Battle::Sound::Unit;
using fSoundUnit = sf4e::Game::Battle::Sound::Unit;
using fSoundPlayerManager = sf4e::Game::Battle::Sound::SoundPlayerManager;

bool fIUnit::bAllowHudUpdate = true;
bool fSoundPlayerManager::bUsePureSounds = true;
bool fSoundPlayerManager::bTrackRequests = false;
bool fSoundPlayerManager::bWarnOnOverflow = false;
std::map<
	rSoundPlayerManager*,
	rSoundPlayerManager*
> fSoundPlayerManager::shadowManagerMap;
std::map<
	rSoundPlayerManager::CriPlayerAdapter*,
	fSoundPlayerManager::DeferredSoundRequest
> fSoundPlayerManager::adapterToCurrentSound;
std::map<rSoundPlayerManager*, std::vector<fSoundPlayerManager::DeferredSoundRequest>> fSoundPlayerManager::queuedStops;

void fBattle::Install() {
	Effect::Install();
	Hud::Install();
	Sound::SoundPlayerManager::Install();
	Sound::Unit::Install();
	System::Install();
	Vfx::Install();
}

void fIUnit::SharedHudUpdate(Task** task) {
	if (bAllowHudUpdate) {
		(this->*rIUnit::publicMethods.SharedHudUpdate)(task);
	}
}

void fSoundPlayerManager::Install() {
	void (fSoundPlayerManager:: * _fDestructor)(BOOL) = &destructor;
	SoundHandle(fSoundPlayerManager:: * _fPlaySound)(SoundHandle,
		uint32_t,
		SoundType,
		SoundFlags,
		Dimps::Math::Vec4F*) = &PlaySound;
	void (fSoundPlayerManager:: * _fStopSound)(SoundHandle, BOOL) = &StopSound;
	void (fSoundPlayerManager:: * _fStopAll)(BOOL) = &StopAll;

	DetourAttach((PVOID*)&rSoundPlayerManager::publicMethods.destructor, *(PVOID*)&_fDestructor);
	DetourAttach((PVOID*)&rSoundPlayerManager::publicMethods.PlaySound, *(PVOID*)&_fPlaySound);
	DetourAttach((PVOID*)&rSoundPlayerManager::publicMethods.StopSound, *(PVOID*)&_fStopSound);
	DetourAttach((PVOID*)&rSoundPlayerManager::publicMethods.StopAll, *(PVOID*)&_fStopAll);

	DetourAttach((PVOID*)&rSoundPlayerManager::staticMethods.Factory, Factory); 

	CriPlayerAdapter::Install();
}

// Reset the tracked metadata for every adapter owned by this manager.
// A freshly created manager's adapters can land on heap addresses that
// a previous battle's adapters occupied; any metadata surviving at those
// keys (see the purge in the destructor, which this backstops) would
// attach stale sounds to brand-new adapters.
static void ResetAdapterMetadata(rSoundPlayerManager* m) {
	rSoundPlayerManager::CriPlayerAdapter* adapters = *rSoundPlayerManager::GetAdapters(m);
	for (int i = 0; i < *rSoundPlayerManager::GetNumAdapters(m); i++) {
		fSoundPlayerManager::adapterToCurrentSound[&adapters[i]] = fSoundPlayerManager::DeferredSoundRequest{};
	}
}

rSoundPlayerManager* fSoundPlayerManager::Factory(DWORD param_1, DWORD param_2, int* numAdapters) {
	if (!bUsePureSounds) {
		rSoundPlayerManager* out = rSoundPlayerManager::staticMethods.Factory(param_1, param_2, numAdapters);
		shadowManagerMap[out] = NULL;
		ResetAdapterMetadata(out);
		return out;
	}

	bool _bAllowNewPlayers = Platform::Sound::bAllowNewPlayers;
	Platform::Sound::bAllowNewPlayers = false;
	rSoundPlayerManager* stub = rSoundPlayerManager::staticMethods.Factory(param_1, param_2, numAdapters);
	Platform::Sound::bAllowNewPlayers = true;
	rSoundPlayerManager* real = rSoundPlayerManager::staticMethods.Factory(param_1, param_2, numAdapters);
	Platform::Sound::bAllowNewPlayers = _bAllowNewPlayers;
	shadowManagerMap[stub] = real;
	queuedStops[stub] = std::vector<DeferredSoundRequest>();
	ResetAdapterMetadata(stub);
	ResetAdapterMetadata(real);
	return stub;
}

void fSoundPlayerManager::destructor(BOOL param_1) {
	// Purge this manager's adapters (and its shadow's) from the metadata
	// map before the adapter arrays are freed. Entries left behind would
	// be keyed by dangling pointers- and because the heap likes to reuse
	// same-sized blocks, the next battle's adapters frequently land on
	// these exact addresses and inherit stale "live" sounds with handles
	// into cue sheets that no longer exist.
	if (bUsePureSounds) {
		rSoundPlayerManager* real = shadowManagerMap[this];
		if (real) {
			rSoundPlayerManager::CriPlayerAdapter* realAdapters = *rSoundPlayerManager::GetAdapters(real);
			for (int i = 0; i < *rSoundPlayerManager::GetNumAdapters(real); i++) {
				adapterToCurrentSound.erase(&realAdapters[i]);
			}
			(real->*rSoundPlayerManager::publicMethods.destructor)(param_1);
		}
	}
	rSoundPlayerManager::CriPlayerAdapter* adapters = *rSoundPlayerManager::GetAdapters(this);
	for (int i = 0; i < *rSoundPlayerManager::GetNumAdapters(this); i++) {
		adapterToCurrentSound.erase(&adapters[i]);
	}
	queuedStops.erase(this);
	shadowManagerMap.erase(this);
	(this->*rSoundPlayerManager::publicMethods.destructor)(param_1);
}

SoundHandle fSoundPlayerManager::PlaySound(
	SoundHandle cueSheetHandle,
	uint32_t cueIdx,
	SoundType type,
	SoundFlags flags,
	Dimps::Math::Vec4F* position
) {
	SoundHandle out = (this->*rSoundPlayerManager::publicMethods.PlaySound)(
		cueSheetHandle,
		cueIdx,
		type,
		flags,
		position
	);

	if (bTrackRequests) {
		SoundReference cueSheetRef = SoundReference::FromHandle(cueSheetHandle);
		spdlog::warn(
			"SoundPlayerManager sound requested: {} {} {} {} {} {} {} {} {} {} and got {}",
			cueSheetRef.index,
			cueSheetRef.useCount,
			cueIdx,
			type,
			flags,
			(void*)position,
			position ? position->x : 0,
			position ? position->y : 0,
			position ? position->z : 0,
			position ? position->w : 0,
			out
		);
		if (bWarnOnOverflow && out == 0xffffffff) {
			MessageBox(NULL, "Sound player overflow", NULL, MB_OK);
		}
	}

	if (bUsePureSounds) {
		if (out == 0xffffffff) {
			// The sound could not be played because there were no players
			// available to serve the request. Stop the oldest sound in this
			// manager, then play this one again.
			//
			// XXX (adanducci): I'm actually quite sure this case can never occur.
			// The vast majority of sound effects use the interruptable flag,
			// and the VO infrastructure already explicitly stops the existing
			// VO sound if one exists.
			DeferredSoundRequest* oldestReq = NULL;
			rSoundPlayerManager::CriPlayerAdapter* adapters = *rSoundPlayerManager::GetAdapters(this);
			for (int i = 0; i < *rSoundPlayerManager::GetNumAdapters(this); i++) {
				DeferredSoundRequest* req = &adapterToCurrentSound[&adapters[i]];
				if (!req->bLive) {
					continue;
				}
				if (oldestReq == NULL || req->nFrame < oldestReq->nFrame) {
					oldestReq = req;
				}
			}

			if (oldestReq != NULL) {
				this->StopSound(oldestReq->currentAdapterHandle, 1);
				out = (this->*rSoundPlayerManager::publicMethods.PlaySound)(
					cueSheetHandle,
					cueIdx,
					type,
					flags,
					position
				);
			}
		}

		if (out == 0xffffffff) {
			// The play failed and freeing a player either wasn't possible
			// or didn't help- e.g. the request itself is invalid (a cue
			// sheet handle from a torn-down battle). Don't record metadata
			// for a sound that isn't playing: decoding this handle would
			// produce adapter index 0xffff, far outside the adapter array.
			return out;
		}

		SoundReference adapterReference = SoundReference::FromHandle(out);
		assert(adapterReference.index < *rSoundPlayerManager::GetNumAdapters(this));
		rSoundPlayerManager::CriPlayerAdapter* adapter = &(*rSoundPlayerManager::GetAdapters(this))[adapterReference.index];
		DeferredSoundRequest& meta = adapterToCurrentSound[adapter];
		if (meta.bLive) {
			// If this adapter was already live, the successful `PlaySound`
			// call must have interrupted an existing sound. The syncing
			// step can't really differentiate between interrupts and
			// stops, so just treat it as a stop. Only stub managers queue
			// stops- for the real manager (reached via SyncState), the
			// interrupt already took effect and nothing ever drains a
			// real manager's queue.
			if (shadowManagerMap.count(this)) {
				queuedStops[this].push_back(meta);
			}
		}
		meta.bLive = true;
		meta.nFrame = System::GetNumFramesSimulated_FixedPoint(System::staticMethods.GetSingleton())->integral;
		meta.currentAdapterHandle = out;
		meta.cueSheetHandle = cueSheetHandle;
		meta.cueIdx = cueIdx;
		meta.type = type;
		meta.flags = flags;
		meta.position = position;
	}

	return out;
};

void fSoundPlayerManager::StopSound(SoundHandle adapterHandle, BOOL criParam) {
	if (bUsePureSounds) {
		SoundReference adapterReference = SoundReference::FromHandle(adapterHandle);
		rSoundPlayerManager::CriPlayerAdapter* adapter = &(*rSoundPlayerManager::GetAdapters(this))[adapterReference.index];
		DeferredSoundRequest& meta = adapterToCurrentSound[adapter];
		meta.bLive = false;
		if (shadowManagerMap.count(this)) {
			queuedStops[this].push_back(meta);
		}
	}

	rSoundPlayerManager* _this = this;
	(this->*rSoundPlayerManager::publicMethods.StopSound)(adapterHandle, criParam);
}

void fSoundPlayerManager::StopAll(BOOL criParam) {
	if (bUsePureSounds) {
		rSoundPlayerManager::CriPlayerAdapter* adapters = *rSoundPlayerManager::GetAdapters(this);
		bool bIsStub = shadowManagerMap.count(this) > 0;
		for (int i = 0; i < *rSoundPlayerManager::GetNumAdapters(this); i++) {
			DeferredSoundRequest& meta = adapterToCurrentSound[&adapters[i]];
			meta.bLive = false;
			if (bIsStub) {
				queuedStops[this].push_back(meta);
			}
		}
		return;
	}

	rSoundPlayerManager* _this = this;
	(this->*rSoundPlayerManager::publicMethods.StopAll)(criParam);
}

void fSoundUnit::Install() {
	BOOL(fSoundUnit:: * _fIsStillPlaying)(uint32_t, uint32_t) = &IsStillPlaying;
	DetourAttach((PVOID*)&rSoundUnit::publicMethods.IsStillPlaying, *(PVOID*)&_fIsStillPlaying);
}

BOOL fSoundUnit::IsStillPlaying(uint32_t managerIdx, uint32_t adapterHandle) {
	MessageBoxA(NULL, "fSoundUnit::IsStillPlaying- method was suspected dead, but must be implemented", NULL, MB_OK);
	return FALSE;
}

bool fSoundPlayerManager::DeferredSoundRequest::IsEqual(DeferredSoundRequest* lhs, DeferredSoundRequest* rhs) {
	return (
		lhs->cueIdx == rhs->cueIdx &&
		lhs->cueSheetHandle == rhs->cueSheetHandle &&
		lhs->flags == rhs->flags &&
		lhs->type == rhs->type &&
		lhs->position == rhs->position
	);
}

void fSoundPlayerManager::SyncState() {
	for (auto managerIter = shadowManagerMap.begin(); managerIter != shadowManagerMap.end(); managerIter++) {
		rSoundPlayerManager* stubManager = managerIter->first;
		rSoundPlayerManager* realManager = managerIter->second;
		rSoundPlayerManager::CriPlayerAdapter* stubPlayers = *rSoundPlayerManager::GetAdapters(stubManager);
		rSoundPlayerManager::CriPlayerAdapter* realPlayers = *rSoundPlayerManager::GetAdapters(realManager);

		// If a sound in the stub manager was imperatively stopped, stop
		// up to one corresponding sound that is actively playing. A stop
		// targets a specific sound *instance*, so a corresponding sound
		// must match both the request arguments and the frame the sound
		// started on (real-side metadata mirrors the stub's start frame,
		// see the reconciliation phase below). If no corresponding sound
		// is actively playing, the target instance was already stopped-
		// either predictively during forward simulation, or by this same
		// stop being applied before a rollback that re-simulated (and
		// therefore re-queued) it. Re-applying such a stop by matching on
		// arguments alone would kill an unrelated instance of the same
		// cue- typically the freshly retriggered copy- which the
		// reconciliation phase would then audibly restart from the
		// beginning.
		for (auto iter = queuedStops[stubManager].begin(); iter != queuedStops[stubManager].end(); iter++) {
			DeferredSoundRequest stoppedSound = *iter;
			for (int i = 0; i < *rSoundPlayerManager::GetNumAdapters(realManager); i++) {
				rSoundPlayerManager::CriPlayerAdapter* realPlayer = &realPlayers[i];
				DeferredSoundRequest* realSound = &adapterToCurrentSound[realPlayer];
				if (!realSound->bLive) {
					continue;
				}

				if (
					DeferredSoundRequest::IsEqual(&stoppedSound, realSound) &&
					realSound->nFrame == stoppedSound.nFrame
				) {
					((fSoundPlayerManager*)realManager)->StopSound(realSound->currentAdapterHandle, 1);
					break;
				}
			}
		}

		// If a sound is playing but it's not supposed to be, stop it. This
		// step has to happen first in order to free up players for playing
		// sounds later.
		for (int i = 0; i < *rSoundPlayerManager::GetNumAdapters(realManager); i++) {
			rSoundPlayerManager::CriPlayerAdapter* realPlayer = &realPlayers[i];
			DeferredSoundRequest* realSound = &adapterToCurrentSound[realPlayer];
			if (!realSound->bLive) {
				continue;
			}

			bool shouldStop = true;
			for (int j = 0; j < *rSoundPlayerManager::GetNumAdapters(stubManager); j++) {
				rSoundPlayerManager::CriPlayerAdapter* stubPlayer = &stubPlayers[j];
				DeferredSoundRequest* stubSound = &adapterToCurrentSound[stubPlayer];
				if (!stubSound->bLive) {
					continue;
				}
				if (DeferredSoundRequest::IsEqual(stubSound, realSound)) {
					shouldStop = false;
					break;
				}
			}
			if (shouldStop) {
				((fSoundPlayerManager*)realManager)->StopSound(realSound->currentAdapterHandle, 1);
			}
		}

		// Finally, reconcile playing sounds. Playing sounds fall into two groups-
		// sounds that should be playing but aren't yet playing, and sounds that
		// should be playing and are already playing.
		// 
		// * If a sound should be playing and is already playing, update
		//   the parameters of the real player from the stub player containing the
		//   sound- this ensures that things like fades are handled appropriately.
		// * If a sound isn't yet playing but should be, play it. This has to
		//   happen after stopping sounds, or there won't be enough players for
		//   all sounds to play.
		//
		// Because the reconciliation is completely decoupled from the original
		// sounds, be sure to ensure all the logic here is consistent and that
		// identical sounds playing in parallel results in updating two different
		// adapters, instead of the same one.
		std::set<int> claimedAdapters; // This is inefficient, but handles arbitrary numbers of
		                               // adapters perfectly. If efficiency becomes a problem,
		                               // consider just bitmasking this.
		for (int i = 0; i < *rSoundPlayerManager::GetNumAdapters(stubManager); i++) {
			rSoundPlayerManager::CriPlayerAdapter* stubPlayer = &stubPlayers[i];
			DeferredSoundRequest* stubSound = &adapterToCurrentSound[stubPlayer];
			if (!stubSound->bLive) {
				continue;
			}

			int targetAdapter = -1;
			for (int j = 0; j < *rSoundPlayerManager::GetNumAdapters(stubManager); j++) {
				rSoundPlayerManager::CriPlayerAdapter* realPlayer = &realPlayers[j];
				DeferredSoundRequest* realSound = &adapterToCurrentSound[realPlayer];
				if (!realSound->bLive) {
					continue;
				}
				if (!DeferredSoundRequest::IsEqual(stubSound, realSound)) {
					continue;
				}

				if (claimedAdapters.count(j) > 0) {
					continue;
				}

				claimedAdapters.insert(j);
				targetAdapter = j;
				break;
			}
			if (targetAdapter == -1) {
				SoundHandle playerHandle = ((fSoundPlayerManager*)realManager)->PlaySound(
					stubSound->cueSheetHandle,
					stubSound->cueIdx,
					stubSound->type,
					stubSound->flags,
					stubSound->position
				);
				if (playerHandle == 0xffffffff) {
					// The real manager couldn't serve the request- most
					// likely the stub sound carries a stale handle (e.g.
					// a cue sheet from a previous battle). Skip this sound
					// rather than decoding the failure sentinel into an
					// adapter index (0xffff) and scribbling far past the
					// adapter array. This was previously only an assert,
					// which NDEBUG builds compile out.
					spdlog::warn(
						"SyncState: real PlaySound failed for cue {} (sheet {:#x}); skipping reconcile",
						stubSound->cueIdx,
						stubSound->cueSheetHandle
					);
					continue;
				}
				SoundReference playerRef = SoundReference::FromHandle(playerHandle);
				targetAdapter = playerRef.index;
			}
			rSoundPlayerManager::CriPlayerAdapter* realPlayer = &realPlayers[targetAdapter];
			// Mirror the stub instance's start frame onto the real-side
			// metadata. The stop phase above uses it to target the exact
			// instance a queued stop was issued against; adopting the
			// stub's frame here (rather than the wall-clock frame the
			// real sound happened to start on) keeps that association
			// stable across rollbacks that shift a sound's start frame.
			adapterToCurrentSound[realPlayer].nFrame = stubSound->nFrame;
			// XXX (adanducci): This is extremely likely to be broken.
			realPlayer->flags = stubPlayer->flags;
			realPlayer->position = stubPlayer->position;
			realPlayer->volume = stubPlayer->volume;
			realPlayer->fadeScale = stubPlayer->fadeScale;
			realPlayer->playState = stubPlayer->playState;
			realPlayer->field6_0x18 = stubPlayer->field6_0x18;
		}

		queuedStops[stubManager].clear();
		(realManager->*rSoundPlayerManager::publicMethods.Update)();
	}
}

void fSoundPlayerManager::CriPlayerAdapter::Install() {
	BOOL(CriPlayerAdapter:: * _fIsStillPlaying)() = &IsStillPlaying;
	DetourAttach((PVOID*)&rSoundPlayerManager::CriPlayerAdapter::publicMethods.IsStillPlaying, *(PVOID*)&_fIsStillPlaying);
}

BOOL fSoundPlayerManager::CriPlayerAdapter::IsStillPlaying() {
	if (bUsePureSounds) {
		DeferredSoundRequest* stubSound = &adapterToCurrentSound[this];
		return stubSound->bLive;
	}

	return (this->*rSoundPlayerManager::CriPlayerAdapter::publicMethods.IsStillPlaying)();
}