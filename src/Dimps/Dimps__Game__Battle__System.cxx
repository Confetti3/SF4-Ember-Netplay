#include <windows.h>

#include "Dimps__Eva.hxx"
#include "Dimps__Game.hxx"
#include "Dimps__Game__Battle__System.hxx"
#include "Dimps__Math.hxx"

using Dimps::Eva::EmRandom;
using Dimps::Game::Request;
using Dimps::Game::Battle::System;
using Dimps::Math::FixedPoint;

System::__mementoableMethods System::mementoableMethods;
System::__publicMethods System::publicMethods;
System::__staticMethods System::staticMethods;
System::__staticVars System::staticVars;

void System::Locate(HMODULE peRoot) {
    unsigned int peRootOffset = (unsigned int)peRoot;
    *(PVOID*)&mementoableMethods.GetMementoSize = (PVOID)(peRootOffset + 0x1d7b70);
    *(PVOID*)&mementoableMethods.RecordToMemento = (PVOID)(peRootOffset + 0x1d7b80);
    *(PVOID*)&mementoableMethods.RestoreFromMemento = (PVOID)(peRootOffset + 0x1d7d80);

    *(PVOID*)&publicMethods.BattleUpdate = (PVOID)(peRootOffset + 0x1d6b10);
    *(PVOID*)&publicMethods.GetCharaUnit = (PVOID)(peRootOffset + 0x163510);
    *(PVOID*)&publicMethods.GetGameManager = (PVOID)(peRootOffset + 0x1d9950);
    *(PVOID*)&publicMethods.GetGlobalTimeScale_Fixed = (PVOID)(peRootOffset + 0x1d9fe0);
    *(PVOID*)&publicMethods.GetNumCharasToSimulateThisFrame = (PVOID)(peRootOffset + 0x1d9960);
    *(PVOID*)&publicMethods.GetUnitTimeScale_Fixed = (PVOID)(peRootOffset + 0x1da060);
    *(PVOID*)&publicMethods.GetUnitByIndex = (PVOID)(peRootOffset + 0x1d9720);
    *(PVOID*)&publicMethods.SysMain_HandleTrainingModeFeatures = (PVOID)(peRootOffset + 0x1dab30);
    *(PVOID*)&publicMethods.SysMain_UpdatePauseState = (PVOID)(peRootOffset + 0x1dbaa0);
    *(PVOID*)&publicMethods.CloseBattle = (PVOID)(peRootOffset + 0x1db3f0);
    *(PVOID*)&publicMethods.IsFight = (PVOID)(peRootOffset + 0x1d9f60);
    *(PVOID*)&publicMethods.IsLeavingBattle = (PVOID)(peRootOffset + 0x1d6a70);
    *(PVOID*)&publicMethods.RecordAllToInternalMementoKeys = (PVOID)(peRootOffset + 0x1d7f80);
    *(PVOID*)&publicMethods.RestoreAllFromInternalMementoKeys = (PVOID)(peRootOffset + 0x1d8020);

    *(PVOID*)&publicMethods.RecordToInternalMementoKey = (PVOID)(peRootOffset + 0x1daab0);
    *(PVOID*)&publicMethods.RestoreFromInternalMementoKey = (PVOID)(peRootOffset + 0x1d90d0);

    *(PVOID*)&publicMethods.GetTaskCore = (PVOID)(peRootOffset + 0x1d9930);

    *(PVOID*)&publicMethods.GetGameMode = (PVOID)(peRootOffset + 0x1d97c0);

    staticMethods.GetSingleton = (System * (*)())(peRootOffset + 0x1dba30);
    staticMethods.SetBattleFlow = (void(*)(System*, int))(peRootOffset + 0x1dce90);
    staticMethods.OnBattleFlow_BattleStart = (void(*)(System*))(peRootOffset + 0x1dd350);

    staticVars.CurrentBattleFlow = (DWORD*)(peRootOffset + 0x664994);
    staticVars.PreviousBattleFlow = (DWORD*)(peRootOffset + 0x664998);
    staticVars.CurrentBattleFlowSubstate = (DWORD*)(peRootOffset + 0x66499c);
    staticVars.PreviousBattleFlowSubstate = (DWORD*)(peRootOffset + 0x6649a0);
    staticVars.BattleFlowCallback_CallEveryFrame_aa9254 = (void(**)(System*))(peRootOffset + 0x6a9254);
    staticVars.BattleFlowSubstateCallable_aa9258 = (void(**)(System*))(peRootOffset + 0x6a9258);
    staticVars.PreviousBattleFlowSubstateFrame = (FixedPoint*)(peRootOffset + 0x6a925c);
    staticVars.PreviousBattleFlowFrame = (FixedPoint*)(peRootOffset + 0x6a9260);
    staticVars.CurrentBattleFlowFrame = (FixedPoint*)(peRootOffset + 0x6a9264);
    staticVars.CurrentBattleFlowSubstateFrame = (FixedPoint*)(peRootOffset + 0x6a9268);
}

int* System::GetBattleExitType(System* s) {
    return (int*)((unsigned int)s + 0xe40);
}

int* System::GetFirstCharaToSimulate(System* s) {
    return (int*)((unsigned int)s + 0xd74);
}

FixedPoint* System::GetNumFramesSimulated_FixedPoint(System* s) {
    return (FixedPoint*)((unsigned int)s + 0xdd4);
}

int* System::GetReadyState(System* s) {
    return (int*)((unsigned int)s + 0xdf8);
}

EmRandom* System::GetRandom(System* s) {
    return (EmRandom*)((unsigned int)s + 0x50);
}

Request** System::GetRequest(System* s) {
    return (Request**)((unsigned int)s + 0x98);
}

DWORD* System::GetSkipRelatedFlags_0xd8c(System* s) {
    return (DWORD*)((unsigned int)s + 0xd8c);
}

DWORD* System::GetSimulationFlags(System* s) {
    return (DWORD*)((unsigned int)s + 0x1454);
}

int* System::GetPausingPlayer(System* s) {
    // 0x5DB7BE stores the player after Pause::Unit Open at 0x5DB799.
    // 0x5DB673 (forced close), 0x5DB8C4 (resume), and 0x5DA896
    // (CloseBattle/SYS START reset) store -1. Training hold leaves it unset.
    return (int*)((unsigned int)s + 0x1458);
}

FixedPoint* System::GetTransitionProgress(System* s) {
    return (FixedPoint*)((unsigned int)s + 0xe24);
}

FixedPoint* System::GetTransitionSpeed(System* s) {
    return (FixedPoint*)((unsigned int)s + 0xe28);
}

int* System::GetTransitionType(System* s) {
    return (int*)((unsigned int)s + 0xe20);
}

System* System::ToMementoable(System* s) {
    return (System*)((unsigned int)s + 0x4);
}

System* System::FromMementoable(System* s) {
    return (System*)((unsigned int)s - 0x4);
}
