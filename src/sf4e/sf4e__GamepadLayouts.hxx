#pragma once
#include "../common/ControllerView.hxx"
#include "../common/ButtonLayoutSlots.hxx"

namespace sf4e { namespace GamepadLayouts {
// Called under the controller layer's lock, on the game thread only.
void Load();
void Started(const gamepads::Inventory& inventory, bool active);
void Set(std::vector<gamepads::StoredLayout> layouts);
const std::vector<gamepads::StoredLayout>& Stored();
void Update(const gamepads::Inventory& inventory);
void Read(gamepads::PadFrame& frame, const gamepads::Inventory& inventory);
// The game's button screen or config loader rewrites the tables (GameTableSwap).
void LendGameTables(gamepads::GameTableUse use);
void ReturnGameTables(gamepads::GameTableUse use);
void Shutdown();
} }
