#pragma once

#include <set>

#include "../Dimps/Dimps__Game.hxx"

#include "sf4e.hxx"

namespace sf4e {
	namespace Game {
		void Install();

		// Set when a record or restore could not capture, or put back,
		// everything it owns: a task core it cannot represent (sf4e.cxx) or an
		// afterimage whose Actor state could not be appended or found
		// (sf4e__Game__Battle__Chara.cxx). The engine's memento chain has no
		// error path, so only SaveState::Save and SaveState::Load reset and
		// read these, and report them through their return values; every
		// other caller keeps the engine's own behaviour.
		struct MementoFailure {
			static bool record;
			static bool restore;
		};

		struct GameMementoKey : Dimps::Game::GameMementoKey
		{
			void Initialize(void* mementoable, int numMementos);
			void ClearKey();
			static void Install();
			static void GenerateChecksums();

			static std::set<Dimps::Game::GameMementoKey*> trackedKeys;
		};

		namespace Sprite {
			struct Control : Dimps::Game::Sprite::Control {
				// These methods cannot be safely implemented! The ::Control class
				// has several unimplemented virtual methods, and all subclasses
				// of Control implement Enable() and Disable(), which would need
				// to be called as part of these methods.
				//
				// static void RecordToAdditionalMemento(Dimps::Game::Sprite::SingleNodeControl* n, AdditionalMemento& m);
				// static void RestoreFromAdditionalMemento(Dimps::Game::Sprite::SingleNodeControl* n, const AdditionalMemento& m);
			};

			struct SingleNodeControl : Dimps::Game::Sprite::SingleNodeControl {
				struct AdditionalMemento {
					bool enabled;
					int currentFrame;
				};

				static void RecordToAdditionalMemento(Dimps::Game::Sprite::SingleNodeControl* n, AdditionalMemento& m);
				static void RestoreFromAdditionalMemento(Dimps::Game::Sprite::SingleNodeControl* n, const AdditionalMemento& m);
			};
		}
	}
}