#pragma once

#include <windows.h>

#include "Dimps__Game.hxx"
#include "Dimps__Math.hxx"

namespace Dimps {
	namespace Game {
		namespace Battle {
			namespace Chara {
				using Dimps::Game::GameMementoKey;

				void Locate(HMODULE peRoot);

				struct Actor
				{
					enum Status {
						AS_STAND = 0,
						AS_CROUCH = 1,
						AS_JUMP = 2,
						AS_STAND_TO_CROUCH = 3,
						AS_CROUCH_TO_STAND = 4,
						AS_STAND_TO_JUMP = 5,
						AS_JUMP_TO_STAND = 6,
						AS_TURN__STAND = 7,
						AS_TURN__CROUCH = 8,
						AS_TURN__WALK = 9,
						AS_FORWARD = 10,
						AS_BACKWARD = 11,
						AS_FRONTDASH = 12,
						AS_BACKDASH = 13,
						AS_GUARD__STAND = 14,
						AS_GUARD__CROUCH = 15,
						AS_SKILL = 16,
						AS_STUN = 17,
						AS_BOUND = 18,
						AS_DOWN = 19,
						AS_RISE = 20,
						AS_DAMAGE = 21,
						AS_DAMAGE__GUARD = 22,
						AS_DAMAGE__BLOW = 23,
						AS_SEQUENCE = 24
					};

					enum BodyPart {
						BP_WAIST = 0,
						BP_STOMACH = 1,
						BP_CHEST = 2,
						BP_HEAD = 3,
						BP_LARM1 = 4,
						BP_LARM2 = 5,
						BP_LHANDROT = 6,
						BP_LHN = 7,
						BP_LLEG1 = 8,
						BP_LLEG2 = 9,
						BP_LFOOT = 10,
						BP_LFN = 11,
						BP_RARM1 = 12,
						BP_RARM2 = 13,
						BP_RHANDROT = 14,
						BP_RHN = 15,
						BP_RLEG1 = 16,
						BP_RLEG2 = 17,
						BP_RFOOT = 18,
						BP_RFN = 19,
						BP_LHOPTION = 20,
						BP_LFOOT_2 = 21,
						BP_RHOPTION = 22,
						BP_RFOOT_2 = 23,
						BP_NECK = 24,
						BP_TRS = 25,
						BP_FACE_LEYE = 26,
						BP_FACE_REYE = 27,
						BP_LND = 28,
						BP_WAN = 29,
						BP_HEN_1 = 30,
						BP_HEOPTION = 31,
						BP_LARMEFF = 32,
						BP_RARMEFF = 33,
						BP_LLEGEFF = 34,
						BP_RLEGEFF = 35,
						BP_HEN_2 = 36,
					};

					enum JumpType {
						JUMP_VERTICAL = 0,
						JUMP_FORWARD = 1,
						JUMP_BACKWARD = 2
					};

					enum ActorID {
						ID_RYU = 0,
						ID_KEN = 1,
						ID_CNL = 2,
						ID_HND = 3,
						ID_BLK = 4,
						ID_ZGF = 5,
						ID_GUL = 6,
						ID_DSM = 7,
						ID_BSN = 8,
						ID_BLR = 9,
						ID_SGT = 10,
						ID_VEG = 11,
						ID_AGL = 12,
						ID_CHB = 13,
						ID_RIC = 14,
						ID_JHA = 15,
						ID_BOS = 16,
						ID_GKI = 17,
						ID_GKN = 18,
						ID_HWK = 19,
						ID_CMY = 20,
						ID_FLN = 21,
						ID_DJY = 22,
						ID_SKR = 23,
						ID_ROS = 24,
						ID_GEN = 25,
						ID_DAN = 26,
						ID_GUY = 27,
						ID_CDY = 28,
						ID_IBK = 29,
						ID_MKT = 30,
						ID_DDL = 31,
						ID_ADN = 32,
						ID_HKN = 33,
						ID_JRI = 34,
						ID_YUN = 35,
						ID_YAN = 36,
						ID_RYX = 37,
						ID_GKX = 38,
						ID_RLN = 39,
						ID_ELN = 40,
						ID_PSN = 41,
						ID_HUG = 42,
						ID_DCP = 43,
					};

