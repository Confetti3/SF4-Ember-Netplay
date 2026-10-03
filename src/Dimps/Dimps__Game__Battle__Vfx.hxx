#pragma once

#include <windows.h>
#include <string>

#include "Dimps__Eva.hxx"
#include "Dimps__Game.hxx"
#include "Dimps__Math.hxx"
#include "Dimps__Platform.hxx"

using Dimps::Game::GameMementoKey;
using Dimps::Math::FixedPoint;

namespace Dimps {
	namespace Game {
		namespace Battle {
			namespace Vfx {
				void Locate(HMODULE peRoot);

				interface IContainer { };

				enum EffectHandleType {
					EHT_OBJ_RESERVED = 0x40000,
					EHT_PARTICLE = 0x1000000,
					EHT_TRACE = 0x2000000,
				};

				const DWORD INVALID_EFFECT_HANDLE = 0xffffffff;

				struct ColorFadeData
				{
					void* resourcePtr;
					DWORD flags;
					float unk_float_data[12];
					short unk_useCount;
					short unk_58;
					FixedPoint unk_60;
					char unk_64[8];
				};

				struct ColorFade
				{
					typedef struct __publicMethods {
						Platform::list_entry<ColorFadeData>* (ColorFade::* Spawn)(void* sourceData);
					} __publicMethods;

					static Platform::list<ColorFadeData>* GetList(ColorFade* fade);
					static void Locate(HMODULE peRoot);

					static __publicMethods publicMethods;
				};

				struct ColorFadeUnit
				{
					static void Locate(HMODULE peRoot);

					typedef struct __publicMethods {
						ColorFade* (ColorFadeUnit::* GetFade)(int index);
					} __publicMethods;

					typedef struct __staticMethods {
						ColorFadeUnit* (*GetSingleton)();
					} __staticMethods;

					static Eva::Task** GetColorFadeMainUpdateTask(ColorFadeUnit* u);

					static __publicMethods publicMethods;
					static __staticMethods staticMethods;
				};

				struct Object
				{
					static void Locate(HMODULE peRoot);
					static std::string* GetNameTmp(Object* o);

					static BYTE* recordCopyCount;
					static BYTE* restoreCopyCount;
				};

				struct ObjectContainer : IContainer
				{
					static const unsigned int DEFAULT_LOOSE_OBJECT_COUNT;
					static const unsigned int RESERVED_OBJECT_COUNT;

					typedef struct __publicMethods {
						Object* (ObjectContainer::* GetObjectFromHandle)(DWORD handle);
					} __publicMethods;

					static void Locate(HMODULE peRoot);

					// While nothing that this project does is truly safe, this is pushing the
					// boundaries. The VFX handles should be opaque, but we can synthesize
					// fake Object handles to look up arbitrary Objects with
					// GetObjectFromHandle.
					static DWORD GenerateFakeHandle(unsigned int index, bool isReservedObject);
					static __publicMethods publicMethods;
				};

				struct Particle
				{
					static void Locate(HMODULE peRoot);
					static std::string* GetNameTmp(Particle* o);
				};

				struct ParticleContainer : IContainer
				{
					static const unsigned int DEFAULT_PARTICLE_COUNT;

					typedef struct __publicMethods {
						Particle* (ParticleContainer::* GetParticleFromHandle)(DWORD handle);
					} __publicMethods;

					static void Locate(HMODULE peRoot);

					// While nothing that this project does is truly safe, this is pushing the
					// boundaries. The VFX handles should be opaque, but we can synthesize
					// fake Particle handles to look up arbitrary Particles with
					// GetParticleFromHandle.
					static DWORD GenerateFakeHandle(unsigned int index);
					static __publicMethods publicMethods;
				};

				struct Trace
				{
					static void Locate(HMODULE peRoot);
					static std::string* GetNameTmp(Trace* o);
				};

				struct TraceContainer : IContainer
				{
					static const unsigned int DEFAULT_TRACE_COUNT;

					typedef struct __publicMethods {
						Trace* (TraceContainer::* GetTraceFromHandle)(DWORD handle);
					} __publicMethods;

					static void Locate(HMODULE peRoot);

					// While nothing that this project does is truly safe, this is pushing the
					// boundaries. The VFX handles should be opaque, but we can synthesize
					// fake Trace handles to look up arbitrary Traces with
					// GetTraceFromHandle.
					static DWORD GenerateFakeHandle(unsigned int index);
					static __publicMethods publicMethods;
				};

				struct Unit
				{
					typedef struct __privateMethods {
						// TODO
					} __privateMethods;

					typedef struct __publicMethods {
						IContainer* (Unit::* GetContainerByType)(DWORD type);
						void (Unit::* RecordToInternalMementoKey)(GameMementoKey::MementoID* id);
						void (Unit::* RestoreFromInternalMementoKey)(GameMementoKey::MementoID* id);
					} __publicMethods;

					typedef struct __staticMethods {
						// TODO
					} __staticMethods;


					static Eva::Task** GetVfxMainUpdateTask(Unit* s);
					static void Locate(HMODULE peRoot);
					static __privateMethods privateMethods;
					static __publicMethods publicMethods;
					static __staticMethods staticMethods;

					enum ContainerType {
						CT_OBJECT = 0,
						CT_PARTICLE = 1,
						CT_TRACE = 2
					};
					// Instance values here
				};
			}
		}
	}
}