#include <windows.h>
#include <cstring>

#include "Dimps__Game__Battle__Command.hxx"

namespace Command = Dimps::Game::Battle::Command;
using Command::CommandImpl;
using Command::Unit;

CommandImpl::__publicMethods CommandImpl::publicMethods;
Unit::__publicMethods Unit::publicMethods;

void Command::Locate(HMODULE peRoot) {
	CommandImpl::Locate(peRoot);
	Unit::Locate(peRoot);
}

CommandImpl::ChargeState* CommandImpl::GetChargeStates(CommandImpl* impl, int& count) {
	auto* base = (unsigned char*)impl;
	count = *(const int*)(base + 0x1598);
	return *(ChargeState**)(base + 0x1594);
}

unsigned short Command::GetMoveRange(const unsigned char* move, float& distance) {
	std::memcpy(&distance, move + 0x14, sizeof distance);
	return *(const unsigned short*)(move + 4);
}

void CommandImpl::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;
	*(PVOID*)(&publicMethods.GetCurrentOnSwitches) = (PVOID)(peRootOffset + 0x19a460);
	*(PVOID*)(&publicMethods.GetCurrentRisingSwitches) = (PVOID)(peRootOffset + 0x19a470);
	*(PVOID*)(&publicMethods.GetCurrentFallingSwitches) = (PVOID)(peRootOffset + 0x19a480);
}

void Unit::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;

	*(PVOID*)&publicMethods.GetCommandImplForEntry = (PVOID)(peRootOffset + 0x199400);
	*(PVOID*)&publicMethods.RecordToInternalMementoKey = (PVOID)(peRootOffset + 0x1983e0);
	*(PVOID*)&publicMethods.RestoreFromInternalMementoKey = (PVOID)(peRootOffset + 0x198410);
}