					typedef struct __publicMethods {
						unsigned int (Actor::* GetActorID)();
						int (Actor::* GetComboDamage)(Dimps::Math::FixedPoint* out);
						int (Actor::* GetCurrentSide)();
						int (Actor::* GetDamage)(Dimps::Math::FixedPoint* out);
                        unsigned int (Actor::* GetStatus)();
                        // Native debug queries getAction/getCharacterFrame use
                        // these inherited Action::Actor readers (vtable +50/+54).
                        int (Actor::* GetActionID)();
                        Dimps::Math::FixedPoint* (Actor::* GetActionFrame)(Dimps::Math::FixedPoint* out);
                        // Used by native recovery dispatch (0 standing, 1 crouching).
                        int (Actor::* GetActionPosture)();
                        // Nonzero makes the native basic-action evaluator return early.
                        int (Actor::* GetBasicActionInhibit)();
                        // Resolves an action ID in the actor's current BAC bank.
                        const unsigned short* (Actor::* GetActionScript)(int action);
                        // The command (.bcm) move the actor's Skill plays (actor +0x2188),
                        // null on any other status; its category is at +0x28.
                        const unsigned char* (Actor::* GetCommandMove)();
						// Frames until the actor can act again, hitstop included, as the
						// game keeps them for its input buffer: the s16 at actor +0x6E7A.
						int (Actor::* GetFramesUntilActionable)();

						void (Actor::* GetVitalityAmt_FixedPoint)(Dimps::Math::FixedPoint* out);
						void (Actor::* GetVitalityMax_FixedPoint)(Dimps::Math::FixedPoint* out);
						void (Actor::* GetVitalityPct_FixedPoint)(Dimps::Math::FixedPoint* out);

						void (Actor::* GetRevengeAmt_FixedPoint)(Dimps::Math::FixedPoint* out);
						void (Actor::* GetRevengeMax_FixedPoint)(Dimps::Math::FixedPoint* out);
						void (Actor::* GetRevengePct_FixedPoint)(Dimps::Math::FixedPoint* out);

						void (Actor::* GetRecoverableVitalityAmt_FixedPoint)(Dimps::Math::FixedPoint* out);
						void (Actor::* GetRecoverableVitalityMax_FixedPoint)(Dimps::Math::FixedPoint* out);
						void (Actor::* GetRecoverableVitalityPct_FixedPoint)(Dimps::Math::FixedPoint* out);

						void (Actor::* GetSuperComboAmt_FixedPoint)(Dimps::Math::FixedPoint* out);
						void (Actor::* GetSuperComboMax_FixedPoint)(Dimps::Math::FixedPoint* out);
						void (Actor::* GetSuperComboPct_FixedPoint)(Dimps::Math::FixedPoint* out);

						void (Actor::* GetSCTimeAmt_FixedPoint)(Dimps::Math::FixedPoint* out);
						void (Actor::* GetSCTimeMax_FixedPoint)(Dimps::Math::FixedPoint* out);
						void (Actor::* GetSCTimePct_FixedPoint)(Dimps::Math::FixedPoint* out);

						void (Actor::* GetUCTimeAmt_FixedPoint)(Dimps::Math::FixedPoint* out);
						void (Actor::* GetUCTimeMax_FixedPoint)(Dimps::Math::FixedPoint* out);
						void (Actor::* GetUCTimePct_FixedPoint)(Dimps::Math::FixedPoint* out);

						float* (Actor::* GetCurrentRootPosition)();
						float* (Actor::* GetCurrentBonePositionByID)(int boneID);
					} __publicMethods;

					typedef struct __staticMethods {
						void (*ResetAfterMemento)(Actor* a);
						char* (*GetBoneLabelByID)(int idx);
						// The locator a body part's strike box ends at, -1 past the last.
						int (*GetLocatorPairEnd)(unsigned int part);
					} __staticMethods;

					// A fighter's body boxes: where a strike lands, one a part of the
					// body from its own locator to GetLocatorPairEnd's, and where a throw connects.
					enum class BodyList { Strike, Throw };
					// One of a list's boxes, 0xA0 bytes, four corners first.
					static unsigned char* GetBodyBox(Actor* a, BodyList list, unsigned index);
					// Which boxes of a list count on this frame, one bit a box: the
					// fighter's defaults as the action's script, invincibility and facing leave them.
					static unsigned int GetBodyMask(Actor* a, BodyList list);
					// The first of an attack's boxes in a list (BOXES_*), null when
					// empty; count: how many. object: an Actor, or an effect object
					// (Effect::Unit::GetFirstEffect), which keeps its lists the same way.
					static unsigned char* GetAttackBoxes(const void* object, unsigned list, unsigned& count);
					enum { BOXES_GUARD = 0, BOXES_HIT = 1, BOXES_THROW = 2 };
					// In an attack box: the next box, and its state, BOX_LIVE while it can touch.
					enum { BOX_NEXT = 0xA8, BOX_STATE = 0xAC, BOX_LIVE = 2 };
					enum { BODY_STRIKE_COUNT = 24, BODY_THROW_COUNT = 4 };

					static void Locate(HMODULE peRoot);
					static __publicMethods publicMethods;
					static __staticMethods staticMethods;

					// Instance values here
				};

				// Each actor owns two (actor +0x6EEC and +0x6EF0). Derives from
				// Action::Actor and overrides its whole memento interface
				// (vtable 0x94fafc), called through the IMementoable subobject
				// at +0x60. Its GameMementoKey sits at object +0x6B10.
				struct Afterimage
				{
					typedef struct __mementoableMethods {
						size_t (Afterimage::* GetMementoSize)();
						int (Afterimage::* RecordToMemento)(void* memento, GameMementoKey::MementoID* id);
						int (Afterimage::* RestoreFromMemento)(void* memento, GameMementoKey::MementoID* id);
					} __mementoableMethods;

					static void Locate(HMODULE peRoot);
					// The IMementoable subobject is at object +0x60 and the key at +0x6B10.
					static GameMementoKey* GetKey(Afterimage* mementoable) {
						return (GameMementoKey*)((unsigned int)mementoable - 0x60 + 0x6b10);
					}
					static __mementoableMethods mementoableMethods;
				};

				struct Unit
				{
					typedef struct __publicMethods {
						Actor* (Unit::* GetActorByIndex)(unsigned int index);

						BOOL(Unit::* CanStoreMemento_MaybeActorExists)();
						void (Unit::* RecordToInternalMementoKey)(GameMementoKey::MementoID* id);
						void (Unit::* RestoreFromInternalMementoKey)(GameMementoKey::MementoID* id);
					} __publicMethods;

					static void Locate(HMODULE peRoot);
					static __publicMethods publicMethods;

					// Instance values here
					void* vtable;
					DWORD pad_0x4;
					Actor* playerActors[2];
					void* actors_0x10[2];
					void* actors_0x18[2];
					void* lpItemCollection;
				};
			}
		}
	}
}