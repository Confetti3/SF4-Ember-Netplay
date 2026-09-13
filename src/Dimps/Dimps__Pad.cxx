#include <windows.h>
#include <cstdint>

#include "Dimps__Pad.hxx"
#include "../common/ControllerInput.hxx"

namespace Pad = Dimps::Pad;
using Pad::System;
using Pad::System_RawInput;
using Pad::System_XInput;

System::__publicMethods System::publicMethods;
System::__staticMethods System::staticMethods;
System_RawInput::__publicMethods System_RawInput::publicMethods;
System_RawInput::__staticMethods System_RawInput::staticMethods;
System_XInput::__publicMethods System_XInput::publicMethods;
System_XInput::__staticMethods System_XInput::staticMethods;

const int System::BUTTON_MAPPING_FIGHT = 0;
const int System::BUTTON_MAPPING_MENU = 1;

bool Pad::ReadController(int deviceType, int deviceIndex, unsigned int& held, unsigned int* physicalButtons, unsigned int* selectPhysical, unsigned int* backPhysical) {
	held = 0;
    if (physicalButtons) *physicalButtons = 0;
    if (selectPhysical) *selectPhysical = 0;
    if (backPhysical) *backPhysical = 0;
	if (!sf4e::input::ControllerDevice{deviceType, deviceIndex}.Valid() ||
		deviceType != (deviceIndex < 4 ? PADTYPE_XINPUT : PADTYPE_DIRECTINPUT)) return false;
	auto* system = reinterpret_cast<const BYTE*>(System::staticMethods.GetSingleton());
	auto* devices = reinterpret_cast<const BYTE*>(System_XInput::staticMethods.GetSingleton());
	if (!system || !devices) return false;
	// Respect the native input-suspension gate used by 00511110/00511640
	// (e.g. an in-process overlay can block input without changing HWND focus).
	if (*reinterpret_cast<const int*>(system + 0xfc) != 0) return false;
	// USF4 Steam build 834219: 006D9120/006D7080/006D7290 are the
	// count, connection and cached-held getters of the combined pad provider.
	const int count = *reinterpret_cast<const int*>(devices + 0xd00);
	if (deviceIndex >= count || count > 12) return false;
	const BYTE* entry = devices + 0xd14 + deviceIndex * 0x100;
	if (*reinterpret_cast<const int*>(entry) != 1) return false;
	const auto* begin = *reinterpret_cast<const BYTE* const*>(system + 0xd4);
	const auto* end = *reinterpret_cast<const BYTE* const*>(system + 0xd8);
	constexpr unsigned bindingStride = 0x27c;
	if (!begin || reinterpret_cast<std::uintptr_t>(end) < reinterpret_cast<std::uintptr_t>(begin) ||
		reinterpret_cast<std::uintptr_t>(end) - reinterpret_cast<std::uintptr_t>(begin) <
			static_cast<unsigned>(deviceIndex + 1) * bindingStride) return false;
	// 00511E70 maps physical bit n through +17C in fight mode (+1FC in
	// menu mode). Reading the fight table preserves LP/LK remapping without
	// changing the game's active map, repeat timers, or device-in-use flags.
	const auto physical = *reinterpret_cast<const std::uint32_t*>(entry + 0xc);
    if (physicalButtons) *physicalButtons = physical;
	const auto* bindings = reinterpret_cast<const std::uint32_t*>(begin + deviceIndex * bindingStride + 0x17c);
	held = sf4e::input::MapButtons(physical, bindings);
    if(selectPhysical)*selectPhysical=sf4e::input::PhysicalBinding(0x10,bindings);
    if(backPhysical)*backPhysical=sf4e::input::PhysicalBinding(0x40,bindings);
	return true;
}

void Pad::Locate(HMODULE peRoot) {
	System::Locate(peRoot);
	System_RawInput::Locate(peRoot);
	System_XInput::Locate(peRoot);
}

