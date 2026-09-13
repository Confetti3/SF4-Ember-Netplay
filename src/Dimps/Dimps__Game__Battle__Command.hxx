#pragma once

#include <windows.h>

#include "Dimps__Game.hxx"

namespace Dimps {
	namespace Game {
		namespace Battle {
			namespace Command {
				using Dimps::Game::GameMementoKey;

				void Locate(HMODULE peRoot);

				struct CommandImpl {
					/**
					 * The order in which basic actions is checked is as follows, listed in bitfield
					 * positions and not masks:
					 * FJump
					 * BJump
					 * NJump
					 * FDash
					 * BDash
					 * UNK10,
					 * UNK9,
					 * Crouch
					 * FWalk
					 * BWalk
					 * Neutral: Note that if neutral is a position you can
					 *   enter, eval will always end here. If neutral is
					 *   disallowed, checks proceed.
					 * UNK13
					 * UNK14
					 * Crouch2
					 * NoCrouch
					 */

					enum BasicActionPosition {
						BAPOS_NEUTRAL = 0, // Least significant bit of the mask.
						BAPOS_CROUCH = 1,
						BAPOS_NJUMP = 2,
						BAPOS_FJUMP = 3,
						BAPOS_BJUMP = 4,
						BAPOS_FWALK = 5,
						BAPOS_BWALK = 6,
						BAPOS_FDASH = 7,
						BAPOS_BDASH = 8,
						BAPOS_UNK9 = 9,
						BAPOS_UNK10 = 10,
						BAPOS_NOCROUCH = 11,
						BAPOS_CROUCH2 = 12,
						BAPOS_UNK13 = 13,
						BAPOS_UNK14 = 14,
					};

					enum BasicActionMask {
						BAMASK_NEUTRAL = 0x1,
						BAMASK_CROUCH = 0x2,
						BAMASK_NJUMP = 0x4,
						BAMASK_FJUMP = 0x8,
						BAMASK_BJUMP = 0x10,
						BAMASK_FWALK = 0x20,
						BAMASK_BWALK = 0x40,
						BAMASK_FDASH = 0x80,
						BAMASK_BDASH = 0x100,
						BAMASK_UNK9 = 0x200,
						BAMASK_UNK10 = 0x400,
						BAMASK_NOCROUCH = 0x800,
						BAMASK_CROUCH2 = 0x1000,
						BAMASK_UNK13 = 0x2000,
						BAMASK_UNK14 = 0x4000,
					};

					typedef struct __publicMethods {
						DWORD(CommandImpl::* GetCurrentOnSwitches)();
						DWORD(CommandImpl::* GetCurrentRisingSwitches)();
						DWORD(CommandImpl::* GetCurrentFallingSwitches)();
					} __publicMethods;

					static void Locate(HMODULE peRoot);
					static __publicMethods publicMethods;
				};

				struct Unit
				{
					typedef struct __publicMethods {
						CommandImpl* (Unit::* GetCommandImplForEntry)(unsigned int index);

						void (Unit::* RecordToInternalMementoKey)(GameMementoKey::MementoID* id);
						void (Unit::* RestoreFromInternalMementoKey)(GameMementoKey::MementoID* id);
					} __publicMethods;

					static void Locate(HMODULE peRoot);
					static __publicMethods publicMethods;
				};
			}
		}
	}
}
