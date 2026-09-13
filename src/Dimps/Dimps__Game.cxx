#include <windows.h>

#include "Dimps__Eva.hxx"
#include "Dimps__Game.hxx"
#include "Dimps__Game__Battle.hxx"

namespace Game = Dimps::Game;
using Game::GameMementoKey;
using Game::ProgressData;
using Game::Request;
using Game::Sprite::Control;
using Game::Sprite::SingleNodeControl;

GameMementoKey::__publicMethods GameMementoKey::publicMethods;
int* GameMementoKey::totalMementoSize;
Request::__publicMethods Request::publicMethods;
Control::__publicMethods Control::publicMethods;

void Game::Locate(HMODULE peRoot) {
	Battle::Locate(peRoot);
	Control::Locate(peRoot);
	GameMementoKey::Locate(peRoot);
	Request::Locate(peRoot);
}

void GameMementoKey::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.Initialize = (PVOID)(peRootOffset + 0x12fd40);
	*(PVOID*)&publicMethods.ClearKey = (PVOID)(peRootOffset + 0x12f3d0);
	totalMementoSize = (int*)(peRootOffset + 0x6a5840);
}

void Request::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.GetRandomSeed = (PVOID)(peRootOffset + 0x285d70);
	*(PVOID*)&publicMethods.SetIsOnlineBattle = (PVOID)(peRootOffset + 0x284620);
	*(PVOID*)&publicMethods.SetRandomSeed = (PVOID)(peRootOffset + 0x285300);
}

void Control::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.Disable_0x57bd80 = (PVOID)(peRootOffset + 0x17bd80);
	*(PVOID*)&publicMethods.Enable_0x577910 = (PVOID)(peRootOffset + 0x177910);
	*(PVOID*)&publicMethods.Enable_0x588450 = (PVOID)(peRootOffset + 0x188450);
}

DWORD* Control::GetEnabled(Control* c) {
	return (DWORD*)((unsigned int)c + 0x18);
}

ProgressData::NextBattleType* ProgressData::GetNextBattleType(ProgressData* data) {
	return (NextBattleType*)((unsigned int)data + 0xec);
}

ProgressData::BattleTypeSettings* ProgressData::GetBattleTypeSettings(ProgressData* data) {
	return (BattleTypeSettings*)((unsigned int)data + 0xf0);
}

int* SingleNodeControl::GetCurrentFrame(SingleNodeControl* c) {
	return (int*)((unsigned int)c + 0x28);
}

Dimps::Eva::IEmSpriteNode** SingleNodeControl::GetSpriteNode(SingleNodeControl* c) {
	return (Dimps::Eva::IEmSpriteNode**)((unsigned int)c + 0x1c);
}