void System::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;
    *(PVOID*)(&publicMethods.UpdateInputs) = (PVOID)(peRootOffset + 0x112180);
	*(PVOID*)(&publicMethods.GetButtons_RawOn) = (PVOID)(peRootOffset + 0x117130);
	*(PVOID*)(&publicMethods.GetButtons_RawRising) = (PVOID)(peRootOffset + 0x117150);
	*(PVOID*)(&publicMethods.GetButtons_RawFalling) = (PVOID)(peRootOffset + 0x117170);
	*(PVOID*)(&publicMethods.GetButtons_RawRisingWithRepeat) = (PVOID)(peRootOffset + 0x117190);
	*(PVOID*)(&publicMethods.GetButtons_MappedOn) = (PVOID)(peRootOffset + 0x1171b0);
	*(PVOID*)(&publicMethods.GetAllDeviceCount) = (PVOID)(peRootOffset + 0x110710);
	*(PVOID*)(&publicMethods.GetOKDeviceCount) = (PVOID)(peRootOffset + 0x110740);
	*(PVOID*)(&publicMethods.GetDeviceName) = (PVOID)(peRootOffset + 0x111cf0);
	*(PVOID*)(&publicMethods.GetDeviceIndexForPlayer) = (PVOID)(peRootOffset + 0x117240);
	*(PVOID*)(&publicMethods.GetDeviceTypeForPlayer) = (PVOID)(peRootOffset + 0x117290);
	*(PVOID*)(&publicMethods.GetAssigmentStatusForPlayer) = (PVOID)(peRootOffset + 0x1173c0);
	*(PVOID*)(&publicMethods.AssociatePlayerAndGamepad) = (PVOID)(peRootOffset + 0x117530);
	*(PVOID*)(&publicMethods.SetSideHasAssignedController) = (PVOID)(peRootOffset + 0x117360);
	*(PVOID*)(&publicMethods.SetDeviceTypeForPlayer) = (PVOID)(peRootOffset + 0x117270);
	*(PVOID*)(&publicMethods.SetActiveButtonMapping) = (PVOID)(peRootOffset + 0x110170);
	*(PVOID*)(&publicMethods.CaptureNextMatchingPadToSide) = (PVOID)(peRootOffset + 0x111110);

	staticMethods.GetSingleton = (System * (*)())(peRootOffset + 0x119480);
}

int* System::PlayerEntry::DeviceIndex(System::PlayerEntry* e) {
	return (int*)((unsigned int)e + 0x0);
}

int* System::PlayerEntry::DeviceType(System::PlayerEntry* e) {
	return (int*)((unsigned int)e + 0x44);
}

int* System::PlayerEntry::AssignedController(System::PlayerEntry* e) {
	return (int*)((unsigned int)e + 0x48);
}

void System_RawInput::Locate(HMODULE peRoot) {
	unsigned int peRootOffset = (unsigned int)peRoot;
	*(PVOID*)(&publicMethods.SetDeviceInUse) = (PVOID)(peRootOffset + 0x2e1310);
	staticMethods.GetSingleton = (System_RawInput * (*)())(peRootOffset + 0x2e00e0);
}

void System_XInput::Locate(HMODULE peRoot) {
    const auto base = reinterpret_cast<std::uintptr_t>(peRoot);
    // Steam 1.05: native controller backend includes DirectInput (indices >= 4).
    *(PVOID*)(&publicMethods.GetDeviceCount) = (PVOID)(base + 0x2d9120);
    *(PVOID*)(&publicMethods.GetDeviceStatus) = (PVOID)(base + 0x2d7080);
    *(PVOID*)(&publicMethods.GetDeviceName) = (PVOID)(base + 0x2d70b0);
    *(PVOID*)(&publicMethods.GetButtonsOn) = (PVOID)(base + 0x2d7290);
	unsigned int peRootOffset = (unsigned int)peRoot;
	*(PVOID*)(&publicMethods.SetDeviceInUse) = (PVOID)(peRootOffset + 0x2d9170);
	staticMethods.GetSingleton = (System_XInput * (*)())(peRootOffset + 0x2d90f0);
}
