#include <windows.h>

#include "Dimps__Eva.hxx"
#include "Dimps__Game__Battle.hxx"
#include "Dimps__Game__Battle__Action.hxx"
#include "Dimps__Game__Battle__Camera.hxx"
#include "Dimps__Game__Battle__Chara.hxx"
#include "Dimps__Game__Battle__Command.hxx"
#include "Dimps__Game__Battle__Effect.hxx"
#include "Dimps__Game__Battle__Hud.hxx"
#include "Dimps__Game__Battle__System.hxx"
#include "Dimps__Game__Battle__Training.hxx"
#include "Dimps__Game__Battle__Vfx.hxx"
#include "Dimps__Platform.hxx"

namespace Battle = Dimps::Game::Battle;

using Dimps::Eva::Task;
using Battle::GameManager;
using Battle::IUnit;
using Battle::Sound::SoundPlayerManager;
using SoundUnit = Battle::Sound::Unit;
using Dimps::Platform::SoundObjectPool;

GameManager::__publicMethods GameManager::publicMethods;
IUnit::__publicMethods IUnit::publicMethods;
SoundPlayerManager::__publicMethods SoundPlayerManager::publicMethods;
SoundPlayerManager::__staticMethods SoundPlayerManager::staticMethods;
SoundPlayerManager::CriPlayerAdapter::__publicMethods SoundPlayerManager::CriPlayerAdapter::publicMethods;
SoundUnit::__publicMethods SoundUnit::publicMethods;

int Battle::orderedEditions[NUM_VALID_EDITIONS] = {
	Battle::ED_SF4,
	Battle::ED_SSF4,
	Battle::ED_AE2011,
	// The index-3 edition is disabled on every character.
	// It looks like it corresponds to regulation 3 which
	// might be AE2012 1.0, alluding to edition select
	// possibly originally supporting subversions.
	0,
	Battle::ED_AE2012,
	Battle::ED_USF4,
	Battle::ED_OMEGA,
};

Battle::ValidEditions* Battle::validEditionsPerChara;
void Battle::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;
	validEditionsPerChara = (ValidEditions*)(peRootOffset + 0x539ba8);

	Action::Locate(peRoot);
	Camera::Locate(peRoot);
	Chara::Locate(peRoot);
	Command::Locate(peRoot);
	GameManager::Locate(peRoot);
	Effect::Locate(peRoot);
	Hud::Locate(peRoot);
	IUnit::Locate(peRoot);
	SoundPlayerManager::Locate(peRoot);
	SoundUnit::Locate(peRoot);
	System::Locate(peRoot);
	Training::Locate(peRoot);
	Vfx::Locate(peRoot);
}

void IUnit::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.SharedHudUpdate = (PVOID)(peRootOffset + 0x18ae90);
}

void GameManager::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;
	*(PVOID*)&publicMethods.GetAgglutinateTime = (PVOID)(peRootOffset + 0x1d14f0);
	*(PVOID*)&publicMethods.GetRoundTime = (PVOID)(peRootOffset + 0x1d14d0);
	*(PVOID*)&publicMethods.GetNativeResultIndex = (PVOID)(peRootOffset + 0x1d1370);
}

Task** Battle::Pause::Unit::GetPauseTask(Unit* u) {
	return (Task**)((unsigned int)u + 0x14);
}

int* SoundPlayerManager::GetNumAdapters(SoundPlayerManager* m) {
	return (int*)((unsigned int)m + 0x8);
}

SoundPlayerManager::CriPlayerAdapter** SoundPlayerManager::GetAdapters(SoundPlayerManager* m) {
	return (SoundPlayerManager::CriPlayerAdapter**)((unsigned int)m + 0x4);
}

SoundPlayerManager::AdapterPool* SoundPlayerManager::GetAdapterPool(SoundPlayerManager* m) {
	return (AdapterPool*)((unsigned int)m + 0xc);
}

void SoundPlayerManager::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;
	*(PVOID*)&staticMethods.Factory = (PVOID)(peRootOffset + 0x194010);
	*(PVOID*)&publicMethods.destructor = (PVOID)(peRootOffset + 0x1940a0);
	*(PVOID*)&publicMethods.PlaySound = (PVOID)(peRootOffset + 0x193e70);
	*(PVOID*)&publicMethods.StopSound = (PVOID)(peRootOffset + 0x193f40);
	*(PVOID*)&publicMethods.StopAll = (PVOID)(peRootOffset + 0x193a40);
	*(PVOID*)&publicMethods.Update = (PVOID)(peRootOffset + 0x193fd0);

	CriPlayerAdapter::Locate(peRoot);
}

void SoundPlayerManager::CriPlayerAdapter::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;
	*(PVOID*)&publicMethods.IsStillPlaying = (PVOID)(peRootOffset + 0x1935b0);
}

void SoundUnit::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;
	*(PVOID*)&publicMethods.IsStillPlaying = (PVOID)(peRootOffset + 0x191dd0);
}

SoundPlayerManager** SoundUnit::GetManagerArray(SoundUnit* u) {
	return (SoundPlayerManager**)((unsigned int)u + 0xc);
}
